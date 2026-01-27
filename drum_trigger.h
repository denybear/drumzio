
// drum_trigger.h
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------
// Types de frappe détectés
// ----------------------------
typedef enum {
	DRUM_HIT_NONE = 0,
	DRUM_HIT_HEAD = 1u << 0,		  // peau
	DRUM_HIT_RIM  = 1u << 1,		  // bord
	DRUM_HIT_BOTH = (DRUM_HIT_HEAD | DRUM_HIT_RIM)  // rimshot / frappe simultanée
} drum_hit_kind_t;

// ----------------------------
// Evènement renvoyé (1 seul par frappe)
// ----------------------------
typedef struct {
	drum_hit_kind_t kind;
	uint16_t peak_head;   // pic max mesuré sur ADC0 (0..4095)
	uint16_t peak_rim;	// pic max mesuré sur ADC1 (0..4095)
	uint32_t t_ms;		// timestamp (ms) de l'évènement
} drum_hit_t;

// ----------------------------
// Configuration (tous paramètres ajustables)
// ----------------------------
typedef struct {
	// Hystérésis: seuil haut = début frappe, seuil bas = fin frappe
	uint16_t th_high_head;
	uint16_t th_low_head;
	uint16_t th_high_rim;
	uint16_t th_low_rim;

	// Timing (ms)
	uint32_t scan_min_ms;   // durée minimale d'observation avant autoriser fin de groupe
	uint32_t release_ms;	// durée sous seuil bas (toutes voies) pour considérer "fin"
	uint32_t max_group_ms;  // sécurité: termine un groupe trop long

	// Retrigger / mask time (ms) par zone (empêche double hit)
	uint32_t retrigger_head_ms;
	uint32_t retrigger_rim_ms;

	// Décision BOTH vs dominant :
	// both_ratio_q15 = ratio max/min en Q15 au-dessous duquel on déclare BOTH
	// ex 1.5 => both_ratio_q15 = 1.5 * 32768
	uint32_t both_ratio_q15;

	// Exige un minimum sur la voie secondaire pour déclarer BOTH
	uint16_t min_secondary_for_both;
} drum_trigger_cfg_t;

// ----------------------------
// Etat interne (doit être conservé entre appels)
// ----------------------------
typedef struct {
	bool group_active;
	uint32_t group_start_ms;

	uint16_t peak_head;
	uint16_t peak_rim;

	uint32_t last_above_low_head_ms;
	uint32_t last_above_low_rim_ms;

	uint32_t last_hit_head_ms;
	uint32_t last_hit_rim_ms;

	bool seen_high_head;
	bool seen_high_rim;
} drum_trigger_state_t;

// ----------------------------
// Helpers
// ----------------------------
static inline void drum_trigger_init(drum_trigger_state_t *st) {
	*st = (drum_trigger_state_t){0};
}

static inline uint32_t drum_q15_from_float(float x) {
	// utilitaire pratique pour initialiser both_ratio_q15
	// (pas utilisé dans la fonction runtime)
	return (uint32_t)(x * 32768.0f);
}

// ----------------------------
// API principale
// - appelle-la à chaque sample (ou boucle) avec les valeurs ADC 12 bits
// - renvoie DRUM_HIT_NONE tant qu'une frappe n'est pas "terminée"
// - renvoie 1 évènement unique quand la frappe est finie (peau/rim/both)
// ----------------------------
drum_hit_t drum_trigger_update(drum_trigger_state_t *st,
							   const drum_trigger_cfg_t *cfg,
							   uint16_t adc_head, uint16_t adc_rim,
							   uint32_t now_ms);

#ifdef __cplusplus
}
#endif
