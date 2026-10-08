/*
 * NetworkWatcher - arp_scanner.c
 * Network scan via ARP request broadcast + ARP table read.
 * Single task with internal periodic scan.
 *
 * LED logic after scan:
 *  - Blacklist present        -> LED steady on
 *  - Only unknown             -> LED slow blink
 *  - Neither blacklist nor unknown -> LED off
 *
 * Notifications (Fase 2.7 + 3.4):
 *  - after every scan, if there are new unknown or blacklist matches,
 *    send ONE aggregated message to Telegram and/or webhook.
 *
 * STACK NOTE:
 *  - The task has 4096 bytes of stack (max limit: 6144 breaks USB CDC).
 *  - Both nw_arp_scan_batch_t (~1 KB) and nw_scan_delta_t (~2.2 KB) are
 *    allocated on the HEAP, not on the stack, to avoid overflow.
 */

#include "arp_scanner.h"
#include "config.h"
#include "wifi_manager.h"
#include "mac_list.h"
#include "led.h"
#include "telegram.h"
#include "webhook.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/arch.h"
#include "lwip/tcpip.h"

static const char *TAG = "arp_scanner";

#define ARP_TASK_STACK_SIZE     4096
#define ARP_TASK_PRIORITY       4

/* ============================================================
 * INTERNAL STATE
 * ============================================================ */

static TaskHandle_t       s_task = NULL;
static SemaphoreHandle_t  s_trigger = NULL;
static SemaphoreHandle_t  s_result_mutex = NULL;
static volatile bool      s_scanning = false;
static nw_arp_scan_batch_t s_last_result = {0};
static volatile int32_t   s_interval_min = NW_SCAN_INTERVAL_DEFAULT_MIN;

/* ============================================================
 * SEND ARP REQUEST
 * ============================================================ */

static err_t send_arp_request(struct netif *lwip_netif, uint32_t ip_host)
{
    ip4_addr_t target_ip;
    target_ip.addr = htonl(ip_host);

    LOCK_TCPIP_CORE();
    err_t ret = etharp_request(lwip_netif, &target_ip);
    UNLOCK_TCPIP_CORE();

    return ret;
}

/* ============================================================
 * READ ARP TABLE
 * ============================================================ */

static void read_arp_table(nw_arp_scan_batch_t *result,
                            uint32_t network,
                            uint32_t netmask,
                            uint32_t my_ip)
{
    LOCK_TCPIP_CORE();

    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip_ret = NULL;
        struct netif *netif_ret = NULL;
        struct eth_addr *eth_ret = NULL;

        if (etharp_get_entry(i, &ip_ret, &netif_ret, &eth_ret) != 1) {
            continue;
        }
        if (!ip_ret || !eth_ret) continue;

        uint32_t ip_host = ntohl(ip_ret->addr);

        if ((ip_host & netmask) != network) continue;
        if (ip_host == my_ip) continue;
        if (ip_host == 0 || ip_host == 0xFFFFFFFF) continue;

        bool dup = false;
        for (size_t j = 0; j < result->count; j++) {
            if (result->entries[j].ip == ip_host) {
                result->entries[j].last_seen = (uint32_t)(esp_timer_get_time() / 1000000LL);
                dup = true;
                break;
            }
        }
        if (dup) continue;

        if (result->count >= ARP_SCAN_MAX_RESULTS) {
            ESP_LOGW(TAG, "result limit reached (%d)", ARP_SCAN_MAX_RESULTS);
            break;
        }

        nw_scan_result_t *e = &result->entries[result->count];
        e->ip = ip_host;
        memcpy(e->mac.bytes, eth_ret->addr, 6);
        e->valid = true;
        e->last_seen = (uint32_t)(esp_timer_get_time() / 1000000LL);
        result->count++;
    }

    UNLOCK_TCPIP_CORE();
}

/* ============================================================
 * UPDATE LED BASED ON LISTS
 * ============================================================ */

static void update_led_from_lists(void)
{
    bool bl_present = mac_list_blacklist_present();
    size_t unknown_count = mac_list_count(NW_LIST_UNKNOWN);

    if (bl_present) {
        ESP_LOGW(TAG, "LED: BLACKLIST present (%u unknown) -> LED steady on",
                 (unsigned)unknown_count);
        led_set_pattern(LED_PATTERN_ON);
    } else if (unknown_count > 0) {
        ESP_LOGW(TAG, "LED: no blacklist, %u unknown -> LED blink",
                 (unsigned)unknown_count);
        led_set_pattern(LED_PATTERN_BLINK_SLOW);
    } else {
        ESP_LOGI(TAG, "LED: lists clean -> LED off");
        led_set_pattern(LED_PATTERN_OFF);
    }
}

/* ============================================================
 * RUN A SCAN
 * ============================================================ */

static void do_scan(void)
{
    if (wifi_manager_get_state() != NW_WIFI_STATE_STA_CONNECTED) {
        ESP_LOGD(TAG, "scan skipped: not connected in STA");
        return;
    }

    s_scanning = true;
    int64_t start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "ARP scan started...");
    led_set_pattern(LED_PATTERN_SCAN);

    esp_netif_t *esp_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!esp_netif) {
        ESP_LOGE(TAG, "STA netif not found");
        s_scanning = false;
        update_led_from_lists();
        return;
    }

    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(esp_netif);
    if (!lwip_netif) {
        ESP_LOGE(TAG, "lwip_netif NULL");
        s_scanning = false;
        update_led_from_lists();
        return;
    }

    if (lwip_netif->hwaddr_len != ETH_HWADDR_LEN) {
        ESP_LOGE(TAG, "invalid hwaddr_len (%d, expected %d)",
                 lwip_netif->hwaddr_len, ETH_HWADDR_LEN);
        s_scanning = false;
        update_led_from_lists();
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(esp_netif, &ip_info) != ESP_OK) {
        ESP_LOGE(TAG, "get_ip_info failed");
        s_scanning = false;
        update_led_from_lists();
        return;
    }

    uint32_t my_ip   = ntohl(ip_info.ip.addr);
    uint32_t netmask = ntohl(ip_info.netmask.addr);
    uint32_t network = my_ip & netmask;
    uint32_t broadcast = network | ~netmask;

    nw_arp_scan_batch_t *result = calloc(1, sizeof(nw_arp_scan_batch_t));
    if (!result) {
        ESP_LOGE(TAG, "calloc result failed");
        s_scanning = false;
        update_led_from_lists();
        return;
    }

    uint32_t first = network + 1;
    uint32_t last  = broadcast - 1;
    uint32_t scanned = 0;

    for (uint32_t ip = first; ip <= last && scanned < 254; ip++) {
        if (ip == my_ip) continue;

        send_arp_request(lwip_netif, ip);
        scanned++;

        if ((scanned % 5) == 0) {
            read_arp_table(result, network, netmask, my_ip);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    read_arp_table(result, network, netmask, my_ip);

    int64_t end_us = esp_timer_get_time();
    result->duration_ms = (uint32_t)((end_us - start_us) / 1000);
    result->scanned_ips = scanned;
    result->valid = true;

    ESP_LOGI(TAG, "scan completed: %u IPs, %u devices, %u ms",
             (unsigned)scanned, (unsigned)result->count,
             (unsigned)result->duration_ms);

    xSemaphoreTake(s_result_mutex, portMAX_DELAY);
    memcpy(&s_last_result, result, sizeof(*result));
    xSemaphoreGive(s_result_mutex);

    nw_scan_delta_t *delta = calloc(1, sizeof(nw_scan_delta_t));
    if (!delta) {
        ESP_LOGW(TAG, "calloc delta failed, skipping notifications");
        int new_count = mac_list_process_scan(result, NULL);
        if (new_count > 0) {
            ESP_LOGW(TAG, "%d new devices added to unknown", new_count);
        }
    } else {
        int new_count = mac_list_process_scan(result, delta);
        if (new_count > 0) {
            ESP_LOGW(TAG, "%d new devices added to unknown", new_count);
        }

        /* Notifications: Telegram + webhook, in parallel, same condition */
        if (delta->new_unknown_count > 0 || delta->blacklist_count > 0) {
            ESP_LOGI(TAG, "notification: %d new unknown, %d in blacklist",
                     delta->new_unknown_count, delta->blacklist_count);

            telegram_notify_scan(delta);
            webhook_notify_scan(delta);
        }

        free(delta);
    }

    free(result);

    update_led_from_lists();

    s_scanning = false;
}

/* ============================================================
 * SINGLE TASK
 * ============================================================ */

static void arp_scanner_task(void *arg)
{
    ESP_LOGI(TAG, "task started (stack=%d, prio=%d)",
             ARP_TASK_STACK_SIZE, ARP_TASK_PRIORITY);

    int waited_ms = 0;
    while (wifi_manager_get_state() != NW_WIFI_STATE_STA_CONNECTED && waited_ms < 60000) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited_ms += 1000;
    }

    if (wifi_manager_get_state() == NW_WIFI_STATE_STA_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "automatic initial scan");
        do_scan();
    } else {
        ESP_LOGW(TAG, "WiFi not connected after 60 sec, skipping initial scan");
    }

    while (1) {
        int32_t interval_min = s_interval_min;
        if (interval_min < NW_SCAN_INTERVAL_MIN_MIN) interval_min = NW_SCAN_INTERVAL_MIN_MIN;
        if (interval_min > NW_SCAN_INTERVAL_MAX_MIN) interval_min = NW_SCAN_INTERVAL_MAX_MIN;

        TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)interval_min * 60000);

        BaseType_t triggered = xSemaphoreTake(s_trigger, wait_ticks);

        if (triggered == pdTRUE) {
            ESP_LOGI(TAG, "manual scan requested");
        } else {
            ESP_LOGI(TAG, "periodic scan (interval %d min)", (int)interval_min);
        }

        do_scan();
    }
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

esp_err_t arp_scanner_init(void)
{
    if (s_task) return ESP_OK;

    s_trigger = xSemaphoreCreateBinary();
    if (!s_trigger) return ESP_ERR_NO_MEM;

    s_result_mutex = xSemaphoreCreateMutex();
    if (!s_result_mutex) {
        vSemaphoreDelete(s_trigger);
        s_trigger = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(
        arp_scanner_task,
        "arp_scan",
        ARP_TASK_STACK_SIZE,
        NULL,
        ARP_TASK_PRIORITY,
        &s_task
    );

    if (ok != pdPASS) {
        vSemaphoreDelete(s_trigger);
        vSemaphoreDelete(s_result_mutex);
        s_trigger = NULL;
        s_result_mutex = NULL;
        s_task = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "arp_scanner initialized (stack=%d)", ARP_TASK_STACK_SIZE);
    return ESP_OK;
}

esp_err_t arp_scanner_start_scan(void)
{
    if (!s_trigger) return ESP_ERR_INVALID_STATE;
    if (s_scanning) return ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_trigger);
    return ESP_OK;
}

bool arp_scanner_is_scanning(void)
{
    return s_scanning;
}

esp_err_t arp_scanner_get_last_result(nw_arp_scan_batch_t *out)
{
    if (!out || !s_result_mutex) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_result_mutex, portMAX_DELAY);
    memcpy(out, &s_last_result, sizeof(*out));
    xSemaphoreGive(s_result_mutex);

    return ESP_OK;
}

void arp_scanner_set_interval(int32_t minutes)
{
    if (minutes < NW_SCAN_INTERVAL_MIN_MIN) minutes = NW_SCAN_INTERVAL_MIN_MIN;
    if (minutes > NW_SCAN_INTERVAL_MAX_MIN) minutes = NW_SCAN_INTERVAL_MAX_MIN;
    s_interval_min = minutes;
    ESP_LOGI(TAG, "scan interval set to %d min", (int)minutes);
}

int32_t arp_scanner_get_interval(void)
{
    return s_interval_min;
}