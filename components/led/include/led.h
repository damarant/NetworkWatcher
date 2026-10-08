/*
 * NetworkWatcher - led.h
 * Controllo del LED mono integrato con pattern di lampeggio.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * PATTERN DEL LED
 * ============================================================ */

typedef enum {
    LED_PATTERN_OFF = 0,      /* Spento */
    LED_PATTERN_ON,           /* Acceso fisso */
    LED_PATTERN_BLINK_SLOW,   /* Lampeggio lento (1 Hz) - stato normale */
    LED_PATTERN_BLINK_FAST,   /* Lampeggio veloce (5 Hz) - attenzione */
    LED_PATTERN_HEARTBEAT,    /* Doppio lampeggio breve - sistema vivo */
    LED_PATTERN_ERROR,        /* 3 lampeggi brevi + pausa - errore */
    LED_PATTERN_SCAN,         /* Lampeggio brevissimo - scansione in corso */
} led_pattern_t;

/* ============================================================
 * INIZIALIZZAZIONE
 * ============================================================ */

/**
 * @brief Inizializza il GPIO del LED e avvia il task di gestione pattern.
 *        Da chiamare una sola volta all'avvio.
 *
 * @return ESP_OK in caso di successo
 */
esp_err_t led_init(void);

/* ============================================================
 * CONTROLLO
 * ============================================================ */

/**
 * @brief Imposta il pattern del LED.
 *
 * @param pattern  Pattern desiderato
 */
void led_set_pattern(led_pattern_t pattern);

/**
 * @brief Restituisce il pattern attualmente attivo.
 */
led_pattern_t led_get_pattern(void);

/**
 * @brief Accende il LED in modo fisso.
 *        Equivalente a led_set_pattern(LED_PATTERN_ON).
 */
void led_on(void);

/**
 * @brief Spegne il LED.
 *        Equivalente a led_set_pattern(LED_PATTERN_OFF).
 */
void led_off(void);

/**
 * @brief Esegue un singolo impulso breve del LED, poi torna al pattern precedente.
 *        Utile per feedback visivo immediato (es. pressione pulsante).
 */
void led_pulse(void);

#ifdef __cplusplus
}
#endif
