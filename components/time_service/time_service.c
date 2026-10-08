/*
 * NetworkWatcher - time_service.c
 * SNTP synchronization without dedicated task.
 *
 * Strategy:
 *  - esp_sntp_init() internally starts its client (no additional task)
 *  - An esp_timer one-shot retries if it fails
 *  - On successful sync, updates mac_list_set_time_synced(true)
 */

#include "time_service.h"
#include "config.h"
#include "mac_list.h"

#include <string.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"

static const char *TAG = "time";

static bool s_synced = false;
static esp_timer_handle_t s_retry_timer = NULL;

/* NTP servers in preference order */
static const char *s_ntp_servers[] = {
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com",
};
#define NTP_SERVER_COUNT (sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0]))

/* Retry intervals (ms) */
#define RETRY_INITIAL_MS    10000   /* 10 sec */
#define RETRY_LATER_MS      300000  /* 5 min */
#define RETRY_OK_MS         3600000 /* 1 hour */

/* ============================================================
 * CALLBACK: SNTP notification (called from lwIP thread)
 * ============================================================ */

static void on_sntp_sync(struct timeval *tv)
{
    s_synced = true;
    mac_list_set_time_synced(true);

    time_t now = tv->tv_sec;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "SNTP synchronized: %s", buf);
}

/* ============================================================
 * HELPER: start SNTP
 * ============================================================ */

static void start_sntp(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    for (size_t i = 0; i < NTP_SERVER_COUNT; i++) {
        esp_sntp_setservername(i, s_ntp_servers[i]);
    }
    esp_sntp_set_time_sync_notification_cb(on_sntp_sync);
    esp_sntp_init();
}

/* ============================================================
 * CALLBACK: retry timer
 * ============================================================ */

static void retry_timer_cb(void *arg)
{
    if (s_synced) return;

    /* Check if SNTP already synchronized */
    time_t now = time(NULL);
    if (now > 1700000000) {
        s_synced = true;
        mac_list_set_time_synced(true);
        ESP_LOGI(TAG, "SNTP synchronized (check): %lld", (long long)now);
        return;
    }

    ESP_LOGW(TAG, "SNTP not yet synchronized, retrying...");

    /* Restart SNTP */
    esp_sntp_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    start_sntp();

    /* Reschedule timer */
    esp_timer_start_once(s_retry_timer, (uint64_t)RETRY_LATER_MS * 1000ULL);
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

esp_err_t time_service_init(void)
{
    /* Check if time is already plausible (e.g. build time) */
    time_t now = time(NULL);
    if (now > 1700000000) {
        s_synced = true;
        mac_list_set_time_synced(true);
        ESP_LOGI(TAG, "time already synchronized: %lld", (long long)now);
        return ESP_OK;
    }

    /* Create retry timer (one-shot, reusable) */
    esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name = "sntp_retry",
    };
    esp_err_t ret = esp_timer_create(&timer_args, &s_retry_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Start SNTP */
    start_sntp();

    /* Schedule first retry after 10 seconds (if SNTP doesn't sync immediately) */
    esp_timer_start_once(s_retry_timer, (uint64_t)RETRY_INITIAL_MS * 1000ULL);

    ESP_LOGI(TAG, "time_service initialized (no task)");
    return ESP_OK;
}

bool time_service_is_synced(void)
{
    if (!s_synced) {
        /* Check if SNTP synchronized in the meantime */
        time_t now = time(NULL);
        if (now > 1700000000) {
            s_synced = true;
            mac_list_set_time_synced(true);
        }
    }
    return s_synced;
}

time_t time_service_now(void)
{
    if (!time_service_is_synced()) return 0;
    return time(NULL);
}