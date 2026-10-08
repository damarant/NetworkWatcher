/*
 * NetworkWatcher - settings.h
 * Gestione impostazioni applicazione (intervallo scansione, Telegram, Webhook).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * INTERVALLO SCANSIONE
 * ============================================================ */

int32_t settings_get_scan_interval(void);
esp_err_t settings_set_scan_interval(int32_t minutes);

/* ============================================================
 * TELEGRAM
 * ============================================================ */

esp_err_t settings_get_telegram_token(char *out, size_t len);
esp_err_t settings_set_telegram_token(const char *token);
esp_err_t settings_get_telegram_chatid(char *out, size_t len);
esp_err_t settings_set_telegram_chatid(const char *chatid);
bool settings_telegram_is_configured(void);

/* ============================================================
 * WEBHOOK (Fase 3.4)
 * ============================================================ */

esp_err_t settings_get_webhook_url(char *out, size_t len);
esp_err_t settings_set_webhook_url(const char *url);
esp_err_t settings_get_webhook_token(char *out, size_t len);
esp_err_t settings_set_webhook_token(const char *token);
bool settings_webhook_is_configured(void);

/* ============================================================
 * INIT
 * ============================================================ */

esp_err_t settings_init(void);

#ifdef __cplusplus
}
#endif