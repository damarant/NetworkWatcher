/*
 * NetworkWatcher - button.h
 * Gestione del pulsante BOOT/0 (GPIO0, attivo basso).
 * Rileva: click breve, pressione lunga (5s), sequenza 3 click + conferma.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * EVENTI DEL PULSANTE
 * ============================================================ */

typedef enum {
    BTN_EVENT_NONE = 0,

    /* Click breve (< 1 sec): evento generico */
    BTN_EVENT_SHORT_PRESS,

    /* Pressione lunga (5 sec): entra in modalita' setup */
    BTN_EVENT_LONG_PRESS,

    /* Sequenza di 3 click rapidi completata: attesa conferma */
    BTN_EVENT_SEQUENCE_3,

    /* Conferma reset (4o click entro 5 sec): reset di fabbrica */
    BTN_EVENT_SEQUENCE_CONFIRMED,

    /* Timeout conferma: sequenza annullata */
    BTN_EVENT_SEQUENCE_CANCELLED,
} btn_event_t;

/* ============================================================
 * CALLBACK
 * ============================================================ */

/**
 * @brief Callback chiamata quando si verifica un evento del pulsante.
 *        Viene invocata dal task del pulsante: non bloccare.
 */
typedef void (*btn_callback_t)(btn_event_t event);

/* ============================================================
 * INIZIALIZZAZIONE
 * ============================================================ */

/**
 * @brief Inizializza il GPIO del pulsante e avvia il task di rilevamento.
 *
 * @param callback  Funzione chiamata ad ogni evento (puo' essere NULL)
 * @return ESP_OK in caso di successo
 */
esp_err_t button_init(btn_callback_t callback);

/**
 * @brief Restituisce true se il pulsante e' attualmente premuto.
 */
bool button_is_pressed(void);

#ifdef __cplusplus
}
#endif