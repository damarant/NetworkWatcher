/*
 * NetworkWatcher - mdns_service.c
 * mDNS implementation.
 */

#include "mdns_service.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "mdns";

static bool s_initialized = false;
static char s_hostname[64] = {0};

esp_err_t mdns_service_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "mDNS already initialized");
        return ESP_OK;
    }

    /* Initialize mDNS */
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Hostname: network-watcher.local */
    ret = mdns_hostname_set(NW_MDNS_HOSTNAME_DEFAULT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Instance name visible in mDNS browsers */
    ret = mdns_instance_name_set("NetworkWatcher");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(ret));
    }

    /* Advertise HTTP service on port 80 */
    mdns_txt_item_t txt[] = {
        { "version", NW_PROJECT_VERSION },
        { "board",   "ESP32-S2" },
    };

    ret = mdns_service_add(NULL, "_http", "_tcp", NW_HTTP_PORT,
                           txt, sizeof(txt) / sizeof(txt[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    snprintf(s_hostname, sizeof(s_hostname), "%s.local", NW_MDNS_HOSTNAME_DEFAULT);

    s_initialized = true;
    ESP_LOGI(TAG, "mDNS active: http://%s", s_hostname);
    return ESP_OK;
}

void mdns_service_stop(void)
{
    if (!s_initialized) return;
    mdns_free();
    s_initialized = false;
    s_hostname[0] = '\0';
    ESP_LOGI(TAG, "mDNS stopped");
}

const char *mdns_service_get_hostname(void)
{
    return s_hostname;
}