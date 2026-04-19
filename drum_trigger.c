#include "drum_trigger.h"

void drum_trigger_init(drum_trigger_state_t *st) {
    if (st) {
        *st = (drum_trigger_state_t){0};
    }
}

drum_hit_t drum_trigger_update(drum_trigger_state_t *st, const drum_trigger_cfg_t *cfg, 
                               uint16_t adc_head, uint16_t adc_rim, uint32_t now_us) {
    drum_hit_t out = { .kind = DRUM_HIT_NONE, .peak_head = 0, .peak_rim = 0, .t_us = now_us };

    // 1. ÉTAT INACTIF
    if (!st->group_active) {
        // Vérification du retrigger (temps mort après la dernière frappe validée)
        bool head_ready = (now_us - st->last_hit_head_us >= cfg->retrigger_us);
        bool rim_ready  = (now_us - st->last_hit_rim_us >= cfg->retrigger_us);

        // Détection du dépassement de seuil
        bool head_trig = (adc_head >= cfg->th_high_head) && head_ready;
        bool rim_trig  = (adc_rim  >= cfg->th_high_rim) && rim_ready;

        if (head_trig || rim_trig) {
            st->group_active = true;
            st->group_start_us = now_us;
            st->peak_head = adc_head;
            st->peak_rim  = adc_rim;
            st->head_was_active = head_trig;
            st->rim_was_active  = rim_trig;
        }
        return out;
    }

    // 2. ÉTAT ACTIF : CAPTURE DES PICS
    if (adc_head > st->peak_head) st->peak_head = adc_head;
    if (adc_rim  > st->peak_rim)  st->peak_rim  = adc_rim;

    uint32_t duration = now_us - st->group_start_us;
    
    // On force la fin dès qu'on dépasse scan_min_us (pas besoin d'attendre le release pour un jeu de rythme)
    if (duration >= cfg->scan_min_us) {
        
        // --- LOGIQUE D'EXCLUSION (CROSSTALK) ---
        // Le plus fort gagne. On ajoute une petite marge de priorité au Head.
        if (st->peak_head >= (st->peak_rim - 100)) {
            out.kind = DRUM_HIT_HEAD;
            st->last_hit_head_us = now_us;
            // Verrouillage agressif du Rim pour éviter que la vibration résiduelle ne le déclenche
            st->last_hit_rim_us = now_us + cfg->crosstalk_min_us; 
        } else {
            out.kind = DRUM_HIT_RIM;
            st->last_hit_rim_us = now_us;
            // Verrouillage agressif du Head
            st->last_hit_head_us = now_us + cfg->crosstalk_min_us;
        }

        out.peak_head = st->peak_head;
        out.peak_rim  = st->peak_rim;
        
        st->group_active = false;
        st->peak_head = 0;
        st->peak_rim = 0;
    }

    return out;
}