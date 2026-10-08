/*
 * NetworkWatcher - telegram.h
 * Notifiche Telegram via Bot API.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inizializza il componente Telegram.
 *        Sicura da chiamare anche se Telegram non e' configurato.
 */
esp_err_t telegram_init(void);

/**
 * @brief Accoda una notifica aggregata di scansione.
 */
esp_err_t telegram_notify_scan(const nw_scan_delta_t *delta);

/**
 * @brief Accoda un messaggio di test con info scheda
 *        (usa token/chat_id configurati in NVS).
 */
esp_err_t telegram_send_test(void);

/**
 * @brief Invia un messaggio di test usando credenziali TEMPORANEE
 *        (NON salvate in NVS). Bloccante: puo' durare fino al timeout
 *        HTTP (10 sec). Da usare solo dall'handler HTTP del pulsante Test.
 *
 * @param token    Bot token (non vuoto)
 * @param chat_id  Chat ID (non vuoto)
 * @return ESP_OK se il messaggio e' stato inviato con successo.
 */
esp_err_t telegram_send_test_with(const char *token, const char *chat_id);

/**
 * @brief Ricarica token/chat_id da settings.
 */
esp_err_t telegram_reload_config(void);

/**
 * @brief True se token + chat_id sono entrambi presenti e non vuoti.
 */
bool telegram_is_configured(void);

#ifdef __cplusplus
}
#endif