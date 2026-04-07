// drum_trigger.c
#include "drum_trigger.h"

static inline bool elapsed(uint32_t now, uint32_t since, uint32_t dt) {
	return (uint32_t)(now - since) >= dt; // ok si overflow uint32
}

static inline uint32_t ratio_q15(uint16_t maxv, uint16_t minv) {
	if (minv == 0) return 0xFFFFFFFFu;
	// (max/min) en Q15
	return ((uint32_t)maxv << 15) / (uint32_t)minv;
}

static inline uint16_t u16_max(uint16_t a, uint16_t b) { return a > b ? a : b; }
static inline uint16_t u16_min(uint16_t a, uint16_t b) { return a < b ? a : b; }
static inline uint32_t u32_max(uint32_t a, uint32_t b) { return a > b ? a : b; }
static inline uint32_t u32_min(uint32_t a, uint32_t b) { return a < b ? a : b; }


drum_hit_t drum_trigger_update(drum_trigger_state_t *st,
							   const drum_trigger_cfg_t *cfg,
							   uint16_t adc_head, uint16_t adc_rim,
							   uint32_t now_ms)
{
	drum_hit_t out = { .kind = DRUM_HIT_NONE, .peak_head = 0, .peak_rim = 0, .t_ms = now_ms };

	// --- 1) Démarrage du groupe si inactif ---
	if (!st->group_active) {
		bool head_ready = elapsed(now_ms, st->last_hit_head_ms, cfg->retrigger_head_ms);
		bool rim_ready  = elapsed(now_ms, st->last_hit_rim_ms,  cfg->retrigger_rim_ms);

		bool head_start = head_ready && (adc_head >= cfg->th_high_head);
		bool rim_start  = rim_ready  && (adc_rim  >= cfg->th_high_rim);

		if (head_start || rim_start) {
			st->group_active = true;
			st->group_start_ms = now_ms;

			st->peak_head = adc_head;
			st->peak_rim  = adc_rim;

			st->seen_high_head = head_start || (adc_head >= cfg->th_high_head);
			st->seen_high_rim  = rim_start  || (adc_rim  >= cfg->th_high_rim);

			// init last_above_low_*: si déjà au-dessus du low, on met now, sinon on met start
			st->last_above_low_head_ms = (adc_head >= cfg->th_low_head) ? now_ms : now_ms;
			st->last_above_low_rim_ms  = (adc_rim  >= cfg->th_low_rim ) ? now_ms : now_ms;
		}
		return out; // pas d'event au démarrage
	}

	// --- 2) Groupe actif : mise à jour des pics et des timers ---
	st->peak_head = u16_max(st->peak_head, adc_head);
	st->peak_rim  = u16_max(st->peak_rim,  adc_rim);

	if (adc_head >= cfg->th_high_head) st->seen_high_head = true;
	if (adc_rim  >= cfg->th_high_rim ) st->seen_high_rim  = true;

	if (adc_head >= cfg->th_low_head) st->last_above_low_head_ms = now_ms;
	if (adc_rim  >= cfg->th_low_rim ) st->last_above_low_rim_ms  = now_ms;

	uint32_t last_above_low_ms = u32_max(st->last_above_low_head_ms, st->last_above_low_rim_ms);

	bool min_scan_ok = elapsed(now_ms, st->group_start_ms, cfg->scan_min_ms);
	bool released	= elapsed(now_ms, last_above_low_ms, cfg->release_ms);
	bool timeout	 = elapsed(now_ms, st->group_start_ms, cfg->max_group_ms);

	// --- 3) Fin de groupe => décision et émission d'un event unique ---
	if ((min_scan_ok && released) || timeout) {

		// pics finaux
		uint16_t ph = st->peak_head;
		uint16_t pr = st->peak_rim;

		bool head_hit = st->seen_high_head && (ph >= cfg->th_high_head);
		bool rim_hit  = st->seen_high_rim  && (pr >= cfg->th_high_rim);

		// classification
		if (!head_hit && !rim_hit) {
			out.kind = DRUM_HIT_NONE;
		} else if (head_hit && !rim_hit) {
			out.kind = DRUM_HIT_HEAD;
		} else if (!head_hit && rim_hit) {
			out.kind = DRUM_HIT_RIM;
		} else { // head_hit && rim_hit
			// Both exceeded high thresholds: check if they're balanced enough to be BOTH
			// Simple rule: if both are strong (>= th_high) and neither is more than 2x stronger, it's BOTH
			uint16_t maxv = (ph >= pr) ? ph : pr;
			uint16_t minv = (ph >= pr) ? pr : ph;
			
			// Ratio 2.0 = allow one channel to be at most 2x stronger than the other
			bool balanced_enough = (maxv <= minv * 2);
			
			if (balanced_enough) {
				out.kind = DRUM_HIT_BOTH;
			} else {
				// One channel much stronger: classify as the dominant one
				out.kind = (ph >= pr) ? DRUM_HIT_HEAD : DRUM_HIT_RIM;
			}
		}

		out.peak_head = ph;
		out.peak_rim  = pr;

		// Retrigger/Mask : on verrouille seulement les zones réellement “déclarées”
		if (out.kind & DRUM_HIT_HEAD) st->last_hit_head_ms = now_ms;
		if (out.kind & DRUM_HIT_RIM)  st->last_hit_rim_ms  = now_ms;

		// reset groupe
		st->group_active = false;
		st->seen_high_head = st->seen_high_rim = false;
		st->peak_head = st->peak_rim = 0;

		return out;
	}

	return out; // groupe en cours, pas d'event
}


