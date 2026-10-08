/*
 * NetworkWatcher - telegram.c
 * Telegram notifications via HTTPS (crt_bundle).
 *
 * STACK NOTE: the telegram task has 8192 bytes of stack (not 5120)
 * because esp_http_client_perform() with mbedTLS TLS handshake uses
 * a lot of stack. Also the escaped/payload buffers are allocated
 * on the heap to avoid consuming additional stack.
 */

#include "telegram.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

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

static const char *TAG = "telegram";

/* ============================================================
 * RUNTIME CONFIG
 * ============================================================ */

#define TG_MSG_MAX       2048
#define TG_TASK_STACK    6144
#define TG_TASK_PRIO     5

typedef struct {
    char text[TG_MSG_MAX];
} tg_msg_t;

static QueueHandle_t  s_queue      = NULL;
static TaskHandle_t   s_task       = NULL;
static char           s_token[NW_TG_TOKEN_MAX_LEN + 1]  = {0};
static char           s_chat_id[NW_TG_CHATID_MAX_LEN + 1] = {0};
static volatile bool  s_configured = false;

/* ============================================================
 * FORWARD
 * ============================================================ */

static void      telegram_task(void *arg);
static esp_err_t tg_send_with(const char *text,
                              const char *token,
                              const char *chat_id);
static esp_err_t tg_send(const char *text);
static esp_err_t tg_build_scan_msg(const nw_scan_delta_t *d,
                                   char *out, size_t out_len);
static esp_err_t tg_build_test_msg(char *out, size_t out_len);

/* ============================================================
 * INIT
 * ============================================================ */

esp_err_t telegram_init(void)
{
    if (s_task) return ESP_OK;

    s_queue = xQueueCreate(NW_TG_QUEUE_SIZE, sizeof(tg_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    telegram_reload_config();

    BaseType_t ok = xTaskCreate(
        telegram_task,
        "telegram",
        TG_TASK_STACK,
        NULL,
        TG_TASK_PRIO,
        &s_task
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate telegram failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init ok (configured=%d, stack=%d)",
             (int)s_configured, TG_TASK_STACK);
    return ESP_OK;
}

esp_err_t telegram_reload_config(void)
{
    char tok[NW_TG_TOKEN_MAX_LEN + 1]  = {0};
    char cid[NW_TG_CHATID_MAX_LEN + 1] = {0};

    settings_get_telegram_token(tok, sizeof(tok));
    settings_get_telegram_chatid(cid, sizeof(cid));

    if (tok[0] != '\0' && cid[0] != '\0') {
        strncpy(s_token, tok, sizeof(s_token) - 1);
        s_token[sizeof(s_token) - 1] = '\0';
        strncpy(s_chat_id, cid, sizeof(s_chat_id) - 1);
        s_chat_id[sizeof(s_chat_id) - 1] = '\0';
        s_configured = true;
        ESP_LOGI(TAG, "config loaded (chat_id=%s)", s_chat_id);
    } else {
        s_token[0]   = '\0';
        s_chat_id[0] = '\0';
        s_configured = false;
        ESP_LOGD(TAG, "Telegram not configured");
    }
    return ESP_OK;
}

bool telegram_is_configured(void)
{
    return s_configured;
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

esp_err_t telegram_notify_scan(const nw_scan_delta_t *delta)
{
    if (!delta) return ESP_ERR_INVALID_ARG;
    if (!s_configured) {
        ESP_LOGD(TAG, "notify_scan ignored: not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_queue) return ESP_ERR_INVALID_STATE;

    tg_msg_t *msg = calloc(1, sizeof(tg_msg_t));
    if (!msg) {
        ESP_LOGW(TAG, "calloc failed for notify_scan");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t r = tg_build_scan_msg(delta, msg->text, sizeof(msg->text));
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

esp_err_t telegram_send_test(void)
{
    if (!s_configured) {
        ESP_LOGD(TAG, "send_test ignored: not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_queue) return ESP_ERR_INVALID_STATE;

    tg_msg_t *msg = calloc(1, sizeof(tg_msg_t));
    if (!msg) {
        ESP_LOGW(TAG, "calloc failed for send_test");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t r = tg_build_test_msg(msg->text, sizeof(msg->text));
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

/*
 * Synchronous test with temporary credentials (not saved in NVS).
 * Blocks the caller until the request completes (max ~10 sec).
 * Only call from the HTTP handler of the Test button.
 */
esp_err_t telegram_send_test_with(const char *token, const char *chat_id)
{
    if (!token || !chat_id) return ESP_ERR_INVALID_ARG;
    if (token[0] == '\0' || chat_id[0] == '\0') return ESP_ERR_INVALID_ARG;

    char text[TG_MSG_MAX];
    esp_err_t r = tg_build_test_msg(text, sizeof(text));
    if (r != ESP_OK) return r;

    ESP_LOGI(TAG, "synchronous test send with temporary credentials");
    return tg_send_with(text, token, chat_id);
}

/* ============================================================
 * TASK
 * ============================================================ */

static void telegram_task(void *arg)
{
    (void)arg;
    tg_msg_t msg;

    ESP_LOGI(TAG, "task started (stack=%d)", TG_TASK_STACK);

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (!s_configured) {
            ESP_LOGD(TAG, "msg discarded: not configured");
            continue;
        }

        esp_err_t r = ESP_FAIL;
        for (int attempt = 1; attempt <= NW_TG_MAX_RETRY; ++attempt) {
            r = tg_send(msg.text);
            if (r == ESP_OK) break;

            int delay_ms = 2000 * (1 << (attempt - 1));
            ESP_LOGW(TAG, "send failed (attempt %d/%d, err=%s), retry in %d ms",
                     attempt, NW_TG_MAX_RETRY, esp_err_to_name(r), delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        if (r == ESP_OK) {
            ESP_LOGI(TAG, "message sent");
        } else {
            ESP_LOGE(TAG, "send failed after %d attempts", NW_TG_MAX_RETRY);
        }
    }
}

/* ============================================================
 * HTTP SEND (core)
 *
 * All large buffers are allocated on the HEAP: escaped (~4 KB) and
 * payload (~4.3 KB) are too large for the callers' task stacks.
 * ============================================================ */

static esp_err_t tg_send_with(const char *text,
                              const char *token,
                              const char *chat_id)
{
    if (!text || !token || !chat_id) return ESP_ERR_INVALID_ARG;
    if (token[0] == '\0' || chat_id[0] == '\0') return ESP_ERR_INVALID_ARG;

    /* URL: small, ok on stack */
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/sendMessage", token);

    /* Large buffers on HEAP */
    size_t escaped_size = TG_MSG_MAX * 2;
    size_t payload_size = TG_MSG_MAX * 2 + 256;

    char *escaped = malloc(escaped_size);
    char *payload = malloc(payload_size);
    if (!escaped || !payload) {
        free(escaped);
        free(payload);
        ESP_LOGW(TAG, "malloc failed for escaped/payload");
        return ESP_ERR_NO_MEM;
    }

    /* Minimal JSON escape */
    size_t j = 0;
    for (size_t i = 0; text[i] && j < escaped_size - 2; ++i) {
        char c = text[i];
        if (c == '"' || c == '\\')      { escaped[j++] = '\\'; escaped[j++] = c; }
        else if (c == '\n')             { escaped[j++] = '\\'; escaped[j++] = 'n'; }
        else if (c == '\r')             { /* skip */ }
        else                            { escaped[j++] = c; }
    }
    escaped[j] = '\0';

    snprintf(payload, payload_size,
             "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"HTML\","
             "\"disable_web_page_preview\":true}",
             chat_id, escaped);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = NW_TG_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(escaped);
        free(payload);
        return ESP_FAIL;
    }

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, payload, strlen(payload));

    esp_err_t r = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);

    if (r == ESP_OK && status == 200) {
        ESP_LOGD(TAG, "HTTP 200 OK");
    } else {
        ESP_LOGW(TAG, "HTTP status=%d err=%s", status, esp_err_to_name(r));
        r = ESP_FAIL;
    }

    esp_http_client_cleanup(cli);
    free(escaped);
    free(payload);
    return r;
}

static esp_err_t tg_send(const char *text)
{
    return tg_send_with(text, s_token, s_chat_id);
}

/* ============================================================
 * MESSAGE FORMATTING
 * ============================================================ */

static esp_err_t tg_build_scan_msg(const nw_scan_delta_t *d,
                                   char *out, size_t out_len)
{
    if (!d || !out) return ESP_ERR_INVALID_ARG;

    int n = snprintf(out, out_len,
                     "\xF0\x9F\x93\xA1 <b>Scan completed</b>\n");

    if (d->new_unknown_count > 0) {
        n += snprintf(out + n, out_len - n,
                      "\n\xF0\x9F\x86\x95 <b>New devices (%d):</b>\n",
                      d->new_unknown_count);
        for (int i = 0; i < d->new_unknown_count && n < (int)out_len; ++i) {
            n += snprintf(out + n, out_len - n,
                          "\xE2\x80\xA2 <code>%s</code> - %s\n",
                          d->new_unknown[i].mac,
                          d->new_unknown[i].ip);
        }
    }

    if (d->blacklist_count > 0) {
        n += snprintf(out + n, out_len - n,
                      "\n\xF0\x9F\x9A\xAB <b>Blacklist (%d):</b>\n",
                      d->blacklist_count);
        for (int i = 0; i < d->blacklist_count && n < (int)out_len; ++i) {
            n += snprintf(out + n, out_len - n,
                          "\xE2\x80\xA2 <code>%s</code> - %s\n",
                          d->blacklist[i].mac,
                          d->blacklist[i].ip);
        }
    }

    return ESP_OK;
}

static esp_err_t tg_build_test_msg(char *out, size_t out_len)
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

    snprintf(out, out_len,
             "\xE2\x9C\x85 <b>Test NetworkWatcher</b>\n"
             "IP: <code>%s</code>\n"
             "Host: <code>network-watcher.local</code>\n"
             "Uptime: %lu sec\n"
             "Heap: %lu bytes",
             ip,
             (unsigned long)uptime,
             (unsigned long)heap);
    return ESP_OK;
}