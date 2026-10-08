/*
 * NetworkWatcher - auth.c
 * Authentication implementation with PSA Crypto.
 */

#include "auth.h"
#include "config.h"
#include "storage.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "psa/crypto.h"

static const char *TAG = "auth";

/* ============================================================
 * INTERNAL STATE
 * ============================================================ */

static uint8_t  s_salt[AUTH_SALT_LEN];
static uint8_t  s_hash[AUTH_HASH_LEN];
static char     s_username[AUTH_USER_MAX_LEN + 1];
static bool     s_must_change_pass = false;

static char     s_token[AUTH_TOKEN_HEX_LEN + 1];
static int64_t  s_session_last_activity_us = 0;

static int      s_failed_attempts = 0;
static int64_t  s_lockout_until_us = 0;

/* ============================================================
 * HELPER: hex encoding
 * ============================================================ */

static void bytes_to_hex(const uint8_t *in, size_t in_len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < in_len; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[in_len * 2] = '\0';
}

/* ============================================================
 * HELPER: compute SHA-256(password || salt) with PSA Crypto
 * ============================================================ */

static esp_err_t compute_hash(const char *password,
                               const uint8_t *salt, size_t salt_len,
                               uint8_t *out_hash)
{
    psa_status_t status;

    /* Initialize PSA (idempotent) */
    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return ESP_FAIL;
    }

    /* Compute SHA-256 over password + salt */
    size_t hash_len = 0;
    uint8_t input[AUTH_PASS_MAX_LEN + AUTH_SALT_LEN];
    size_t input_len = strlen(password);

    if (input_len > AUTH_PASS_MAX_LEN) input_len = AUTH_PASS_MAX_LEN;
    memcpy(input, password, input_len);
    memcpy(input + input_len, salt, salt_len);
    input_len += salt_len;

    status = psa_hash_compute(PSA_ALG_SHA_256,
                              input, input_len,
                              out_hash, AUTH_HASH_LEN,
                              &hash_len);

    if (status != PSA_SUCCESS || hash_len != AUTH_HASH_LEN) {
        ESP_LOGE(TAG, "psa_hash_compute failed: %d", (int)status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ============================================================
 * HELPER: save credentials to NVS
 * ============================================================ */

static esp_err_t save_credentials(const char *user, const char *password)
{
    esp_fill_random(s_salt, sizeof(s_salt));

    esp_err_t ret = compute_hash(password, s_salt, sizeof(s_salt), s_hash);
    if (ret != ESP_OK) return ret;

    ret = storage_set_string(NW_NVS_KEY_ADMIN_USER, user);
    if (ret != ESP_OK) return ret;

    ret = storage_set_blob("admin_salt", s_salt, sizeof(s_salt));
    if (ret != ESP_OK) return ret;

    ret = storage_set_blob(NW_NVS_KEY_ADMIN_PASS_HASH, s_hash, sizeof(s_hash));
    if (ret != ESP_OK) return ret;

    strncpy(s_username, user, AUTH_USER_MAX_LEN);
    s_username[AUTH_USER_MAX_LEN] = '\0';

    return ESP_OK;
}

/* ============================================================
 * HELPER: load credentials from NVS
 * ============================================================ */

static esp_err_t load_credentials(void)
{
    esp_err_t ret = storage_get_string(NW_NVS_KEY_ADMIN_USER,
                                       s_username, sizeof(s_username));
    if (ret != ESP_OK) return ret;

    ret = storage_get_blob("admin_salt", s_salt, sizeof(s_salt));
    if (ret != ESP_OK) return ret;

    ret = storage_get_blob(NW_NVS_KEY_ADMIN_PASS_HASH, s_hash, sizeof(s_hash));
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/* ============================================================
 * INIT
 * ============================================================ */

esp_err_t auth_init(void)
{
    /* Initialize PSA Crypto */
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return ESP_FAIL;
    }

    esp_err_t ret = load_credentials();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "credentials loaded from NVS (user='%s')", s_username);

        storage_get_bool("must_change_pass", &s_must_change_pass, false);
        if (s_must_change_pass) {
            ESP_LOGW(TAG, "password change required on first login");
        }
    } else {
        ESP_LOGW(TAG, "no credentials found, creating default '%s'/'%s'",
                 NW_ADMIN_DEFAULT_USER, NW_ADMIN_DEFAULT_PASS);

        ret = save_credentials(NW_ADMIN_DEFAULT_USER, NW_ADMIN_DEFAULT_PASS);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "saving default credentials failed: %s",
                     esp_err_to_name(ret));
            return ret;
        }

        s_must_change_pass = true;
        storage_set_bool("must_change_pass", true);
    }

    memset(s_token, 0, sizeof(s_token));
    s_session_last_activity_us = 0;
    s_failed_attempts = 0;
    s_lockout_until_us = 0;

    ESP_LOGI(TAG, "auth initialized");
    return ESP_OK;
}

/* ============================================================
 * LOGIN
 * ============================================================ */

esp_err_t auth_login(const char *user, const char *pass,
                     char *out_token, size_t token_len)
{
    if (!user || !pass || !out_token || token_len < AUTH_TOKEN_HEX_LEN + 1) {
        return ESP_ERR_INVALID_ARG;
    }

    if (auth_is_locked_out()) {
        ESP_LOGW(TAG, "login blocked: lockout active (%d seconds remaining)",
                 auth_lockout_remaining());
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(user, s_username) != 0) {
        ESP_LOGW(TAG, "login failed: wrong username");
        s_failed_attempts++;
        if (s_failed_attempts >= AUTH_MAX_LOGIN_ATTEMPTS) {
            s_lockout_until_us = esp_timer_get_time()
                               + (int64_t)AUTH_LOCKOUT_SECONDS * 1000000LL;
            ESP_LOGE(TAG, "too many attempts, lockout for %d seconds",
                     AUTH_LOCKOUT_SECONDS);
        }
        return ESP_FAIL;
    }

    uint8_t computed[AUTH_HASH_LEN];
    esp_err_t ret = compute_hash(pass, s_salt, sizeof(s_salt), computed);
    if (ret != ESP_OK) return ESP_FAIL;

    if (memcmp(computed, s_hash, AUTH_HASH_LEN) != 0) {
        ESP_LOGW(TAG, "login failed: wrong password");
        s_failed_attempts++;
        if (s_failed_attempts >= AUTH_MAX_LOGIN_ATTEMPTS) {
            s_lockout_until_us = esp_timer_get_time()
                               + (int64_t)AUTH_LOCKOUT_SECONDS * 1000000LL;
            ESP_LOGE(TAG, "too many attempts, lockout for %d seconds",
                     AUTH_LOCKOUT_SECONDS);
        }
        return ESP_FAIL;
    }

    s_failed_attempts = 0;
    s_lockout_until_us = 0;

    uint8_t token_bytes[AUTH_TOKEN_LEN];
    esp_fill_random(token_bytes, sizeof(token_bytes));
    bytes_to_hex(token_bytes, sizeof(token_bytes), s_token);

    s_session_last_activity_us = esp_timer_get_time();

    strncpy(out_token, s_token, token_len - 1);
    out_token[token_len - 1] = '\0';

    ESP_LOGI(TAG, "login successful for '%s'", user);
    return ESP_OK;
}

/* ============================================================
 * LOGOUT
 * ============================================================ */

void auth_logout(const char *token)
{
    if (!token) return;
    if (strcmp(token, s_token) == 0) {
        memset(s_token, 0, sizeof(s_token));
        s_session_last_activity_us = 0;
        ESP_LOGI(TAG, "session terminated");
    }
}

/* ============================================================
 * SESSION CHECK
 * ============================================================ */

bool auth_check_session(const char *token)
{
    if (!token || s_token[0] == '\0') return false;
    if (strcmp(token, s_token) != 0) return false;

    int64_t now = esp_timer_get_time();
    int64_t elapsed_s = (now - s_session_last_activity_us) / 1000000LL;
    if (elapsed_s > NW_SESSION_TIMEOUT_S) {
        ESP_LOGI(TAG, "session expired (%lld seconds)", (long long)elapsed_s);
        memset(s_token, 0, sizeof(s_token));
        return false;
    }

    return true;
}

void auth_touch_session(const char *token)
{
    if (token && strcmp(token, s_token) == 0) {
        s_session_last_activity_us = esp_timer_get_time();
    }
}

/* ============================================================
 * PASSWORD CHANGE
 * ============================================================ */

esp_err_t auth_change_password(const char *old_pass, const char *new_pass)
{
    if (!old_pass || !new_pass) return ESP_ERR_INVALID_ARG;

    uint8_t computed[AUTH_HASH_LEN];
    esp_err_t ret = compute_hash(old_pass, s_salt, sizeof(s_salt), computed);
    if (ret != ESP_OK) return ESP_FAIL;

    if (memcmp(computed, s_hash, AUTH_HASH_LEN) != 0) {
        ESP_LOGW(TAG, "password change: wrong old password");
        return ESP_FAIL;
    }

    size_t len = strlen(new_pass);
    if (len < AUTH_PASS_MIN_LEN || len > AUTH_PASS_MAX_LEN) {
        ESP_LOGW(TAG, "password change: invalid length (%u)", (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }

    ret = save_credentials(s_username, new_pass);
    if (ret != ESP_OK) return ret;

    s_must_change_pass = false;
    storage_set_bool("must_change_pass", false);

    ESP_LOGI(TAG, "password changed successfully");
    return ESP_OK;
}

/* ============================================================
 * STATUS
 * ============================================================ */

bool auth_must_change_password(void)
{
    return s_must_change_pass;
}

bool auth_is_locked_out(void)
{
    if (s_lockout_until_us == 0) return false;
    int64_t now = esp_timer_get_time();
    if (now >= s_lockout_until_us) {
        s_lockout_until_us = 0;
        s_failed_attempts = 0;
        return false;
    }
    return true;
}

int auth_lockout_remaining(void)
{
    if (!auth_is_locked_out()) return 0;
    int64_t now = esp_timer_get_time();
    int64_t remaining = (s_lockout_until_us - now) / 1000000LL;
    return (int)remaining;
}

esp_err_t auth_get_username(char *out, size_t len)
{
    if (!out || len == 0) return ESP_ERR_INVALID_ARG;
    strncpy(out, s_username, len - 1);
    out[len - 1] = '\0';
    return ESP_OK;
}

void auth_clear_sessions(void)
{
    memset(s_token, 0, sizeof(s_token));
    s_session_last_activity_us = 0;
}