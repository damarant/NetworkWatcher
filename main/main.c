/*
 * NetworkWatcher - main.c
 * Orchestration: storage, LED, button, Wi-Fi, auth, web server, mDNS,
 * ARP scan, MAC lists, NTP sync, settings, Telegram, generic webhook,
 * log ring buffer.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "config.h"
#include "storage.h"
#include "led.h"
#include "button.h"
#include "wifi_manager.h"
#include "auth.h"
#include "web_server.h"
#include "mdns_service.h"
#include "arp_scanner.h"
#include "mac_list.h"
#include "settings.h"
#include "time_service.h"
#include "telegram.h"
#include "webhook.h"
#include "log_buffer.h"

static const char *TAG = "main";

/* ============================================================
 * BUTTON CALLBACK
 * ============================================================ */

static void on_button_event(btn_event_t evt)
{
    switch (evt) {

    case BTN_EVENT_SHORT_PRESS:
        ESP_LOGI(TAG, "short press");
        led_pulse();
        break;

    case BTN_EVENT_LONG_PRESS:
        ESP_LOGW(TAG, "long press -> clearing Wi-Fi credentials and rebooting to AP");
        led_set_pattern(LED_PATTERN_ERROR);
        vTaskDelay(pdMS_TO_TICKS(1500));

        ESP_LOGW(TAG, "stopping WiFi before reset...");
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_restore();
        vTaskDelay(pdMS_TO_TICKS(500));

        wifi_manager_forget_credentials();

        ESP_LOGW(TAG, "rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        break;

    case BTN_EVENT_SEQUENCE_3:
        ESP_LOGW(TAG, "3-click sequence detected: waiting for confirmation (4th click within 5 sec)");
        led_set_pattern(LED_PATTERN_BLINK_FAST);
        break;

    case BTN_EVENT_SEQUENCE_CONFIRMED:
        ESP_LOGE(TAG, "FACTORY RESET confirmed");
        led_set_pattern(LED_PATTERN_ON);
        vTaskDelay(pdMS_TO_TICKS(3000));

        ESP_LOGW(TAG, "stopping WiFi before reset...");
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(500));

        ESP_LOGW(TAG, "resetting WiFi config via esp_wifi_restore()...");
        esp_wifi_restore();
        vTaskDelay(pdMS_TO_TICKS(1000));

        storage_erase_all();

        ESP_LOGE(TAG, "rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        break;

    case BTN_EVENT_SEQUENCE_CANCELLED:
        ESP_LOGI(TAG, "sequence cancelled, returning to normal state");
        if (wifi_manager_get_state() == NW_WIFI_STATE_AP) {
            led_set_pattern(LED_PATTERN_BLINK_SLOW);
        } else if (wifi_manager_get_state() == NW_WIFI_STATE_STA_CONNECTED) {
            led_set_pattern(LED_PATTERN_HEARTBEAT);
        } else {
            led_set_pattern(LED_PATTERN_BLINK_SLOW);
        }
        break;

    default:
        break;
    }
}

/* ============================================================
 * WIFI CALLBACK
 * ============================================================ */

static void on_wifi_event(nw_wifi_event_t evt, void *data)
{
    switch (evt) {

    case NW_WIFI_EVT_AP_STARTED:
        ESP_LOGI(TAG, "AP ready: SSID='%s' pass='%s'", NW_AP_SSID, NW_AP_PASSWORD);
        led_set_pattern(LED_PATTERN_BLINK_SLOW);
        mdns_service_init();
        break;

    case NW_WIFI_EVT_AP_STOPPED:
        ESP_LOGI(TAG, "AP stopped");
        break;

    case NW_WIFI_EVT_STA_CONNECTED:
        ESP_LOGI(TAG, "connected to home network");
        led_set_pattern(LED_PATTERN_BLINK_FAST);
        break;

    case NW_WIFI_EVT_STA_GOT_IP: {
        char ip[16] = {0};
        wifi_manager_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "IP assigned: %s", ip);
        led_set_pattern(LED_PATTERN_HEARTBEAT);
        mdns_service_init();
        break;
    }

    case NW_WIFI_EVT_STA_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from home network");
        led_set_pattern(LED_PATTERN_BLINK_SLOW);
        break;

    default:
        break;
    }
}

/* ============================================================
 * APP_MAIN
 * ============================================================ */

void app_main(void)
{
    /* --- 0. Log buffer (FIRST of all: capture more logs) --- */
    log_buffer_init();

    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, " %s v%s", NW_PROJECT_NAME, NW_PROJECT_VERSION);
    ESP_LOGI(TAG, "===========================================");

    ESP_LOGW(TAG, "free HEAP at boot: %u bytes",
             (unsigned)esp_get_free_heap_size());

    /* --- 1. LED --- */
    ESP_ERROR_CHECK(led_init());
    led_set_pattern(LED_PATTERN_BLINK_FAST);
    ESP_LOGI(TAG, "led OK  heap=%u", (unsigned)esp_get_free_heap_size());

    /* --- 2. Storage --- */
    esp_err_t ret = storage_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "storage_init failed: %s", esp_err_to_name(ret));
        led_set_pattern(LED_PATTERN_ERROR);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "storage OK  heap=%u", (unsigned)esp_get_free_heap_size());

    /* --- 3. Auth --- */
    ret = auth_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "auth_init failed: %s", esp_err_to_name(ret));
        led_set_pattern(LED_PATTERN_ERROR);
    } else {
        ESP_LOGI(TAG, "auth OK  heap=%u", (unsigned)esp_get_free_heap_size());
    }

    /* --- 4. Button --- */
    ESP_ERROR_CHECK(button_init(on_button_event));
    ESP_LOGI(TAG, "button OK  heap=%u", (unsigned)esp_get_free_heap_size());

    /* --- 5. Wi-Fi manager --- */
    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_event));
    ESP_LOGI(TAG, "wifi_manager OK  heap=%u", (unsigned)esp_get_free_heap_size());

    /* --- 6. Web server --- */
    ret = web_server_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "web_server_init failed: %s", esp_err_to_name(ret));
        led_set_pattern(LED_PATTERN_ERROR);
    } else {
        ESP_LOGI(TAG, "web_server OK  heap=%u", (unsigned)esp_get_free_heap_size());
    }

    /* --- 7. MAC list --- */
    ret = mac_list_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mac_list_init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "mac_list OK  heap=%u", (unsigned)esp_get_free_heap_size());
    }

    /* --- 8. ARP scanner --- */
    ret = arp_scanner_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "arp_scanner_init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "arp_scanner OK  heap=%u", (unsigned)esp_get_free_heap_size());
    }

    /* --- 9. Settings --- */
    ret = settings_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "settings_init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "settings OK  heap=%u", (unsigned)esp_get_free_heap_size());
    }

    /* --- 10. Time service --- */
    ret = time_service_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "time_service_init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "time_service OK  heap=%u", (unsigned)esp_get_free_heap_size());
    }

    /* --- 10b. Telegram --- */
    ret = telegram_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "telegram_init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "telegram OK  heap=%u", (unsigned)esp_get_free_heap_size());
    }

    /* --- 10c. Webhook --- */
    ret = webhook_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "webhook_init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "webhook OK  heap=%u", (unsigned)esp_get_free_heap_size());
    }

    /* --- 11. Mode decision --- */
    if (wifi_manager_has_credentials()) {
        char ssid[NW_WIFI_SSID_MAX_LEN] = {0};
        char pass[NW_WIFI_PASS_MAX_LEN] = {0};

        ret = wifi_manager_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
        if (ret == ESP_OK && ssid[0] != '\0') {
            ESP_LOGI(TAG, "credentials found for '%s', starting STA connection", ssid);
            wifi_manager_connect_sta(ssid, pass);
        } else {
            ESP_LOGW(TAG, "corrupted credentials, starting AP");
            wifi_manager_start_ap();
        }
    } else {
        ESP_LOGI(TAG, "no saved credentials, starting setup AP");
        wifi_manager_start_ap();
    }

    /* --- 12. Main loop --- */
    ESP_LOGI(TAG, "system started  heap=%u min=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        nw_wifi_state_t st = wifi_manager_get_state();
        const char *st_str = "?";
        switch (st) {
            case NW_WIFI_STATE_IDLE:            st_str = "IDLE"; break;
            case NW_WIFI_STATE_AP:              st_str = "AP"; break;
            case NW_WIFI_STATE_STA_CONNECTING:  st_str = "CONNECTING"; break;
            case NW_WIFI_STATE_STA_CONNECTED:   st_str = "CONNECTED"; break;
            case NW_WIFI_STATE_STA_DISCONNECTED:st_str = "DISCONNECTED"; break;
            case NW_WIFI_STATE_STA_RETRY:       st_str = "RETRY"; break;
        }

        char ip[16] = {0};
        wifi_manager_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "state=%s ip=%s host=%s scanning=%d time_sync=%d tg=%d wh=%d heap=%u min=%u",
                 st_str, ip[0] ? ip : "-",
                 mdns_service_get_hostname(),
                 arp_scanner_is_scanning() ? 1 : 0,
                 time_service_is_synced() ? 1 : 0,
                 telegram_is_configured() ? 1 : 0,
                 webhook_is_configured() ? 1 : 0,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
    }
}