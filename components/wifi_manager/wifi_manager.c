/*
 * NetworkWatcher - wifi_manager.c
 * Wi-Fi management (AP/STA/scan/retry/test).
 */

#include "wifi_manager.h"
#include "config.h"
#include "storage.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"

static const char *TAG = "wifi_mgr";

/* ============================================================
 * INTERNAL STATE
 * ============================================================ */

static bool             s_initialized     = false;
static nw_wifi_state_t  s_state           = NW_WIFI_STATE_IDLE;
static wifi_manager_cb_t s_callback       = NULL;
static esp_netif_t     *s_netif_ap        = NULL;
static esp_netif_t     *s_netif_sta       = NULL;

/* Flag: are we in scan phase?
 * When true, the STA_START handler does NOT call esp_wifi_connect(). */
static volatile bool    s_scan_mode       = false;

/* Retry policy */
static int              s_retry_count     = 0;
static esp_timer_handle_t s_retry_timer   = NULL;

/* Credentials in RAM */
static char             s_sta_ssid[NW_WIFI_SSID_MAX_LEN] = {0};
static char             s_sta_pass[NW_WIFI_PASS_MAX_LEN] = {0};

/* Test STA */
static EventGroupHandle_t s_test_evt_group = NULL;
#define TEST_BIT_CONNECTED  BIT0
#define TEST_BIT_FAILED     BIT1

/* ============================================================
 * HELPER: notify event
 * ============================================================ */

static void notify(nw_wifi_event_t evt, void *data)
{
    if (s_callback) s_callback(evt, data);
}

static void set_state(nw_wifi_state_t st)
{
    if (s_state != st) {
        ESP_LOGI(TAG, "state: %d -> %d", (int)s_state, (int)st);
        s_state = st;
    }
}

/* ============================================================
 * RETRY POLICY
 * ============================================================ */

static uint32_t retry_interval_ms(void)
{
    if (s_retry_count < NW_WIFI_RETRY_FAST_COUNT) {
        return NW_WIFI_RETRY_FAST_MS;
    }
    if (s_retry_count < (NW_WIFI_RETRY_FAST_COUNT + NW_WIFI_RETRY_MEDIUM_COUNT)) {
        return NW_WIFI_RETRY_MEDIUM_MS;
    }
    return NW_WIFI_RETRY_SLOW_MS;
}

static void retry_timer_cb(void *arg)
{
    if (s_state != NW_WIFI_STATE_STA_RETRY) return;

    s_retry_count++;
    ESP_LOGW(TAG, "connection retry #%d", s_retry_count);
    set_state(NW_WIFI_STATE_STA_CONNECTING);
    esp_wifi_connect();
}

static void schedule_retry(void)
{
    uint32_t interval = retry_interval_ms();
    ESP_LOGW(TAG, "next retry in %u ms (attempt #%d)",
             (unsigned)interval, s_retry_count + 1);

    set_state(NW_WIFI_STATE_STA_RETRY);

    if (s_retry_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = retry_timer_cb,
            .name     = "wifi_retry",
        };
        esp_timer_create(&args, &s_retry_timer);
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)interval * 1000ULL);
}

/* ============================================================
 * EVENT HANDLERS
 * ============================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP started");
            set_state(NW_WIFI_STATE_AP);
            notify(NW_WIFI_EVT_AP_STARTED, NULL);
            break;

        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "AP stopped");
            notify(NW_WIFI_EVT_AP_STOPPED, NULL);
            break;

        case WIFI_EVENT_STA_START:
            /* If in scan mode, do NOT connect.
             * The Wi-Fi driver generates this event also when switching
             * to WIFI_MODE_APSTA to scan. If we called esp_wifi_connect()
             * here, the scan would fail with ESP_ERR_WIFI_STATE. */
            if (s_scan_mode) {
                ESP_LOGD(TAG, "STA_START ignored (scan mode)");
            } else {
                ESP_LOGI(TAG, "STA started, trying to connect");
                set_state(NW_WIFI_STATE_STA_CONNECTING);
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA connected to AP");
            s_retry_count = 0;
            set_state(NW_WIFI_STATE_STA_CONNECTED);
            notify(NW_WIFI_EVT_STA_CONNECTED, NULL);
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;

            /* If in scan phase, ignore */
            if (s_scan_mode) {
                ESP_LOGD(TAG, "STA_DISCONNECTED ignored (scan mode)");
                break;
            }

            /* If in test phase, signal failure */
            if (s_test_evt_group) {
                ESP_LOGW(TAG, "STA disconnected during test (reason %d)",
                         d ? d->reason : -1);
                xEventGroupSetBits(s_test_evt_group, TEST_BIT_FAILED);
                break;
            }

            ESP_LOGW(TAG, "STA disconnected (reason %d)", d ? d->reason : -1);
            set_state(NW_WIFI_STATE_STA_DISCONNECTED);
            notify(NW_WIFI_EVT_STA_DISCONNECTED, d);
            schedule_retry();
            break;
        }

        default:
            break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP obtained: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_count = 0;

        if (s_test_evt_group) {
            xEventGroupSetBits(s_test_evt_group, TEST_BIT_CONNECTED);
        }

        notify(NW_WIFI_EVT_STA_GOT_IP, &evt->ip_info);
    }
}

/* ============================================================
 * INIT
 * ============================================================ */

esp_err_t wifi_manager_init(wifi_manager_cb_t cb)
{
    if (s_initialized) return ESP_OK;

    s_callback = cb;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create_default: %s", esp_err_to_name(ret));
        return ret;
    }

    s_netif_ap  = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    s_initialized = true;
    ESP_LOGI(TAG, "wifi_manager initialized");
    return ESP_OK;
}

/* ============================================================
 * AP MODE
 * ============================================================ */

esp_err_t wifi_manager_start_ap(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    esp_wifi_stop();

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, NW_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(NW_AP_SSID);
    strncpy((char *)ap_cfg.ap.password, NW_AP_PASSWORD, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.channel        = NW_AP_CHANNEL;
    ap_cfg.ap.max_connection = NW_AP_MAX_CONN;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "AP started: SSID='%s'", NW_AP_SSID);
    return ESP_OK;
}

esp_err_t wifi_manager_stop_ap(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    esp_wifi_stop();
    return ESP_OK;
}

/* ============================================================
 * STA MODE
 * ============================================================ */

esp_err_t wifi_manager_connect_sta(const char *ssid, const char *pass)
{
    if (!s_initialized || !ssid) return ESP_ERR_INVALID_ARG;

    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';

    if (pass) {
        strncpy(s_sta_pass, pass, sizeof(s_sta_pass) - 1);
        s_sta_pass[sizeof(s_sta_pass) - 1] = '\0';
    } else {
        s_sta_pass[0] = '\0';
    }

    esp_wifi_stop();

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, s_sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, s_sta_pass, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (ret != ESP_OK) return ret;

    s_retry_count = 0;
    set_state(NW_WIFI_STATE_STA_CONNECTING);

    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "STA connection to '%s'", s_sta_ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_disconnect_sta(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    return esp_wifi_disconnect();
}

/* ============================================================
 * SCAN
 * ------------------------------------------------------------
 * Uses the s_scan_mode flag to prevent the STA_START handler
 * from calling esp_wifi_connect() when switching to APSTA.
 * ============================================================ */

esp_err_t wifi_manager_scan(nw_wifi_ap_t *out, size_t max, size_t *found)
{
    if (!out || !found) return ESP_ERR_INVALID_ARG;
    *found = 0;

    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    /* Enter scan mode: STA_START handler won't call connect */
    s_scan_mode = true;

    wifi_mode_t prev_mode;
    esp_wifi_get_mode(&prev_mode);

    bool restore_mode = false;
    if (prev_mode != WIFI_MODE_STA && prev_mode != WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        restore_mode = true;
        /* Wait for STA to stabilize in APSTA */
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(ret));
        s_scan_mode = false;
        if (restore_mode) esp_wifi_set_mode(prev_mode);
        return ret;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        s_scan_mode = false;
        if (restore_mode) esp_wifi_set_mode(prev_mode);
        return ESP_OK;
    }

    uint16_t to_get = (ap_count > max) ? max : ap_count;
    wifi_ap_record_t *records = calloc(to_get, sizeof(wifi_ap_record_t));
    if (!records) {
        s_scan_mode = false;
        if (restore_mode) esp_wifi_set_mode(prev_mode);
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&to_get, records);
    if (ret != ESP_OK) {
        free(records);
        s_scan_mode = false;
        if (restore_mode) esp_wifi_set_mode(prev_mode);
        return ret;
    }

    for (uint16_t i = 0; i < to_get; i++) {
        strncpy(out[i].ssid, (const char *)records[i].ssid, NW_WIFI_SSID_MAX_LEN - 1);
        out[i].ssid[NW_WIFI_SSID_MAX_LEN - 1] = '\0';
        out[i].rssi     = records[i].rssi;
        out[i].authmode = records[i].authmode;
        out[i].channel  = records[i].primary;
    }
    *found = to_get;

    free(records);

    /* Exit scan mode */
    s_scan_mode = false;

    if (restore_mode) esp_wifi_set_mode(prev_mode);

    ESP_LOGI(TAG, "scan completed: %u networks found", (unsigned)*found);
    return ESP_OK;
}

/* ============================================================
 * CONNECTION TEST
 * ============================================================ */

esp_err_t wifi_manager_test_sta(const char *ssid, const char *pass,
                                char *out_ip, size_t ip_len,
                                char *out_gw, size_t gw_len,
                                char *out_dns, size_t dns_len,
                                uint32_t timeout_ms)
{
    if (!s_initialized || !ssid) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "connection test to '%s' (timeout %u ms)",
             ssid, (unsigned)timeout_ms);

    wifi_mode_t prev_mode;
    esp_wifi_get_mode(&prev_mode);

    s_test_evt_group = xEventGroupCreate();
    if (!s_test_evt_group) return ESP_ERR_NO_MEM;

    if (prev_mode != WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (pass) {
        strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    }
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_test_evt_group,
        TEST_BIT_CONNECTED | TEST_BIT_FAILED,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms)
    );

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t result = ESP_FAIL;

    if (bits & TEST_BIT_CONNECTED) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_netif_sta, &ip_info) == ESP_OK) {
            if (out_ip && ip_len)  snprintf(out_ip, ip_len, IPSTR, IP2STR(&ip_info.ip));
            if (out_gw && gw_len)  snprintf(out_gw, gw_len, IPSTR, IP2STR(&ip_info.gw));
        }

        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            if (out_dns && dns_len) snprintf(out_dns, dns_len, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }

        ESP_LOGI(TAG, "test successful: IP=%s GW=%s DNS=%s",
                 out_ip ? out_ip : "?", out_gw ? out_gw : "?", out_dns ? out_dns : "?");
        result = ESP_OK;
    } else {
        ESP_LOGW(TAG, "test failed (timeout or disconnection)");
    }

    if (prev_mode != WIFI_MODE_APSTA) {
        esp_wifi_set_mode(prev_mode);
    }

    vEventGroupDelete(s_test_evt_group);
    s_test_evt_group = NULL;

    return result;
}

esp_err_t wifi_manager_test_cleanup(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    esp_wifi_disconnect();
    return ESP_OK;
}

/* ============================================================
 * PERSISTENCE
 * ============================================================ */

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !pass) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = storage_set_string(NW_NVS_KEY_WIFI_SSID, ssid);
    if (ret != ESP_OK) return ret;

    ret = storage_set_string(NW_NVS_KEY_WIFI_PASS, pass);
    if (ret != ESP_OK) return ret;

    ret = storage_set_bool(NW_NVS_KEY_WIFI_CONFIGURED, true);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "credentials saved for '%s'", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len,
                                        char *pass, size_t pass_len)
{
    if (!ssid || !pass) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = storage_get_string(NW_NVS_KEY_WIFI_SSID, ssid, ssid_len);
    if (ret != ESP_OK) return ret;

    ret = storage_get_string(NW_NVS_KEY_WIFI_PASS, pass, pass_len);
    return ret;
}

esp_err_t wifi_manager_forget_credentials(void)
{
    storage_erase_key(NW_NVS_KEY_WIFI_SSID);
    storage_erase_key(NW_NVS_KEY_WIFI_PASS);
    storage_erase_key(NW_NVS_KEY_WIFI_CONFIGURED);

    s_sta_ssid[0] = '\0';
    s_sta_pass[0] = '\0';

    ESP_LOGW(TAG, "Wi-Fi credentials cleared");
    return ESP_OK;
}

bool wifi_manager_has_credentials(void)
{
    bool cfg = false;
    storage_get_bool(NW_NVS_KEY_WIFI_CONFIGURED, &cfg, false);
    return cfg;
}

/* ============================================================
 * INFO
 * ============================================================ */

nw_wifi_state_t wifi_manager_get_state(void)
{
    return s_state;
}

esp_err_t wifi_manager_get_ip(char *out, size_t len)
{
    if (!out || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_netif_sta) return ESP_ERR_INVALID_STATE;

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(s_netif_sta, &ip_info);
    if (ret != ESP_OK) return ret;

    snprintf(out, len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t wifi_manager_get_mac(uint8_t mac[6])
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    return esp_wifi_get_mac(WIFI_IF_STA, mac);
}