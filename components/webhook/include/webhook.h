/*
 * NetworkWatcher - webhook.h
 * Notifiche via webhook HTTP generico (POST JSON).
 *
 * Compatibile con: ntfy.sh, Discord webhook, Slack webhook, IFTTT,
 * Home Assistant, Gotify, server custom.
 *
 * Fase 3.4.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inizializza il componente webhook:
 *  - crea la coda messaggi (max NW_WH_QUEUE_SIZE)
 *  - avvia webhook_task (stack 8192)
 *  - legge URL e token da settings (NVS)
 *
 * Sicura da chiamare anche se il webhook non e' configurato:
 * il task resta in idle e scarta i messaggi in arrivo.
 */
esp_err_t webhook_init(void);

/**
 * @brief Accoda una notifica di scansione.
 *        Costruisce il payload JSON e lo mette in coda (non bloccante).
 */
esp_err_t webhook_notify_scan(const nw_scan_delta_t *delta);

/**
 * @brief Accoda un messaggio di test con info scheda
 *        (IP, hostname, uptime, heap).
 */
esp_err_t webhook_send_test(void);

/**
 * @brief Invia un test SINCRONO con URL e token temporanei (non salvati).
 *        Blocca fino al termine (max timeout HTTP).
 */
esp_err_t webhook_send_test_with(const char *url, const char *token);

/**
 * @brief Ricarica URL e token da settings.
 */
esp_err_t webhook_reload_config(void);

/**
 * @brief True se URL configurato (token opzionale).
 */
bool webhook_is_configured(void);

#ifdef __cplusplus
}
#endif