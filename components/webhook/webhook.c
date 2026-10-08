/*
 * NetworkWatcher - webhook.c
 * Notifications via generic HTTP webhook.
 *
 * JSON payload sent:
 * {
 *   "event": "scan",
 *   "timestamp": 1759680000,
 *   "new_unknown_count": 3,
 *   "blacklist_count": 1,
 *   "new_unknown": [ {"mac": "...", "ip": "...", "vendor": "..."}, ... ],
 *   "blacklist":   [ {"mac": "...", "ip": "...", "vendor": "..."}, ... ]
 * }
 *
 * For test: {"event":"test", "message":"...", "ip":"...", "hostname":"...", ...}
 */

#include "webhook.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_netif.h"

#include "settings.h"
#include "oui_lookup.h"
#include "mac_list.h"

static const char *TAG = "webhook";

/* ============================================================
 * RUNTIME CONFIG
 * ============================================================ */

#define WH_PAYLOAD_MAX   2048
#define WH_TASK_STACK    6144
#define WH_TASK_PRIO     5

typedef struct {
    char payload[WH_PAYLOAD_MAX];
} wh_msg_t;

static QueueHandle_t  s_queue      = NULL;
static TaskHandle_t   s_task       = NULL;
static char           s_url[NW_WH_URL_MAX_LEN + 1]     = {0};
static char           s_token[NW_WH_TOKEN_MAX_LEN + 1] = {0};
static volatile bool  s_configured = false;

/* ============================================================
 * FORWARD
 * ============================================================ */

static void      webhook_task(void *arg);
static esp_err_t wh_send_with(const char *payload,
                              const char *url,
                              const char *token);
static esp_err_t wh_send(const char *payload);
static esp_err_t wh_build_scan_payload(const nw_scan_delta_t *d,
                                       char *out, size_t out_len);
static esp_err_t wh_build_test_payload(char *out, size_t out_len);

/* ============================================================
 * INIT
 * ============================================================ */

esp_err_t webhook_init(void)
{
    if (s_task) return ESP_OK;

    s_queue = xQueueCreate(NW_WH_QUEUE_SIZE, sizeof(wh_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    webhook_reload_config();

    BaseType_t ok = xTaskCreate(
        webhook_task,
        "webhook",
        WH_TASK_STACK,
        NULL,
        WH_TASK_PRIO,
        &s_task
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate webhook failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init ok (configured=%d, stack=%d)",
             (int)s_configured, WH_TASK_STACK);
    return ESP_OK;
}

esp_err_t webhook_reload_config(void)
{
    char url[NW_WH_URL_MAX_LEN + 1]     = {0};
    char tok[NW_WH_TOKEN_MAX_LEN + 1]   = {0};

    settings_get_webhook_url(url, sizeof(url));
    settings_get_webhook_token(tok, sizeof(tok));

    if (url[0] != '\0') {
        strncpy(s_url, url, sizeof(s_url) - 1);
        s_url[sizeof(s_url) - 1] = '\0';
        strncpy(s_token, tok, sizeof(s_token) - 1);
        s_token[sizeof(s_token) - 1] = '\0';
        s_configured = true;
        ESP_LOGI(TAG, "config loaded (url len=%u, token=%s)",
                 (unsigned)strlen(s_url),
                 s_token[0] ? "yes" : "no");
    } else {
        s_url[0]   = '\0';
        s_token[0] = '\0';
        s_configured = false;
        ESP_LOGD(TAG, "webhook not configured");
    }
    return ESP_OK;
}

bool webhook_is_configured(void)
{
    return s_configured;
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

esp_err_t webhook_notify_scan(const nw_scan_delta_t *delta)
{
    if (!delta) return ESP_ERR_INVALID_ARG;
    if (!s_configured) {
        ESP_LOGD(TAG, "notify_scan ignored: not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_queue) return ESP_ERR_INVALID_STATE;

    wh_msg_t *msg = calloc(1, sizeof(wh_msg_t));
    if (!msg) {
        ESP_LOGW(TAG, "calloc failed for notify_scan");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t r = wh_build_scan_payload(delta, msg->payload, sizeof(msg->payload));
    if (r != ESP_OK) {
        free(msg);
        return r;
    }

    if (xQueueSend(s_queue, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, message discarded");
        free(msg);
        return ESP_ERR_NO_MEM;
    }
    free(msg);
    return ESP_OK;
}

esp_err_t webhook_send_test(void)
{
    if (!s_configured) {
        ESP_LOGD(TAG, "send_test ignored: not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_queue) return ESP_ERR_INVALID_STATE;

    wh_msg_t *msg = calloc(1, sizeof(wh_msg_t));
    if (!msg) {
        ESP_LOGW(TAG, "calloc failed for send_test");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t r = wh_build_test_payload(msg->payload, sizeof(msg->payload));
    if (r != ESP_OK) {
        free(msg);
        return r;
    }

    if (xQueueSend(s_queue, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, test discarded");
        free(msg);
        return ESP_ERR_NO_MEM;
    }
    free(msg);
    return ESP_OK;
}

esp_err_t webhook_send_test_with(const char *url, const char *token)
{
    if (!url) return ESP_ERR_INVALID_ARG;
    if (url[0] == '\0') return ESP_ERR_INVALID_ARG;

    char payload[WH_PAYLOAD_MAX];
    esp_err_t r = wh_build_test_payload(payload, sizeof(payload));
    if (r != ESP_OK) return r;

    ESP_LOGI(TAG, "synchronous test send with temporary URL");
    return wh_send_with(payload, url, token ? token : "");
}

/* ============================================================
 * TASK
 * ============================================================ */

static void webhook_task(void *arg)
{
    (void)arg;
    wh_msg_t msg;

    ESP_LOGI(TAG, "task started (stack=%d)", WH_TASK_STACK);

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (!s_configured) {
            ESP_LOGD(TAG, "msg discarded: not configured");
            continue;
        }

        esp_err_t r = ESP_FAIL;
        for (int attempt = 1; attempt <= NW_WH_MAX_RETRY; ++attempt) {
            r = wh_send(msg.payload);
            if (r == ESP_OK) break;

            int delay_ms = 2000 * (1 << (attempt - 1));
            ESP_LOGW(TAG, "send failed (attempt %d/%d, err=%s), retry in %d ms",
                     attempt, NW_WH_MAX_RETRY, esp_err_to_name(r), delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        if (r == ESP_OK) {
            ESP_LOGI(TAG, "message sent");
        } else {
            ESP_LOGE(TAG, "send failed after %d attempts", NW_WH_MAX_RETRY);
        }
    }
}

/* ============================================================
 * HTTP SEND
 *
 * All large buffers are allocated on the HEAP.
 * The stack (6144) is sized for TLS handshake.
 * ============================================================ */

static esp_err_t wh_send_with(const char *payload,
                              const char *url,
                              const char *token)
{
    if (!payload || !url) return ESP_ERR_INVALID_ARG;
    if (url[0] == '\0') return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = NW_WH_HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "User-Agent", "NetworkWatcher/0.1");

    if (token && token[0] != '\0') {
        char auth_hdr[NW_WH_TOKEN_MAX_LEN + 16];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
        esp_http_client_set_header(cli, "Authorization", auth_hdr);
    }

    esp_http_client_set_post_field(cli, payload, strlen(payload));

    esp_err_t r = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);

    if (r == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGD(TAG, "HTTP %d OK", status);
    } else {
        ESP_LOGW(TAG, "HTTP status=%d err=%s", status, esp_err_to_name(r));
        r = ESP_FAIL;
    }

    esp_http_client_cleanup(cli);
    return r;
}

static esp_err_t wh_send(const char *payload)
{
    return wh_send_with(payload, s_url, s_token);
}

/* ============================================================
 * JSON PAYLOAD FORMATTING
 * ============================================================ */

static esp_err_t wh_build_scan_payload(const nw_scan_delta_t *d,
                                       char *out, size_t out_len)
{
    if (!d || !out) return ESP_ERR_INVALID_ARG;

    int64_t now = (int64_t)time(NULL);
    int n = snprintf(out, out_len,
                     "{\"event\":\"scan\",\"timestamp\":%lld,"
                     "\"new_unknown_count\":%d,\"blacklist_count\":%d,",
                     (long long)now,
                     d->new_unknown_count, d->blacklist_count);

    n += snprintf(out + n, out_len - n, "\"new_unknown\":[");
    for (int i = 0; i < d->new_unknown_count && n < (int)out_len - 100; ++i) {
        const char *vendor = NULL;
        uint8_t mac[6];
        if (string_to_mac(d->new_unknown[i].mac, mac)) {
            vendor = oui_lookup_vendor(mac);
        }
        n += snprintf(out + n, out_len - n,
                      "%s{\"mac\":\"%s\",\"ip\":\"%s\",\"vendor\":\"%s\"}",
                      i > 0 ? "," : "",
                      d->new_unknown[i].mac,
                      d->new_unknown[i].ip,
                      vendor ? vendor : "");
    }
    n += snprintf(out + n, out_len - n, "],");

    n += snprintf(out + n, out_len - n, "\"blacklist\":[");
    for (int i = 0; i < d->blacklist_count && n < (int)out_len - 100; ++i) {
        const char *vendor = NULL;
        uint8_t mac[6];
        if (string_to_mac(d->blacklist[i].mac, mac)) {
            vendor = oui_lookup_vendor(mac);
        }
        n += snprintf(out + n, out_len - n,
                      "%s{\"mac\":\"%s\",\"ip\":\"%s\",\"vendor\":\"%s\"}",
                      i > 0 ? "," : "",
                      d->blacklist[i].mac,
                      d->blacklist[i].ip,
                      vendor ? vendor : "");
    }
    n += snprintf(out + n, out_len - n, "]}");

    return ESP_OK;
}

static esp_err_t wh_build_test_payload(char *out, size_t out_len)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    char ip[16] = "n/a";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }

    uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t heap   = (uint32_t)esp_get_free_heap_size();
    int64_t  now    = (int64_t)time(NULL);

    snprintf(out, out_len,
             "{\"event\":\"test\","
             "\"timestamp\":%lld,"
             "\"message\":\"Test NetworkWatcher\","
             "\"ip\":\"%s\","
             "\"hostname\":\"network-watcher.local\","
             "\"uptime\":%lu,"
             "\"heap\":%lu}",
             (long long)now,
             ip,
             (unsigned long)uptime,
             (unsigned long)heap);
    return ESP_OK;
}