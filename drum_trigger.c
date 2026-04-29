#include "drum_trigger.h"

void drum_trigger_init(drum_trigger_state_t *st) {
    if (st) {
        *st = (drum_trigger_state_t){0};
    }
}

drum_hit_t drum_trigger_update(drum_trigger_state_t *st, const drum_trigger_cfg_t *cfg, 
                               uint16_t adc_head, uint16_t adc_rim, uint32_t now_us) {
    drum_hit_t out = { .kind = DRUM_HIT_NONE, .p_h = 0, .p_r = 0, .t_us = now_us };

    // 1. ÉTAT INACTIF : On cherche un début d'onde
    if (!st->group_active) {
        bool head_ready = (now_us - st->last_hit_head_us >= cfg->retrigger_us);
        bool rim_ready  = (now_us - st->last_hit_rim_us >= cfg->retrigger_us);

        // ASTUCE : On commence le groupe dès qu'on dépasse th_low au lieu de th_high
        // Cela permet de capturer toute la montée des coups doux.
        if ((adc_head >= cfg->th_low_head && head_ready) || 
            (adc_rim >= cfg->th_low_rim && rim_ready)) {
            
            st->group_active = true;
            st->group_start_us = now_us;
            st->peak_head = adc_head;
            st->peak_rim  = adc_rim;
        }
        return out;
    }

    // 2. ÉTAT ACTIF : On accumule le maximum
    if (adc_head > st->peak_head) st->peak_head = adc_head;
    if (adc_rim  > st->peak_rim)  st->peak_rim  = adc_rim;

    // 3. FIN DU SCAN : On décide si c'est une frappe valide
    if (now_us - st->group_start_us >= cfg->scan_min_us) {
        
        // VALIDATION : Pour être un hit, il faut qu'AU MOINS UN peak ait atteint th_high
        bool valid_head = (st->peak_head >= cfg->th_high_head);
        bool valid_rim  = (st->peak_rim >= cfg->th_high_rim);

        if (valid_head || valid_rim) {
            // Priorité au RIM (ton choix précédent)
            // On ne choisit le HEAD que s'il est bcp plus fort, sinon c'est RIM par défaut
            if (valid_head && (st->peak_head > (st->peak_rim + 100))) {
                out.kind = DRUM_HIT_HEAD;
                st->last_hit_head_us = now_us;
                st->last_hit_rim_us = now_us + cfg->crosstalk_min_us;
            } else if (valid_rim) {
                out.kind = DRUM_HIT_RIM;
                st->last_hit_rim_us = now_us;
                st->last_hit_head_us = now_us + cfg->crosstalk_min_us;
            }
            out.p_h = st->peak_head;
            out.p_r = st->peak_rim;
        }

        // Reset pour la suite
        st->group_active = false;
        st->peak_head = 0;
        st->peak_rim = 0;
    }

    return out;
}
