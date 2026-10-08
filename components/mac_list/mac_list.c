/*
 * NetworkWatcher - mac_list.c
 * MAC list management (whitelist, blacklist, unknown).
 *
 * Hybrid timestamps: every timestamp has a flag indicating whether it is
 * Unix time (seconds since epoch) or uptime (seconds since boot).
 *
 * Fase 3.3:
 *  - In-RAM IP cache (MAC -> last IP seen)
 *  - Populated by mac_list_process_scan()
 *  - Exposed via mac_list_get_ip()
 */

#include "mac_list.h"
#include "config.h"
#include "storage.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "mac_list";

/* ============================================================
 * INTERNAL STATE
 * ============================================================ */

typedef struct {
    nw_list_entry_t entries[NW_LIST_MAX_ENTRIES];
    size_t          count;
    bool            loaded;
} mac_list_t;

static mac_list_t s_lists[3];
static bool       s_initialized   = false;
static bool       s_time_synced   = false;
static bool       s_blacklist_present = false;

/* IP cache: MAC -> last IP seen (in RAM, not persisted) */
#define IP_CACHE_SIZE 32

typedef struct {
    uint8_t  mac[6];
    uint32_t ip;       /* host byte order, 0 = empty slot */
    bool     valid;
} ip_cache_entry_t;

static ip_cache_entry_t s_ip_cache[IP_CACHE_SIZE];

static int list_index(nw_list_type_t type)
{
    switch (type) {
        case NW_LIST_WHITELIST: return 0;
        case NW_LIST_BLACKLIST: return 1;
        case NW_LIST_UNKNOWN:   return 2;
        default:                return -1;
    }
}

static const char *list_path(nw_list_type_t type)
{
    switch (type) {
        case NW_LIST_WHITELIST: return NW_FS_LIST_WHITELIST;
        case NW_LIST_BLACKLIST: return NW_FS_LIST_BLACKLIST;
        case NW_LIST_UNKNOWN:   return NW_FS_LIST_UNKNOWN;
        default:                return NULL;
    }
}

const char *mac_list_type_name(nw_list_type_t type)
{
    switch (type) {
        case NW_LIST_WHITELIST: return "whitelist";
        case NW_LIST_BLACKLIST: return "blacklist";
        case NW_LIST_UNKNOWN:   return "unknown";
        default:                return "invalid";
    }
}

int mac_list_type_from_name(const char *name)
{
    if (!name) return -1;
    if (strcmp(name, "whitelist") == 0) return NW_LIST_WHITELIST;
    if (strcmp(name, "blacklist") == 0) return NW_LIST_BLACKLIST;
    if (strcmp(name, "unknown")   == 0) return NW_LIST_UNKNOWN;
    return -1;
}

/* ============================================================
 * HELPER: current timestamp (hybrid)
 * ============================================================ */

static uint32_t now_timestamp(bool *is_unix)
{
    if (s_time_synced) {
        time_t t = time(NULL);
        if (t > 1700000000) {
            *is_unix = true;
            return (uint32_t)t;
        }
    }
    *is_unix = false;
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

/* ============================================================
 * MAC HELPERS
 * ============================================================ */

void mac_to_string(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool string_to_mac(const char *str, uint8_t out[6])
{
    if (!str) return false;
    unsigned int m[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (m[i] > 0xFF) return false;
        out[i] = (uint8_t)m[i];
    }
    return true;
}

static bool mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

/* ============================================================
 * IP CACHE
 * ============================================================ */

static void ip_cache_update(const uint8_t mac[6], uint32_t ip)
{
    /* Look for MAC already in cache */
    for (size_t i = 0; i < IP_CACHE_SIZE; i++) {
        if (s_ip_cache[i].valid && mac_equal(s_ip_cache[i].mac, mac)) {
            s_ip_cache[i].ip = ip;
            return;
        }
    }

    /* New entry: find first free slot */
    for (size_t i = 0; i < IP_CACHE_SIZE; i++) {
        if (!s_ip_cache[i].valid) {
            memcpy(s_ip_cache[i].mac, mac, 6);
            s_ip_cache[i].ip = ip;
            s_ip_cache[i].valid = true;
            return;
        }
    }

    /* Cache full: overwrite the first slot (approximate FIFO) */
    memcpy(s_ip_cache[0].mac, mac, 6);
    s_ip_cache[0].ip = ip;
    s_ip_cache[0].valid = true;
}

esp_err_t mac_list_get_ip(const uint8_t mac[6], char *out, size_t out_len)
{
    if (!mac || !out || out_len < 8) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';

    for (size_t i = 0; i < IP_CACHE_SIZE; i++) {
        if (s_ip_cache[i].valid && mac_equal(s_ip_cache[i].mac, mac)) {
            uint32_t ip = s_ip_cache[i].ip;
            snprintf(out, out_len, "%u.%u.%u.%u",
                     (unsigned)((ip >> 24) & 0xFF),
                     (unsigned)((ip >> 16) & 0xFF),
                     (unsigned)((ip >> 8) & 0xFF),
                     (unsigned)(ip & 0xFF));
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

/* ============================================================
 * DESCRIPTION VALIDATION
 * ============================================================ */

static bool is_desc_valid(const char *desc)
{
    if (!desc) return false;
    size_t len = strlen(desc);
    if (len > NW_LIST_DESC_MAX_LEN) return false;

    for (size_t i = 0; i < len; i++) {
        char c = desc[i];
        if (!isalnum((unsigned char)c) && c != ' ' && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

/* ============================================================
 * MINIMAL JSON PARSER (for load)
 * ============================================================ */

static bool json_extract_bool(const char *json, const char *key, bool *out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static long json_extract_long(const char *json, const char *key, long default_v)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_v;
    p += strlen(pattern);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    return strtol(p, NULL, 10);
}

static int json_extract_string(const char *json, const char *key,
                                char *out, size_t out_len)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            out[i++] = *p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return (int)i;
}

static void json_escape_str(char *dst, size_t dst_len, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_len; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if ((unsigned char)c < 0x20) {
            /* skip control chars */
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

/* ============================================================
 * LOAD / SAVE
 * ============================================================ */

esp_err_t mac_list_load(nw_list_type_t type)
{
    int idx = list_index(type);
    if (idx < 0) return ESP_ERR_INVALID_ARG;

    const char *path = list_path(type);
    if (!path) return ESP_ERR_INVALID_ARG;

    mac_list_t *list = &s_lists[idx];
    memset(list, 0, sizeof(*list));

    char *content = NULL;
    size_t len = 0;
    esp_err_t ret = storage_read_file(path, &content, &len);

    if (ret == ESP_ERR_NOT_FOUND || !content) {
        ESP_LOGI(TAG, "list %s: file not found, creating empty", mac_list_type_name(type));
        list->loaded = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "list %s: read error (%s)",
                 mac_list_type_name(type), esp_err_to_name(ret));
        list->loaded = true;
        return ret;
    }

    const char *p = content;

    bool ts = false;
    if (json_extract_bool(p, "time_synced", &ts)) {
        s_time_synced = ts;
    }

    const char *entries_start = strstr(p, "\"entries\"");
    if (entries_start) {
        entries_start = strchr(entries_start, '[');
    }

    if (entries_start) {
        const char *cur = entries_start;
        while (list->count < NW_LIST_MAX_ENTRIES) {
            cur = strchr(cur, '{');
            if (!cur) break;

            const char *end = strchr(cur, '}');
            if (!end) break;

            char mac_str[32] = {0};
            char desc[NW_LIST_DESC_MAX_LEN + 1] = {0};

            size_t block_len = (size_t)(end - cur) + 1;
            if (block_len > 512) block_len = 512;
            char block[520];
            memcpy(block, cur, block_len);
            block[block_len] = '\0';

            json_extract_string(block, "mac", mac_str, sizeof(mac_str));
            json_extract_string(block, "desc", desc, sizeof(desc));

            long first_seen = json_extract_long(block, "first_seen", 0);
            long last_seen  = json_extract_long(block, "last_seen", 0);

            bool fs_unix = false;
            bool ls_unix = false;
            json_extract_bool(block, "first_seen_is_unix", &fs_unix);
            json_extract_bool(block, "last_seen_is_unix", &ls_unix);

            uint8_t mac[6];
            if (string_to_mac(mac_str, mac)) {
                nw_list_entry_t *e = &list->entries[list->count];
                memcpy(e->mac.bytes, mac, 6);
                strncpy(e->desc, desc, NW_LIST_DESC_MAX_LEN);
                e->desc[NW_LIST_DESC_MAX_LEN] = '\0';
                e->first_seen = (uint32_t)first_seen;
                e->last_seen  = (uint32_t)last_seen;
                e->first_seen_is_unix = fs_unix;
                e->last_seen_is_unix  = ls_unix;
                e->valid = true;
                list->count++;
            }

            cur = end + 1;
        }
    }

    free(content);
    list->loaded = true;

    ESP_LOGI(TAG, "list %s loaded: %u entries",
             mac_list_type_name(type), (unsigned)list->count);
    return ESP_OK;
}

esp_err_t mac_list_save(nw_list_type_t type)
{
    int idx = list_index(type);
    if (idx < 0) return ESP_ERR_INVALID_ARG;

    const char *path = list_path(type);
    if (!path) return ESP_ERR_INVALID_ARG;

    mac_list_t *list = &s_lists[idx];

    size_t bufsize = 256 + list->count * 320;
    if (bufsize < 512) bufsize = 512;

    char *json = malloc(bufsize);
    if (!json) {
        ESP_LOGE(TAG, "malloc %u failed for saving %s",
                 (unsigned)bufsize, mac_list_type_name(type));
        return ESP_ERR_NO_MEM;
    }

    size_t pos = 0;
    pos += snprintf(json + pos, bufsize - pos,
                    "{\n  \"version\": 1,\n  \"time_synced\": %s,\n  \"entries\": [\n",
                    s_time_synced ? "true" : "false");

    for (size_t i = 0; i < list->count; i++) {
        nw_list_entry_t *e = &list->entries[i];
        char mac_str[18];
        mac_to_string(e->mac.bytes, mac_str);

        char desc_esc[NW_LIST_DESC_MAX_LEN * 2 + 2];
        json_escape_str(desc_esc, sizeof(desc_esc), e->desc);

        pos += snprintf(json + pos, bufsize - pos,
                        "%s    {\"mac\":\"%s\",\"desc\":\"%s\","
                        "\"first_seen\":%u,\"last_seen\":%u,"
                        "\"first_seen_is_unix\":%s,\"last_seen_is_unix\":%s}",
                        i > 0 ? ",\n" : "",
                        mac_str, desc_esc,
                        (unsigned)e->first_seen,
                        (unsigned)e->last_seen,
                        e->first_seen_is_unix ? "true" : "false",
                        e->last_seen_is_unix ? "true" : "false");
    }

    pos += snprintf(json + pos, bufsize - pos, "\n  ]\n}\n");

    esp_err_t ret = storage_write_file(path, json, pos);
    free(json);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "saving %s failed: %s",
                 mac_list_type_name(type), esp_err_to_name(ret));
    }
    return ret;
}

/* ============================================================
 * INIT
 * ============================================================ */

esp_err_t mac_list_init(void)
{
    if (s_initialized) return ESP_OK;

    for (int i = 0; i < 3; i++) {
        s_lists[i].count = 0;
        s_lists[i].loaded = false;
    }
    memset(s_ip_cache, 0, sizeof(s_ip_cache));

    mac_list_load(NW_LIST_WHITELIST);
    mac_list_load(NW_LIST_BLACKLIST);
    mac_list_load(NW_LIST_UNKNOWN);

    s_initialized = true;
    ESP_LOGI(TAG, "mac_list initialized");
    return ESP_OK;
}

/* ============================================================
 * CRUD
 * ============================================================ */

bool mac_list_contains(nw_list_type_t type, const uint8_t mac[6])
{
    int idx = list_index(type);
    if (idx < 0) return false;

    mac_list_t *list = &s_lists[idx];
    for (size_t i = 0; i < list->count; i++) {
        if (mac_equal(list->entries[i].mac.bytes, mac)) return true;
    }
    return false;
}

esp_err_t mac_list_add(nw_list_type_t type, const uint8_t mac[6], const char *desc)
{
    int idx = list_index(type);
    if (idx < 0) return ESP_ERR_INVALID_ARG;

    mac_list_t *list = &s_lists[idx];

    for (size_t i = 0; i < list->count; i++) {
        if (mac_equal(list->entries[i].mac.bytes, mac)) {
            bool ls_unix;
            uint32_t ls = now_timestamp(&ls_unix);
            list->entries[i].last_seen = ls;
            list->entries[i].last_seen_is_unix = ls_unix;

            if (desc && desc[0] && strlen(desc) <= NW_LIST_DESC_MAX_LEN) {
                if (is_desc_valid(desc)) {
                    strncpy(list->entries[i].desc, desc, NW_LIST_DESC_MAX_LEN);
                    list->entries[i].desc[NW_LIST_DESC_MAX_LEN] = '\0';
                }
            }
            return ESP_OK;
        }
    }

    if (list->count >= NW_LIST_MAX_ENTRIES) {
        ESP_LOGW(TAG, "list %s full (%d entries)",
                 mac_list_type_name(type), NW_LIST_MAX_ENTRIES);
        return ESP_ERR_NO_MEM;
    }

    nw_list_entry_t *e = &list->entries[list->count];
    memcpy(e->mac.bytes, mac, 6);
    if (desc && is_desc_valid(desc)) {
        strncpy(e->desc, desc, NW_LIST_DESC_MAX_LEN);
        e->desc[NW_LIST_DESC_MAX_LEN] = '\0';
    } else {
        e->desc[0] = '\0';
    }

    bool fs_unix;
    uint32_t ts = now_timestamp(&fs_unix);
    e->first_seen = ts;
    e->last_seen = ts;
    e->first_seen_is_unix = fs_unix;
    e->last_seen_is_unix = fs_unix;
    e->valid = true;
    list->count++;

    return ESP_OK;
}

esp_err_t mac_list_remove(nw_list_type_t type, const uint8_t mac[6])
{
    int idx = list_index(type);
    if (idx < 0) return ESP_ERR_INVALID_ARG;

    mac_list_t *list = &s_lists[idx];

    for (size_t i = 0; i < list->count; i++) {
        if (mac_equal(list->entries[i].mac.bytes, mac)) {
            for (size_t j = i; j < list->count - 1; j++) {
                list->entries[j] = list->entries[j + 1];
            }
            list->count--;
            memset(&list->entries[list->count], 0, sizeof(nw_list_entry_t));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t mac_list_move(nw_list_type_t from, nw_list_type_t to, const uint8_t mac[6])
{
    if (from == to) return ESP_OK;

    int from_idx = list_index(from);
    int to_idx   = list_index(to);
    if (from_idx < 0 || to_idx < 0) return ESP_ERR_INVALID_ARG;

    mac_list_t *src = &s_lists[from_idx];
    mac_list_t *dst = &s_lists[to_idx];

    nw_list_entry_t *found = NULL;
    for (size_t i = 0; i < src->count; i++) {
        if (mac_equal(src->entries[i].mac.bytes, mac)) {
            found = &src->entries[i];
            break;
        }
    }
    if (!found) return ESP_ERR_NOT_FOUND;

    if (dst->count >= NW_LIST_MAX_ENTRIES) {
        ESP_LOGW(TAG, "destination list %s full", mac_list_type_name(to));
        return ESP_ERR_NO_MEM;
    }

    nw_list_entry_t *e = &dst->entries[dst->count];
    *e = *found;
    bool ls_unix;
    uint32_t ls = now_timestamp(&ls_unix);
    e->last_seen = ls;
    e->last_seen_is_unix = ls_unix;
    dst->count++;

    mac_list_remove(from, mac);
    return ESP_OK;
}

esp_err_t mac_list_update_desc(nw_list_type_t type, const uint8_t mac[6], const char *desc)
{
    if (!is_desc_valid(desc)) return ESP_ERR_INVALID_ARG;

    int idx = list_index(type);
    if (idx < 0) return ESP_ERR_INVALID_ARG;

    mac_list_t *list = &s_lists[idx];
    for (size_t i = 0; i < list->count; i++) {
        if (mac_equal(list->entries[i].mac.bytes, mac)) {
            strncpy(list->entries[i].desc, desc, NW_LIST_DESC_MAX_LEN);
            list->entries[i].desc[NW_LIST_DESC_MAX_LEN] = '\0';
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t mac_list_clear(nw_list_type_t type)
{
    int idx = list_index(type);
    if (idx < 0) return ESP_ERR_INVALID_ARG;

    memset(&s_lists[idx], 0, sizeof(mac_list_t));
    s_lists[idx].loaded = true;
    return ESP_OK;
}

/* ============================================================
 * QUERY
 * ============================================================ */

esp_err_t mac_list_find(const uint8_t mac[6], nw_list_type_t *found_in)
{
    if (mac_list_contains(NW_LIST_WHITELIST, mac)) {
        if (found_in) *found_in = NW_LIST_WHITELIST;
        return ESP_OK;
    }
    if (mac_list_contains(NW_LIST_BLACKLIST, mac)) {
        if (found_in) *found_in = NW_LIST_BLACKLIST;
        return ESP_OK;
    }
    if (mac_list_contains(NW_LIST_UNKNOWN, mac)) {
        if (found_in) *found_in = NW_LIST_UNKNOWN;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

size_t mac_list_count(nw_list_type_t type)
{
    int idx = list_index(type);
    if (idx < 0) return 0;
    return s_lists[idx].count;
}

const nw_list_entry_t *mac_list_get(nw_list_type_t type, size_t index)
{
    int idx = list_index(type);
    if (idx < 0) return NULL;
    if (index >= s_lists[idx].count) return NULL;
    return &s_lists[idx].entries[index];
}

/* ============================================================
 * PROCESS SCAN
 * ============================================================ */

int mac_list_process_scan(const nw_arp_scan_batch_t *scan,
                          nw_scan_delta_t *delta_out)
{
    if (!scan || !scan->valid) {
        if (delta_out) memset(delta_out, 0, sizeof(*delta_out));
        return 0;
    }

    if (delta_out) memset(delta_out, 0, sizeof(*delta_out));

    int new_count = 0;
    bool bl_present = false;
    bool lists_changed = false;

    for (size_t i = 0; i < scan->count; i++) {
        const uint8_t *mac = scan->entries[i].mac.bytes;
        uint32_t ip = scan->entries[i].ip;

        /* Update IP cache (for all MACs seen, regardless of list) */
        ip_cache_update(mac, ip);

        nw_list_type_t found_in;
        esp_err_t ret = mac_list_find(mac, &found_in);

        if (ret == ESP_OK) {
            if (found_in == NW_LIST_BLACKLIST) {
                bl_present = true;

                if (delta_out && delta_out->blacklist_count < NW_SCAN_DELTA_MAX) {
                    nw_scan_delta_entry_t *e =
                        &delta_out->blacklist[delta_out->blacklist_count];
                    mac_to_string(mac, e->mac);
                    snprintf(e->ip, sizeof(e->ip), "%u.%u.%u.%u",
                             (unsigned)((ip >> 24) & 0xFF),
                             (unsigned)((ip >> 16) & 0xFF),
                             (unsigned)((ip >> 8) & 0xFF),
                             (unsigned)(ip & 0xFF));
                    delta_out->blacklist_count++;
                }
            }
            if (mac_list_add(found_in, mac, NULL) == ESP_OK) {
                lists_changed = true;
            }
        } else {
            if (mac_list_add(NW_LIST_UNKNOWN, mac, NULL) == ESP_OK) {
                new_count++;
                lists_changed = true;

                if (delta_out && delta_out->new_unknown_count < NW_SCAN_DELTA_MAX) {
                    nw_scan_delta_entry_t *e =
                        &delta_out->new_unknown[delta_out->new_unknown_count];
                    mac_to_string(mac, e->mac);
                    snprintf(e->ip, sizeof(e->ip), "%u.%u.%u.%u",
                             (unsigned)((ip >> 24) & 0xFF),
                             (unsigned)((ip >> 16) & 0xFF),
                             (unsigned)((ip >> 8) & 0xFF),
                             (unsigned)(ip & 0xFF));
                    delta_out->new_unknown_count++;
                }

                char mac_str[18];
                mac_to_string(mac, mac_str);
                ESP_LOGI(TAG, "new device: %s", mac_str);
            }
        }
    }

    s_blacklist_present = bl_present;

    if (lists_changed) {
        mac_list_save(NW_LIST_WHITELIST);
        mac_list_save(NW_LIST_BLACKLIST);
        mac_list_save(NW_LIST_UNKNOWN);
    }

    return new_count;
}

bool mac_list_blacklist_present(void)
{
    return s_blacklist_present;
}

bool mac_list_is_time_synced(void)
{
    return s_time_synced;
}

void mac_list_set_time_synced(bool synced)
{
    if (s_time_synced != synced) {
        s_time_synced = synced;
        ESP_LOGI(TAG, "time_synced: %s", synced ? "true" : "false");
    }
}