/*
 * NetworkWatcher - storage.c
 * NVS + LittleFS wrapper implementation.
 */

#include "storage.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_littlefs.h"
#include "esp_partition.h"

static const char *TAG = "storage";

static bool s_initialized = false;

/* ============================================================
 * INITIALIZATION
 * ============================================================ */

esp_err_t storage_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "storage already initialized");
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupted, erasing and reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "NVS initialized");

    esp_vfs_littlefs_conf_t conf = {
        .base_path = NW_FS_BASE,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "LittleFS mount failed and format failed");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Partition 'storage' not found in partition table");
        } else {
            ESP_LOGE(TAG, "LittleFS register failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "LittleFS mounted on %s", NW_FS_BASE);
    }

    s_initialized = true;
    return ESP_OK;
}

/* ============================================================
 * PRIVATE HELPERS
 * ============================================================ */

static esp_err_t nvs_open_rw(nvs_handle_t *handle)
{
    return nvs_open(NW_NVS_NAMESPACE, NVS_READWRITE, handle);
}

static esp_err_t nvs_open_ro(nvs_handle_t *handle)
{
    return nvs_open(NW_NVS_NAMESPACE, NVS_READONLY, handle);
}

/* ============================================================
 * NVS - STRINGS
 * ============================================================ */

esp_err_t storage_get_string(const char *key, char *out, size_t len)
{
    if (!key || !out || len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open_ro(&h);
    if (ret != ESP_OK) return ret;

    size_t required = len;
    ret = nvs_get_str(h, key, out, &required);
    nvs_close(h);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "key '%s' not found", key);
        return ret;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_str('%s') error: %s", key, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t storage_set_string(const char *key, const char *value)
{
    if (!key || !value) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open_rw(&h);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(h, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_str('%s') error: %s", key, esp_err_to_name(ret));
    }
    return ret;
}

/* ============================================================
 * NVS - INTEGERS
 * ============================================================ */

esp_err_t storage_get_int(const char *key, int32_t *out, int32_t default_v)
{
    if (!key || !out) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open_ro(&h);
    if (ret != ESP_OK) {
        *out = default_v;
        return ret;
    }

    int32_t v = default_v;
    ret = nvs_get_i32(h, key, &v);
    nvs_close(h);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *out = default_v;
        return ESP_OK;
    }
    if (ret == ESP_OK) {
        *out = v;
    }
    return ret;
}

esp_err_t storage_set_int(const char *key, int32_t value)
{
    if (!key) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open_rw(&h);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_i32(h, key, value);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

/* ============================================================
 * NVS - BOOL
 * ============================================================ */

esp_err_t storage_get_bool(const char *key, bool *out, bool default_v)
{
    int32_t v = default_v ? 1 : 0;
    esp_err_t ret = storage_get_int(key, &v, default_v ? 1 : 0);
    if (out) *out = (v != 0);
    return ret;
}

esp_err_t storage_set_bool(const char *key, bool value)
{
    return storage_set_int(key, value ? 1 : 0);
}

/* ============================================================
 * NVS - BLOB
 * ============================================================ */

esp_err_t storage_get_blob(const char *key, void *out, size_t len)
{
    if (!key || !out || len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open_ro(&h);
    if (ret != ESP_OK) return ret;

    size_t required = len;
    ret = nvs_get_blob(h, key, out, &required);
    nvs_close(h);
    return ret;
}

esp_err_t storage_set_blob(const char *key, const void *data, size_t len)
{
    if (!key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open_rw(&h);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_blob(h, key, data, len);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

/* ============================================================
 * NVS - ERASE
 * ============================================================ */

esp_err_t storage_erase_key(const char *key)
{
    if (!key) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open_rw(&h);
    if (ret != ESP_OK) return ret;

    ret = nvs_erase_key(h, key);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t storage_erase_all(void)
{
    ESP_LOGW(TAG, "=== FACTORY RESET ===");

    nvs_handle_t h;
    esp_err_t ret;

    /* Erase ONLY the custom namespace 'nw_config'.
     * We do NOT touch 'nvs.net80211' — erasing that namespace
     * must be done via esp_wifi_restore() (see main.c), because
     * it is managed internally by ESP-IDF and erasing it manually
     * corrupts NVS and prevents USB enumeration at boot. */
    ret = nvs_open(NW_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret == ESP_OK) {
        ret = nvs_erase_all(h);
        if (ret == ESP_OK) ret = nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "NVS namespace '%s' erased", NW_NVS_NAMESPACE);
    } else {
        ESP_LOGW(TAG, "cannot open '%s': %s",
                 NW_NVS_NAMESPACE, esp_err_to_name(ret));
    }

    /* Erase list files in LittleFS */
    storage_delete_file(NW_FS_LIST_WHITELIST);
    storage_delete_file(NW_FS_LIST_BLACKLIST);
    storage_delete_file(NW_FS_LIST_UNKNOWN);
    ESP_LOGW(TAG, "LittleFS list files erased");

    ESP_LOGW(TAG, "=== RESET COMPLETED ===");
    return ESP_OK;
}

/* ============================================================
 * LITTLEFS - FILES
 * ============================================================ */

esp_err_t storage_read_file(const char *path, char **out, size_t *len)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (len) *len = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGD(TAG, "file '%s' cannot be opened", path);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ESP_FAIL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return ESP_FAIL; }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (rd != (size_t)size) {
        free(buf);
        return ESP_FAIL;
    }
    buf[size] = '\0';

    *out = buf;
    if (len) *len = (size_t)size;
    return ESP_OK;
}

esp_err_t storage_write_file(const char *path, const char *data, size_t len)
{
    if (!path || !data) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot create '%s'", path);
        return ESP_FAIL;
    }

    size_t wr = fwrite(data, 1, len, f);
    fclose(f);

    if (wr != len) {
        ESP_LOGE(TAG, "partial write on '%s' (%u/%u)", path,
                 (unsigned)wr, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t storage_delete_file(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (remove(path) == 0) return ESP_OK;
    return ESP_ERR_NOT_FOUND;
}

bool storage_file_exists(const char *path)
{
    if (!path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

long storage_file_size(const char *path)
{
    if (!path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    fclose(f);
    return size;
}

/* ============================================================
 * LITTLEFS - INFO
 * ============================================================ */

esp_err_t storage_get_fs_info(size_t *total, size_t *used)
{
    if (!total || !used) return ESP_ERR_INVALID_ARG;
    return esp_littlefs_info("storage", total, used);
}