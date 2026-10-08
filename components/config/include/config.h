/*
 * NetworkWatcher - config.h
 * Costanti, tipi e definizioni condivise da tutti i componenti.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * VERSIONE E NOME PROGETTO
 * ============================================================ */
#define NW_PROJECT_NAME         "NetworkWatcher"
#define NW_PROJECT_VERSION      "1.0.0"

/* ============================================================
 * HARDWARE
 * ============================================================ */

#define NW_LED_GPIO             15
#define NW_LED_ACTIVE_LOW       0
#define NW_BUTTON_GPIO          0

/* ============================================================
 * TIMING PULSANTE
 * ============================================================ */
#define NW_BUTTON_DEBOUNCE_MS           50
#define NW_BUTTON_LONG_PRESS_MS         5000
#define NW_BUTTON_SEQUENCE_GAP_MS       1000
#define NW_BUTTON_SEQUENCE_COUNT        3
#define NW_BUTTON_RESET_CONFIRM_MS      5000

/* ============================================================
 * WIFI - ACCESS POINT
 * ============================================================ */
#define NW_AP_SSID              "Network Watcher"
#define NW_AP_PASSWORD          "12345678"
#define NW_AP_CHANNEL           1
#define NW_AP_MAX_CONN          4

/* ============================================================
 * WIFI - STATION
 * ============================================================ */
#define NW_WIFI_SCAN_MAX_AP     20
#define NW_WIFI_MAX_SAVED       1
#define NW_WIFI_CONNECT_TIMEOUT_MS      15000

#define NW_WIFI_RETRY_FAST_COUNT        5
#define NW_WIFI_RETRY_FAST_MS           5000
#define NW_WIFI_RETRY_MEDIUM_COUNT      15
#define NW_WIFI_RETRY_MEDIUM_MS         30000
#define NW_WIFI_RETRY_SLOW_MS           300000

/* ============================================================
 * SCANSIONE ARP
 * ============================================================ */
#define NW_SCAN_INTERVAL_DEFAULT_MIN    5
#define NW_SCAN_INTERVAL_MIN_MIN        1
#define NW_SCAN_INTERVAL_MAX_MIN        60
#define NW_SCAN_PING_TIMEOUT_MS         200
#define NW_SCAN_PING_PARALLEL           4

/* ============================================================
 * LISTE MAC
 * ============================================================ */
#define NW_LIST_MAX_ENTRIES             100
#define NW_LIST_DESC_MAX_LEN            32
#define NW_MAC_STR_LEN                  18

/* ============================================================
 * WEB / SESSIONE
 * ============================================================ */
#define NW_SESSION_TIMEOUT_S            1800
#define NW_HTTP_PORT                    80
#define NW_HTTP_MAX_BODY                4096
#define NW_ADMIN_USER_MAX_LEN           32
#define NW_ADMIN_PASS_MAX_LEN           64
#define NW_ADMIN_DEFAULT_USER           "admin"
#define NW_ADMIN_DEFAULT_PASS           "admin"

/* ============================================================
 * TELEGRAM
 * ============================================================ */
#define NW_TG_TOKEN_MAX_LEN             64
#define NW_TG_CHATID_MAX_LEN            32
#define NW_TG_QUEUE_SIZE                3        /* era 10: -14 KB heap */
#define NW_TG_MAX_RETRY                 3
#define NW_TG_HTTP_TIMEOUT_MS           10000

/* ============================================================
 * WEBHOOK GENERICO (Fase 3.4)
 * ============================================================ */
#define NW_WH_URL_MAX_LEN               256
#define NW_WH_TOKEN_MAX_LEN             128
#define NW_WH_QUEUE_SIZE                3        /* era 10: -14 KB heap */
#define NW_WH_MAX_RETRY                 3
#define NW_WH_HTTP_TIMEOUT_MS           10000

/* ============================================================
 * STORAGE - CHIAVI NVS
 * ============================================================ */
#define NW_NVS_NAMESPACE                "nw_config"

#define NW_NVS_KEY_WIFI_SSID            "wifi_ssid"
#define NW_NVS_KEY_WIFI_PASS            "wifi_pass"
#define NW_NVS_KEY_WIFI_CONFIGURED      "wifi_cfg"

#define NW_NVS_KEY_ADMIN_USER           "admin_user"
#define NW_NVS_KEY_ADMIN_PASS_HASH      "admin_hash"

#define NW_NVS_KEY_TG_TOKEN             "tg_token"
#define NW_NVS_KEY_TG_CHATID            "tg_chatid"

#define NW_NVS_KEY_WH_URL               "wh_url"
#define NW_NVS_KEY_WH_TOKEN             "wh_token"

#define NW_NVS_KEY_SCAN_INTERVAL        "scan_int"
#define NW_NVS_KEY_MDNS_HOSTNAME        "mdns_host"
#define NW_NVS_KEY_FIRST_BOOT           "first_boot"

/* ============================================================
 * STORAGE - PERCORSI LITTLEFS
 * ============================================================ */
#define NW_FS_BASE                     "/storage"
#define NW_FS_LIST_WHITELIST           "/storage/whitelist.json"
#define NW_FS_LIST_BLACKLIST           "/storage/blacklist.json"
#define NW_FS_LIST_UNKNOWN             "/storage/unknown.json"
#define NW_FS_BACKUP_DIR               "/storage/backup"
#define NW_FS_LOG_FILE                 "/storage/log.txt"

/* ============================================================
 * MDNS
 * ============================================================ */
#define NW_MDNS_HOSTNAME_DEFAULT       "network-watcher"

/* ============================================================
 * LOG
 * ============================================================ */
#define NW_LOG_RING_BUFFER_SIZE        4096

/* ============================================================
 * TIPI CONDIVISI
 * ============================================================ */

typedef enum {
    NW_STATE_BOOT = 0,
    NW_STATE_AP_MODE,
    NW_STATE_STA_CONNECTING,
    NW_STATE_STA_CONNECTED,
    NW_STATE_STA_RETRY,
    NW_STATE_ERROR,
} nw_state_t;

typedef enum {
    NW_LIST_WHITELIST = 0,
    NW_LIST_BLACKLIST,
    NW_LIST_UNKNOWN,
} nw_list_type_t;

typedef struct {
    uint8_t bytes[6];
} nw_mac_t;

typedef struct {
    nw_mac_t mac;
    char     desc[NW_LIST_DESC_MAX_LEN + 1];
    uint32_t first_seen;
    uint32_t last_seen;
    bool     first_seen_is_unix;
    bool     last_seen_is_unix;
    bool     valid;
} nw_list_entry_t;

typedef struct {
    nw_mac_t mac;
    uint32_t ip;
    uint32_t last_seen;
    bool     valid;
} nw_scan_result_t;

/* ============================================================
 * ARP SCANNER - TIPI CONDIVISI
 * ============================================================ */

#define ARP_SCAN_MAX_RESULTS    64
#define ARP_SCAN_PING_TIMEOUT   200

typedef struct {
    nw_scan_result_t entries[ARP_SCAN_MAX_RESULTS];
    size_t           count;
    uint32_t         duration_ms;
    uint32_t         scanned_ips;
    bool             valid;
} nw_arp_scan_batch_t;

/* ============================================================
 * DELTA SCANSIONE
 * ============================================================ */

#define NW_SCAN_DELTA_MAX   32

typedef struct {
    char mac[NW_MAC_STR_LEN];
    char ip[16];
} nw_scan_delta_entry_t;

typedef struct {
    nw_scan_delta_entry_t new_unknown[NW_SCAN_DELTA_MAX];
    int                   new_unknown_count;

    nw_scan_delta_entry_t blacklist[NW_SCAN_DELTA_MAX];
    int                   blacklist_count;
} nw_scan_delta_t;

/* ============================================================
 * COSTANTI DI DEFAULT
 * ============================================================ */

#define NW_DEFAULT_SCAN_INTERVAL_MIN    NW_SCAN_INTERVAL_DEFAULT_MIN
#define NW_DEFAULT_WIFI_TIMEOUT_S       15

#ifdef __cplusplus
}
#endif