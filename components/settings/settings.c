/*
 * NetworkWatcher - settings.c
 * Application settings management (scan interval, Telegram, Webhook).
 */

#include "settings.h"
#include "config.h"
#include "storage.h"
#include "arp_scanner.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "settings";

/* ============================================================
 * INTERNAL STATE
 * ============================================================ */

static int32_t s_scan_interval = NW_SCAN_INTERVAL_DEFAULT_MIN;
static bool    s_initialized = false;

/* NVS keys */
#define NVS_KEY_SCAN_INTERVAL   "scan_interval"
#define NVS_KEY_TG_TOKEN        "telegram_tok"
#define NVS_KEY_TG_CHATID       "telegram_id"
#define NVS_KEY_WH_URL          "webhook_url"
#define NVS_KEY_WH_TOKEN        "webhook_tok"

/* ============================================================
 * HELPER
 * ============================================================ */

static int32_t clamp_interval(int32_t m)
{
    if (m < NW_SCAN_INTERVAL_MIN_MIN) return NW_SCAN_INTERVAL_MIN_MIN;
    if (m > NW_SCAN_INTERVAL_MAX_MIN) return NW_SCAN_INTERVAL_MAX_MIN;
    return m;
}

/* ============================================================
 * SCAN INTERVAL
 * ============================================================ */

int32_t settings_get_scan_interval(void)
{
    return s_scan_interval;
}

esp_err_t settings_set_scan_interval(int32_t minutes)
{
    int32_t clamped = clamp_interval(minutes);
    if (clamped != minutes) {
        ESP_LOGW(TAG, "interval %d clamped to %d (min=%d, max=%d)",
                 (int)minutes, (int)clamped,
                 NW_SCAN_INTERVAL_MIN_MIN, NW_SCAN_INTERVAL_MAX_MIN);
    }

    s_scan_interval = clamped;

    esp_err_t ret = storage_set_int(NVS_KEY_SCAN_INTERVAL, clamped);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "saving interval failed: %s", esp_err_to_name(ret));
        return ret;
    }

    arp_scanner_set_interval(clamped);

    ESP_LOGI(TAG, "scan interval set to %d min", (int)clamped);
    return ESP_OK;
}

/* ============================================================
 * TELEGRAM
 * ============================================================ */

esp_err_t settings_get_telegram_token(char *out, size_t len)
{
    if (!out || len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    esp_err_t ret = storage_get_string(NVS_KEY_TG_TOKEN, out, len);
    if (ret != ESP_OK) out[0] = '\0';
    return ESP_OK;
}

esp_err_t settings_set_telegram_token(const char *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    if (strlen(token) > NW_TG_TOKEN_MAX_LEN) {
        ESP_LOGW(TAG, "telegram token too long (max %d)", NW_TG_TOKEN_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = storage_set_string(NVS_KEY_TG_TOKEN, token);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "saving telegram token failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "telegram token saved (%u chars)", (unsigned)strlen(token));
    return ESP_OK;
}

esp_err_t settings_get_telegram_chatid(char *out, size_t len)
{
    if (!out || len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    esp_err_t ret = storage_get_string(NVS_KEY_TG_CHATID, out, len);
    if (ret != ESP_OK) out[0] = '\0';
    return ESP_OK;
}

esp_err_t settings_set_telegram_chatid(const char *chatid)
{
    if (!chatid) return ESP_ERR_INVALID_ARG;
    if (strlen(chatid) > NW_TG_CHATID_MAX_LEN) {
        ESP_LOGW(TAG, "telegram chatid too long (max %d)", NW_TG_CHATID_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = storage_set_string(NVS_KEY_TG_CHATID, chatid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "saving telegram chatid failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "telegram chatid saved (%u chars)", (unsigned)strlen(chatid));
    return ESP_OK;
}

bool settings_telegram_is_configured(void)
{
    char token[NW_TG_TOKEN_MAX_LEN + 1] = {0};
    char chatid[NW_TG_CHATID_MAX_LEN + 1] = {0};

    settings_get_telegram_token(token, sizeof(token));
    settings_get_telegram_chatid(chatid, sizeof(chatid));

    return (token[0] != '\0' && chatid[0] != '\0');
}

/* ============================================================
 * WEBHOOK
 * ============================================================ */

esp_err_t settings_get_webhook_url(char *out, size_t len)
{
    if (!out || len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    esp_err_t ret = storage_get_string(NVS_KEY_WH_URL, out, len);
    if (ret != ESP_OK) out[0] = '\0';
    return ESP_OK;
}

esp_err_t settings_set_webhook_url(const char *url)
{
    if (!url) return ESP_ERR_INVALID_ARG;
    if (strlen(url) > NW_WH_URL_MAX_LEN) {
        ESP_LOGW(TAG, "webhook url too long (max %d)", NW_WH_URL_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    /* Accept only http:// or https:// */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        if (url[0] != '\0') {
            ESP_LOGW(TAG, "webhook url must start with http:// or https://");
            return ESP_ERR_INVALID_ARG;
        }
    }
    esp_err_t ret = storage_set_string(NVS_KEY_WH_URL, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "saving webhook url failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "webhook url saved (%u chars)", (unsigned)strlen(url));
    return ESP_OK;
}

esp_err_t settings_get_webhook_token(char *out, size_t len)
{
    if (!out || len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    esp_err_t ret = storage_get_string(NVS_KEY_WH_TOKEN, out, len);
    if (ret != ESP_OK) out[0] = '\0';
    return ESP_OK;
}

esp_err_t settings_set_webhook_token(const char *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    if (strlen(token) > NW_WH_TOKEN_MAX_LEN) {
        ESP_LOGW(TAG, "webhook token too long (max %d)", NW_WH_TOKEN_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = storage_set_string(NVS_KEY_WH_TOKEN, token);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "saving webhook token failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "webhook token saved (%u chars)", (unsigned)strlen(token));
    return ESP_OK;
}

bool settings_webhook_is_configured(void)
{
    char url[NW_WH_URL_MAX_LEN + 1] = {0};
    settings_get_webhook_url(url, sizeof(url));
    return (url[0] != '\0');
    /* Token is optional: URL presence is enough */
}

/* ============================================================
 * INIT
 * ============================================================ */

esp_err_t settings_init(void)
{
    if (s_initialized) return ESP_OK;

    int32_t saved = NW_SCAN_INTERVAL_DEFAULT_MIN;
    esp_err_t ret = storage_get_int(NVS_KEY_SCAN_INTERVAL, &saved,
                                    NW_SCAN_INTERVAL_DEFAULT_MIN);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "reading interval failed, using default %d",
                 NW_SCAN_INTERVAL_DEFAULT_MIN);
        saved = NW_SCAN_INTERVAL_DEFAULT_MIN;
    }

    s_scan_interval = clamp_interval(saved);
    arp_scanner_set_interval(s_scan_interval);

    ESP_LOGI(TAG, "settings initialized (interval=%d min, telegram=%s, webhook=%s)",
             (int)s_scan_interval,
             settings_telegram_is_configured() ? "configured" : "not configured",
             settings_webhook_is_configured()  ? "configured" : "not configured");

    s_initialized = true;
    return ESP_OK;
}