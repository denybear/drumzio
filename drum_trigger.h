#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    DRUM_HIT_NONE = 0,
    DRUM_HIT_HEAD = 1,
    DRUM_HIT_RIM  = 2,
    DRUM_HIT_BUTTON = 4
} drum_hit_kind_t;

typedef struct {
    drum_hit_kind_t kind;
    uint16_t p_h;    // peak_head -> p_h (plus court pour le JSON)
    uint16_t p_r;    // peak_rim  -> p_r
    uint32_t t_us;   // Timestamp en microsecondes
} drum_hit_t;

typedef struct {
    uint16_t th_high_head, th_low_head;
    uint16_t th_high_rim,  th_low_rim;
    uint32_t scan_min_us;      // Fenêtre de capture du pic
    uint32_t retrigger_us;     // Temps mort anti-rebond
    uint32_t crosstalk_min_us; // Fenêtre durant laquelle on ignore l'autre zone
} drum_trigger_cfg_t;

typedef struct {
    bool group_active;
    uint32_t group_start_us;
    uint16_t peak_head, peak_rim;
    uint32_t last_hit_head_us, last_hit_rim_us;
    bool head_was_active, rim_was_active;
} drum_trigger_state_t;

void drum_trigger_init(drum_trigger_state_t *st);
drum_hit_t drum_trigger_update(drum_trigger_state_t *st, const drum_trigger_cfg_t *cfg, uint16_t adc_head, uint16_t adc_rim, uint32_t now_us);