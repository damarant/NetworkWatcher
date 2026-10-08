/*
 * NetworkWatcher - web_server.c
 * HTTP server with cookie-based session authentication.
 *
 * Fase 2.7: Telegram
 * Fase 3.1: Backup/Restore
 * Fase 3.2: Bulk move MAC
 * Fase 3.3: Vendor OUI + IP
 * Fase 3.4: Generic webhook
 * Fase 3.5: Log viewer
 */

#include "web_server.h"
#include "config.h"
#include "auth.h"
#include "wifi_manager.h"
#include "arp_scanner.h"
#include "mac_list.h"
#include "settings.h"
#include "led.h"
#include "storage.h"
#include "telegram.h"
#include "webhook.h"
#include "oui_lookup.h"
#include "log_buffer.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_wifi.h"

static const char *TAG = "web_server";

static httpd_handle_t s_server = NULL;

/* ============================================================
 * MIME TYPES
 * ============================================================ */

static const char *get_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(ext, ".json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(ext, ".png")  == 0) return "image/png";
    if (strcasecmp(ext, ".jpg")  == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".gif")  == 0) return "image/gif";
    if (strcasecmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (strcasecmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcasecmp(ext, ".txt")  == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, ".woff") == 0) return "font/woff";
    if (strcasecmp(ext, ".woff2")== 0) return "font/woff2";

    return "application/octet-stream";
}

/* ============================================================
 * HELPER: cookie and authentication
 * ============================================================ */

static bool get_cookie_value(httpd_req_t *req, const char *name,
                              char *out, size_t out_len)
{
    if (!req || !name || !out || out_len == 0) return false;
    out[0] = '\0';

    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (hdr_len == 0 || hdr_len > 512) return false;

    char *hdr = malloc(hdr_len + 1);
    if (!hdr) return false;

    if (httpd_req_get_hdr_value_str(req, "Cookie", hdr, hdr_len + 1) != ESP_OK) {
        free(hdr);
        return false;
    }

    bool found = false;
    size_t name_len = strlen(name);

    char *p = hdr;
    while (p && *p) {
        while (*p == ' ' || *p == ';') p++;
        if (*p == '\0') break;

        char *eq = strchr(p, '=');
        if (!eq) break;

        if ((size_t)(eq - p) == name_len && strncmp(p, name, name_len) == 0) {
            char *val_start = eq + 1;
            char *val_end = strchr(val_start, ';');
            size_t val_len = val_end ? (size_t)(val_end - val_start)
                                     : strlen(val_start);
            if (val_len >= out_len) val_len = out_len - 1;
            memcpy(out, val_start, val_len);
            out[val_len] = '\0';
            found = true;
            break;
        }

        char *next = strchr(p, ';');
        if (!next) break;
        p = next + 1;
    }

    free(hdr);
    return found;
}

static bool is_authenticated(httpd_req_t *req)
{
    char token[AUTH_TOKEN_HEX_LEN + 1];
    if (!get_cookie_value(req, AUTH_COOKIE_NAME, token, sizeof(token))) {
        return false;
    }
    if (!auth_check_session(token)) {
        return false;
    }
    auth_touch_session(token);
    return true;
}

/* ============================================================
 * HELPER: static files (streaming)
 * ============================================================ */

static esp_err_t serve_static_file(httpd_req_t *req, const char *uri)
{
    char filepath[600];

    if (strstr(uri, "..") != NULL) {
        ESP_LOGW(TAG, "path traversal blocked: %s", uri);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    snprintf(filepath, sizeof(filepath), "%s/www%s", NW_FS_BASE, uri);

    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGW(TAG, "404: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, get_mime_type(filepath));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char chunk[1024];
    size_t rd;
    esp_err_t ret = ESP_OK;

    while ((rd = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        ret = httpd_resp_send_chunk(req, chunk, rd);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "send_chunk failed: %s", esp_err_to_name(ret));
            break;
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);

    return ret;
}

static esp_err_t redirect_to_login(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ============================================================
 * HELPER: form parsing
 * ============================================================ */

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t i = 0, j = 0;
    while (src[i] && j < dst_len - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static bool get_form_field(const char *body, const char *field,
                           char *out, size_t out_len)
{
    if (!body || !field || !out || out_len == 0) return false;
    out[0] = '\0';

    char key[64];
    snprintf(key, sizeof(key), "%s=", field);

    const char *p = strstr(body, key);
    if (!p) return false;
    p += strlen(key);

    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    char tmp[512];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, p, len);
    tmp[len] = '\0';

    url_decode(out, tmp, out_len);
    return true;
}

static void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_len; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c == '\n') {
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            /* skip */
        } else if ((unsigned char)c < 0x20) {
            /* skip */
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

/* ============================================================
 * HANDLER: login/logout
 * ============================================================ */

static esp_err_t handler_login_page(httpd_req_t *req)
{
    return serve_static_file(req, "/login.html");
}

static esp_err_t handler_api_login(httpd_req_t *req)
{
    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char user[64] = {0};
    char pass[128] = {0};
    get_form_field(body, "user", user, sizeof(user));
    get_form_field(body, "pass", pass, sizeof(pass));

    ESP_LOGI(TAG, "login attempt user='%s'", user);

    if (auth_is_locked_out()) {
        ESP_LOGW(TAG, "login rejected: lockout active");
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Locked out", -1);
        return ESP_OK;
    }

    char token[AUTH_TOKEN_HEX_LEN + 1];
    esp_err_t ret = auth_login(user, pass, token, sizeof(token));

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "login failed");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login?error=1");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    char cookie_hdr[320];
    snprintf(cookie_hdr, sizeof(cookie_hdr),
             "%s=%s; HttpOnly; Path=/; Max-Age=%d",
             AUTH_COOKIE_NAME, token, NW_SESSION_TIMEOUT_S);

    httpd_resp_set_hdr(req, "Set-Cookie", cookie_hdr);

    const char *dest;
    if (auth_must_change_password()) {
        dest = "/change-password";
    } else if (!wifi_manager_has_credentials()) {
        dest = "/setup";
    } else {
        dest = "/";
    }

    ESP_LOGI(TAG, "login OK, redirect to %s", dest);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", dest);
    httpd_resp_send(req, NULL, 0);   /* body vuoto → niente da inviare → niente errore */
    return ESP_OK;
}

static esp_err_t handler_api_logout(httpd_req_t *req)
{
    char token[AUTH_TOKEN_HEX_LEN + 1];
    if (get_cookie_value(req, AUTH_COOKIE_NAME, token, sizeof(token))) {
        auth_logout(token);
    }

    char cookie_hdr[128];
    snprintf(cookie_hdr, sizeof(cookie_hdr),
             "%s=; HttpOnly; Path=/; Max-Age=0",
             AUTH_COOKIE_NAME);

    httpd_resp_set_hdr(req, "Set-Cookie", cookie_hdr);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_change_password_page(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return redirect_to_login(req);
    }
    return serve_static_file(req, "/change-password.html");
}

static esp_err_t handler_api_change_password(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char old_pass[128] = {0};
    char new_pass[128] = {0};
    get_form_field(body, "old_pass", old_pass, sizeof(old_pass));
    get_form_field(body, "new_pass", new_pass, sizeof(new_pass));

    esp_err_t ret = auth_change_password(old_pass, new_pass);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "password change failed");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/change-password?error=1");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    const char *dest = wifi_manager_has_credentials() ? "/" : "/setup";
    ESP_LOGI(TAG, "password changed, redirect to %s", dest);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", dest);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ============================================================
 * HANDLER: Wi-Fi setup
 * ============================================================ */

static esp_err_t handler_setup_page(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return redirect_to_login(req);
    }
    return serve_static_file(req, "/setup.html");
}

static esp_err_t handler_api_wifi_scan(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Wi-Fi scan requested");

    nw_wifi_ap_t aps[NW_WIFI_SCAN_MAX_AP];
    size_t found = 0;
    esp_err_t ret = wifi_manager_scan(aps, NW_WIFI_SCAN_MAX_AP, &found);

    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"scan failed\"}", -1);
        return ESP_OK;
    }

    size_t bufsize = 256 + found * 128;
    char *json = malloc(bufsize);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t pos = 0;
    pos += snprintf(json + pos, bufsize - pos, "{\"count\":%u,\"aps\":[", (unsigned)found);

    for (size_t i = 0; i < found; i++) {
        char ssid_esc[128];
        json_escape(ssid_esc, sizeof(ssid_esc), aps[i].ssid);

        pos += snprintf(json + pos, bufsize - pos,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%u,\"ch\":%u}",
            i > 0 ? "," : "",
            ssid_esc,
            aps[i].rssi,
            aps[i].authmode,
            aps[i].channel);
    }

    pos += snprintf(json + pos, bufsize - pos, "]}");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, json, pos);

    free(json);
    return ESP_OK;
}

static esp_err_t handler_api_wifi_test(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[NW_WIFI_SSID_MAX_LEN] = {0};
    char pass[NW_WIFI_PASS_MAX_LEN] = {0};
    get_form_field(body, "ssid", ssid, sizeof(ssid));
    get_form_field(body, "pass", pass, sizeof(pass));

    ESP_LOGI(TAG, "connection test to '%s'", ssid);

    char ip[16] = {0};
    char gw[16] = {0};
    char dns[16] = {0};

    esp_err_t ret = wifi_manager_test_sta(ssid, pass,
                                          ip, sizeof(ip),
                                          gw, sizeof(gw),
                                          dns, sizeof(dns),
                                          15000);

    httpd_resp_set_type(req, "application/json; charset=utf-8");

    if (ret == ESP_OK) {
        char json[256];
        snprintf(json, sizeof(json),
            "{\"ok\":true,\"ip\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\"}",
            ip, gw, dns);
        httpd_resp_send(req, json, -1);
    } else {
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"timeout o credenziali errate\"}", -1);
    }

    return ESP_OK;
}

static esp_err_t handler_api_wifi_save(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[NW_WIFI_SSID_MAX_LEN] = {0};
    char pass[NW_WIFI_PASS_MAX_LEN] = {0};
    get_form_field(body, "ssid", ssid, sizeof(ssid));
    get_form_field(body, "pass", pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"SSID vuoto\"}", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "saving credentials for '%s'", ssid);

    esp_err_t ret = wifi_manager_save_credentials(ssid, pass);

    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        httpd_resp_send(req, "{\"ok\":true,\"restart\":true}", -1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"save failed\"}", -1);
    }

    return ESP_OK;
}

/* ============================================================
 * HANDLER: ARP scan
 * ============================================================ */

static esp_err_t handler_scan_page(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return redirect_to_login(req);
    }
    return serve_static_file(req, "/scan.html");
}

static esp_err_t handler_api_scan_now(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "manual scan requested");

    esp_err_t ret = arp_scanner_start_scan();

    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"scansione gia' in corso\"}", -1);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"avvio fallito\"}", -1);
    }

    return ESP_OK;
}

static esp_err_t handler_api_scan_status(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    bool scanning = arp_scanner_is_scanning();

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char json[128];
    snprintf(json, sizeof(json), "{\"scanning\":%s}", scanning ? "true" : "false");
    httpd_resp_send(req, json, -1);
    return ESP_OK;
}

static esp_err_t handler_api_scan_last(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    nw_arp_scan_batch_t res;
    esp_err_t ret = arp_scanner_get_last_result(&res);

    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"no data\"}", -1);
        return ESP_OK;
    }

    size_t bufsize = 256 + ARP_SCAN_MAX_RESULTS * 96;
    char *json = malloc(bufsize);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t pos = 0;
    pos += snprintf(json + pos, bufsize - pos,
                    "{\"valid\":%s,\"count\":%u,\"duration_ms\":%u,\"scanned_ips\":%u,\"devices\":[",
                    res.valid ? "true" : "false",
                    (unsigned)res.count,
                    (unsigned)res.duration_ms,
                    (unsigned)res.scanned_ips);

    for (size_t i = 0; i < res.count; i++) {
        nw_scan_result_t *e = &res.entries[i];
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                 (unsigned)((e->ip >> 24) & 0xFF),
                 (unsigned)((e->ip >> 16) & 0xFF),
                 (unsigned)((e->ip >> 8) & 0xFF),
                 (unsigned)(e->ip & 0xFF));

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 e->mac.bytes[0], e->mac.bytes[1], e->mac.bytes[2],
                 e->mac.bytes[3], e->mac.bytes[4], e->mac.bytes[5]);

        pos += snprintf(json + pos, bufsize - pos,
                        "%s{\"ip\":\"%s\",\"mac\":\"%s\"}",
                        i > 0 ? "," : "", ip_str, mac_str);
    }

    pos += snprintf(json + pos, bufsize - pos, "]}");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, json, pos);

    free(json);
    return ESP_OK;
}

/* ============================================================
 * HANDLER: MAC lists
 * ============================================================ */

static esp_err_t handler_lists_page(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return redirect_to_login(req);
    }
    return serve_static_file(req, "/lists.html");
}

static size_t serialize_list_json(nw_list_type_t type, char *buf, size_t bufsize)
{
    size_t pos = 0;
    const char *name = mac_list_type_name(type);
    size_t count = mac_list_count(type);

    pos += snprintf(buf + pos, bufsize - pos,
                    "\"%s\":{\"count\":%u,\"entries\":[",
                    name, (unsigned)count);

    for (size_t i = 0; i < count; i++) {
        const nw_list_entry_t *e = mac_list_get(type, i);
        if (!e) continue;

        char mac_str[18];
        mac_to_string(e->mac.bytes, mac_str);

        char desc_esc[NW_LIST_DESC_MAX_LEN * 2 + 2];
        json_escape(desc_esc, sizeof(desc_esc), e->desc);

        const char *vendor = oui_lookup_vendor(e->mac.bytes);
        char vendor_esc[64];
        json_escape(vendor_esc, sizeof(vendor_esc), vendor ? vendor : "");

        char ip_str[16] = "";
        if (type == NW_LIST_UNKNOWN) {
            mac_list_get_ip(e->mac.bytes, ip_str, sizeof(ip_str));
        }

        pos += snprintf(buf + pos, bufsize - pos,
                        "%s{\"mac\":\"%s\",\"desc\":\"%s\","
                        "\"vendor\":\"%s\",\"ip\":\"%s\","
                        "\"first_seen\":%u,\"last_seen\":%u,"
                        "\"first_seen_is_unix\":%s,\"last_seen_is_unix\":%s}",
                        i > 0 ? "," : "",
                        mac_str, desc_esc, vendor_esc, ip_str,
                        (unsigned)e->first_seen,
                        (unsigned)e->last_seen,
                        e->first_seen_is_unix ? "true" : "false",
                        e->last_seen_is_unix ? "true" : "false");
    }

    pos += snprintf(buf + pos, bufsize - pos, "]}");
    return pos;
}

static esp_err_t handler_api_lists(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    size_t total_entries = mac_list_count(NW_LIST_WHITELIST)
                         + mac_list_count(NW_LIST_BLACKLIST)
                         + mac_list_count(NW_LIST_UNKNOWN);

    size_t bufsize = 512 + total_entries * 260;
    if (bufsize < 1024) bufsize = 1024;
    if (bufsize > 24000) bufsize = 24000;

    char *json = malloc(bufsize);
    if (!json) {
        ESP_LOGE(TAG, "malloc %u failed for /api/lists", (unsigned)bufsize);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t pos = 0;
    pos += snprintf(json + pos, bufsize - pos,
                    "{\"time_synced\":%s,",
                    mac_list_is_time_synced() ? "true" : "false");
    pos += serialize_list_json(NW_LIST_WHITELIST, json + pos, bufsize - pos);
    pos += snprintf(json + pos, bufsize - pos, ",");
    pos += serialize_list_json(NW_LIST_BLACKLIST, json + pos, bufsize - pos);
    pos += snprintf(json + pos, bufsize - pos, ",");
    pos += serialize_list_json(NW_LIST_UNKNOWN, json + pos, bufsize - pos);
    pos += snprintf(json + pos, bufsize - pos, "}");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, json, pos);

    free(json);
    return ESP_OK;
}

static esp_err_t handler_api_lists_add(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char type_str[32] = {0};
    char mac_str[32]  = {0};
    char desc[NW_LIST_DESC_MAX_LEN * 2 + 2] = {0};

    get_form_field(body, "type", type_str, sizeof(type_str));
    get_form_field(body, "mac", mac_str, sizeof(mac_str));
    get_form_field(body, "desc", desc, sizeof(desc));

    int type = mac_list_type_from_name(type_str);
    if (type < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid type\"}", -1);
        return ESP_OK;
    }

    uint8_t mac[6];
    if (!string_to_mac(mac_str, mac)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid MAC\"}", -1);
        return ESP_OK;
    }

    esp_err_t ret = mac_list_add((nw_list_type_t)type, mac, desc);
    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        mac_list_save((nw_list_type_t)type);
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else if (ret == ESP_ERR_NO_MEM) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"lista piena\"}", -1);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"aggiunta fallita\"}", -1);
    }
    return ESP_OK;
}

static esp_err_t handler_api_lists_remove(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char type_str[32] = {0};
    char mac_str[32]  = {0};
    get_form_field(body, "type", type_str, sizeof(type_str));
    get_form_field(body, "mac", mac_str, sizeof(mac_str));

    int type = mac_list_type_from_name(type_str);
    if (type < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid type\"}", -1);
        return ESP_OK;
    }

    uint8_t mac[6];
    if (!string_to_mac(mac_str, mac)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid MAC\"}", -1);
        return ESP_OK;
    }

    esp_err_t ret = mac_list_remove((nw_list_type_t)type, mac);
    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        mac_list_save((nw_list_type_t)type);
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"non trovato\"}", -1);
    }
    return ESP_OK;
}

static esp_err_t handler_api_lists_clear(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[128];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char type_str[32] = {0};
    get_form_field(body, "type", type_str, sizeof(type_str));

    int type = mac_list_type_from_name(type_str);
    if (type < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid type\"}", -1);
        return ESP_OK;
    }

    esp_err_t ret = mac_list_clear((nw_list_type_t)type);
    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        mac_list_save((nw_list_type_t)type);
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"clear fallito\"}", -1);
    }
    return ESP_OK;
}

static esp_err_t handler_api_lists_move(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char from_str[32] = {0};
    char to_str[32]   = {0};
    char mac_str[32]  = {0};
    get_form_field(body, "from", from_str, sizeof(from_str));
    get_form_field(body, "to", to_str, sizeof(to_str));
    get_form_field(body, "mac", mac_str, sizeof(mac_str));

    int from = mac_list_type_from_name(from_str);
    int to   = mac_list_type_from_name(to_str);
    if (from < 0 || to < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid type\"}", -1);
        return ESP_OK;
    }

    uint8_t mac[6];
    if (!string_to_mac(mac_str, mac)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid MAC\"}", -1);
        return ESP_OK;
    }

    esp_err_t ret = mac_list_move((nw_list_type_t)from, (nw_list_type_t)to, mac);
    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        mac_list_save((nw_list_type_t)from);
        mac_list_save((nw_list_type_t)to);
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else if (ret == ESP_ERR_NO_MEM) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"lista destinazione piena\"}", -1);
    } else if (ret == ESP_ERR_NOT_FOUND) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"MAC non trovato\"}", -1);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"spostamento fallito\"}", -1);
    }
    return ESP_OK;
}

static esp_err_t handler_api_lists_move_all(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[128];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char from_str[32] = {0};
    char to_str[32]   = {0};
    get_form_field(body, "from", from_str, sizeof(from_str));
    get_form_field(body, "to", to_str, sizeof(to_str));

    int from = mac_list_type_from_name(from_str);
    int to   = mac_list_type_from_name(to_str);

    if (from < 0 || to < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid type\"}", -1);
        return ESP_OK;
    }

    if (from == to) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"lista sorgente e destinazione coincidono\"}", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "bulk move: %s -> %s", from_str, to_str);

    int moved = 0;
    int failed = 0;

    while (mac_list_count((nw_list_type_t)from) > 0) {
        const nw_list_entry_t *e = mac_list_get((nw_list_type_t)from, 0);
        if (!e) break;

        uint8_t mac[6];
        memcpy(mac, e->mac.bytes, 6);

        esp_err_t r = mac_list_move((nw_list_type_t)from,
                                    (nw_list_type_t)to, mac);
        if (r == ESP_OK) {
            moved++;
        } else {
            failed++;
            break;
        }
    }

    mac_list_save((nw_list_type_t)from);
    mac_list_save((nw_list_type_t)to);

    ESP_LOGI(TAG, "bulk move completed: %d moved, %d failed", moved, failed);

    httpd_resp_set_type(req, "application/json");
    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"moved\":%d,\"failed\":%d}",
             moved, failed);
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

static esp_err_t handler_api_lists_desc(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char type_str[32] = {0};
    char mac_str[32]  = {0};
    char desc[NW_LIST_DESC_MAX_LEN * 2 + 2] = {0};
    get_form_field(body, "type", type_str, sizeof(type_str));
    get_form_field(body, "mac", mac_str, sizeof(mac_str));
    get_form_field(body, "desc", desc, sizeof(desc));

    int type = mac_list_type_from_name(type_str);
    if (type < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid type\"}", -1);
        return ESP_OK;
    }

    uint8_t mac[6];
    if (!string_to_mac(mac_str, mac)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid MAC\"}", -1);
        return ESP_OK;
    }

    esp_err_t ret = mac_list_update_desc((nw_list_type_t)type, mac, desc);
    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        mac_list_save((nw_list_type_t)type);
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else if (ret == ESP_ERR_INVALID_ARG) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"descrizione non valida\"}", -1);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"MAC non trovato\"}", -1);
    }
    return ESP_OK;
}

/* ============================================================
 * HANDLER: settings
 * ============================================================ */

static esp_err_t handler_settings_page(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return redirect_to_login(req);
    }
    return serve_static_file(req, "/settings.html");
}

static esp_err_t handler_api_settings_get(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    bool tg_configured = settings_telegram_is_configured();
    bool wh_configured = settings_webhook_is_configured();

    char json[256];
    snprintf(json, sizeof(json),
        "{\"scan_interval\":%d,"
        "\"telegram_configured\":%s,"
        "\"webhook_configured\":%s}",
        (int)settings_get_scan_interval(),
        tg_configured ? "true" : "false",
        wh_configured ? "true" : "false");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, json, -1);
    return ESP_OK;
}

static esp_err_t handler_api_settings_scan_interval(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[128];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char val_str[16] = {0};
    get_form_field(body, "interval", val_str, sizeof(val_str));

    int interval = atoi(val_str);
    if (interval < NW_SCAN_INTERVAL_MIN_MIN || interval > NW_SCAN_INTERVAL_MAX_MIN) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char json[128];
        snprintf(json, sizeof(json),
            "{\"ok\":false,\"error\":\"intervallo deve essere tra %d e %d\"}",
            NW_SCAN_INTERVAL_MIN_MIN, NW_SCAN_INTERVAL_MAX_MIN);
        httpd_resp_send(req, json, -1);
        return ESP_OK;
    }

    esp_err_t ret = settings_set_scan_interval(interval);
    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        char json[128];
        snprintf(json, sizeof(json),
            "{\"ok\":true,\"interval\":%d}",
            (int)settings_get_scan_interval());
        httpd_resp_send(req, json, -1);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"salvataggio fallito\"}", -1);
    }
    return ESP_OK;
}

static esp_err_t handler_api_settings_password(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char old_pass[128] = {0};
    char new_pass[128] = {0};
    char confirm[128] = {0};
    get_form_field(body, "old_pass", old_pass, sizeof(old_pass));
    get_form_field(body, "new_pass", new_pass, sizeof(new_pass));
    get_form_field(body, "confirm", confirm, sizeof(confirm));

    if (strcmp(new_pass, confirm) != 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"le password non coincidono\"}", -1);
        return ESP_OK;
    }

    esp_err_t ret = auth_change_password(old_pass, new_pass);
    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else if (ret == ESP_ERR_INVALID_ARG) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"password non valida (min 8 caratteri)\"}", -1);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"password attuale errata\"}", -1);
    }
    return ESP_OK;
}

static esp_err_t handler_api_settings_telegram(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char token[NW_TG_TOKEN_MAX_LEN * 2 + 2] = {0};
    char chatid[NW_TG_CHATID_MAX_LEN * 2 + 2] = {0};
    get_form_field(body, "token", token, sizeof(token));
    get_form_field(body, "chatid", chatid, sizeof(chatid));

    esp_err_t ret1 = settings_set_telegram_token(token);
    esp_err_t ret2 = settings_set_telegram_chatid(chatid);

    httpd_resp_set_type(req, "application/json");

    if (ret1 == ESP_OK && ret2 == ESP_OK) {
        telegram_reload_config();
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"salvataggio fallito\"}", -1);
    }
    return ESP_OK;
}

static esp_err_t handler_api_settings_telegram_test(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char token[NW_TG_TOKEN_MAX_LEN * 2 + 2] = {0};
    char chatid[NW_TG_CHATID_MAX_LEN * 2 + 2] = {0};
    get_form_field(body, "token", token, sizeof(token));
    get_form_field(body, "chatid", chatid, sizeof(chatid));

    if (token[0] == '\0' || chatid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"token o chatid mancanti\"}", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Telegram test with temporary credentials");

    esp_err_t ret = telegram_send_test_with(token, chatid);

    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Telegram test sent successfully");
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else if (ret == ESP_ERR_NO_MEM) {
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"memoria insufficiente\"}", -1);
    } else {
        ESP_LOGW(TAG, "Telegram test failed");
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"invio fallito (token/chatid errati o rete)\"}", -1);
    }
    return ESP_OK;
}

static esp_err_t handler_api_settings_telegram_clear(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Telegram credentials deletion requested");

    settings_set_telegram_token("");
    settings_set_telegram_chatid("");
    telegram_reload_config();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    return ESP_OK;
}

static esp_err_t handler_api_settings_webhook(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char url[NW_WH_URL_MAX_LEN * 2 + 2] = {0};
    char token[NW_WH_TOKEN_MAX_LEN * 2 + 2] = {0};
    get_form_field(body, "url", url, sizeof(url));
    get_form_field(body, "token", token, sizeof(token));

    esp_err_t ret1 = settings_set_webhook_url(url);
    esp_err_t ret2 = settings_set_webhook_token(token);

    httpd_resp_set_type(req, "application/json");

    if (ret1 == ESP_OK && ret2 == ESP_OK) {
        webhook_reload_config();
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else if (ret1 == ESP_ERR_INVALID_ARG) {
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"URL non valido (http:// o https://)\"}", -1);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"salvataggio fallito\"}", -1);
    }
    return ESP_OK;
}

static esp_err_t handler_api_settings_webhook_test(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char url[NW_WH_URL_MAX_LEN * 2 + 2] = {0};
    char token[NW_WH_TOKEN_MAX_LEN * 2 + 2] = {0};
    get_form_field(body, "url", url, sizeof(url));
    get_form_field(body, "token", token, sizeof(token));

    if (url[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"URL mancante\"}", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "webhook test with temporary URL");

    esp_err_t ret = webhook_send_test_with(url, token);

    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "webhook test sent successfully");
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else if (ret == ESP_ERR_NO_MEM) {
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"memoria insufficiente\"}", -1);
    } else {
        ESP_LOGW(TAG, "webhook test failed");
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"invio fallito (URL non raggiungibile)\"}", -1);
    }
    return ESP_OK;
}

static esp_err_t handler_api_settings_webhook_clear(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "webhook credentials deletion requested");

    settings_set_webhook_url("");
    settings_set_webhook_token("");
    webhook_reload_config();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    return ESP_OK;
}

static esp_err_t handler_api_settings_factory_reset(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    char body[128];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char confirm[16] = {0};
    get_form_field(body, "confirm", confirm, sizeof(confirm));

    if (strcmp(confirm, "RESET") != 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"digitare RESET per confermare\"}", -1);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "FACTORY RESET requested from web UI");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"restart\":true}", -1);

    vTaskDelay(pdMS_TO_TICKS(2000));

    led_set_pattern(LED_PATTERN_ON);
    vTaskDelay(pdMS_TO_TICKS(1500));

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_restore();
    vTaskDelay(pdMS_TO_TICKS(1000));

    storage_erase_all();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/* ============================================================
 * HANDLER: backup / restore
 * ============================================================ */

static esp_err_t handler_api_backup_export(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "backup export requested");

    size_t total_entries = mac_list_count(NW_LIST_WHITELIST)
                         + mac_list_count(NW_LIST_BLACKLIST)
                         + mac_list_count(NW_LIST_UNKNOWN);

    size_t bufsize = 512 + total_entries * 260;
    if (bufsize < 1024) bufsize = 1024;
    if (bufsize > 40000) bufsize = 40000;

    char *json = malloc(bufsize);
    if (!json) {
        ESP_LOGE(TAG, "malloc %u failed for backup export", (unsigned)bufsize);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t pos = 0;
    pos += snprintf(json + pos, bufsize - pos,
                    "{\n  \"version\": 1,\n  \"exported_at\": %lld,\n  \"scan_interval\": %d,\n",
                    (long long)time(NULL),
                    (int)settings_get_scan_interval());

    const nw_list_type_t types[3] = {
        NW_LIST_WHITELIST, NW_LIST_BLACKLIST, NW_LIST_UNKNOWN
    };
    const char *keys[3] = { "whitelist", "blacklist", "unknown" };

    for (int t = 0; t < 3; t++) {
        size_t count = mac_list_count(types[t]);
        pos += snprintf(json + pos, bufsize - pos, "  \"%s\": [", keys[t]);

        for (size_t i = 0; i < count; i++) {
            const nw_list_entry_t *e = mac_list_get(types[t], i);
            if (!e) continue;

            char mac_str[18];
            mac_to_string(e->mac.bytes, mac_str);

            char desc_esc[NW_LIST_DESC_MAX_LEN * 2 + 2];
            json_escape(desc_esc, sizeof(desc_esc), e->desc);

            pos += snprintf(json + pos, bufsize - pos,
                            "%s\n    {\"mac\":\"%s\",\"desc\":\"%s\","
                            "\"first_seen\":%u,\"last_seen\":%u,"
                            "\"first_seen_is_unix\":%s,\"last_seen_is_unix\":%s}",
                            i > 0 ? "," : "",
                            mac_str, desc_esc,
                            (unsigned)e->first_seen,
                            (unsigned)e->last_seen,
                            e->first_seen_is_unix ? "true" : "false",
                            e->last_seen_is_unix ? "true" : "false");

            if (pos >= bufsize - 100) {
                ESP_LOGW(TAG, "backup export truncated at %u bytes", (unsigned)pos);
                break;
            }
        }

        pos += snprintf(json + pos, bufsize - pos, "%s\n  ]%s",
                        count > 0 ? "\n  " : "",
                        t < 2 ? "," : "");
    }

    pos += snprintf(json + pos, bufsize - pos, "\n}\n");

    char cd_hdr[128];
    time_t now = time(NULL);
    struct tm tm_info;
    gmtime_r(&now, &tm_info);
    char filename[64];
    strftime(filename, sizeof(filename), "networkwatcher-backup-%Y-%m-%d.json", &tm_info);
    snprintf(cd_hdr, sizeof(cd_hdr), "attachment; filename=\"%s\"", filename);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", cd_hdr);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, pos);

    ESP_LOGI(TAG, "backup exported: %u bytes, %u entries total",
             (unsigned)pos, (unsigned)total_entries);

    free(json);
    return ESP_OK;
}

static esp_err_t handler_api_backup_import(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    if (arp_scanner_is_scanning()) {
        ESP_LOGW(TAG, "import rejected: ARP scan in progress");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"scansione ARP in corso, attendere il termine\"}", -1);
        return ESP_OK;
    }

    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > NW_HTTP_MAX_BODY * 20) {
        ESP_LOGW(TAG, "import: invalid content_len (%u)", (unsigned)content_len);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"file vuoto o troppo grande\"}", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "backup import requested (%u bytes)", (unsigned)content_len);

    char *body = malloc(content_len + 1);
    if (!body) {
        ESP_LOGE(TAG, "malloc %u failed for import", (unsigned)content_len);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"memoria insufficiente\"}", -1);
        return ESP_OK;
    }

    int received_total = 0;
    while (received_total < (int)content_len) {
        int r = httpd_req_recv(req, body + received_total,
                               content_len - received_total);
        if (r <= 0) {
            ESP_LOGE(TAG, "httpd_req_recv error (%d)", r);
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        received_total += r;
    }
    body[received_total] = '\0';

    const char *keys[3] = { "whitelist", "blacklist", "unknown" };
    nw_list_type_t types[3] = {
        NW_LIST_WHITELIST, NW_LIST_BLACKLIST, NW_LIST_UNKNOWN
    };

    bool keys_present[3] = { false, false, false };
    for (int t = 0; t < 3; t++) {
        char key_pattern[32];
        snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", keys[t]);
        if (strstr(body, key_pattern) != NULL) {
            keys_present[t] = true;
        }
    }

    if (!keys_present[0] || !keys_present[1] || !keys_present[2]) {
        ESP_LOGW(TAG, "import rejected: missing keys");
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"file incompleto: mancano whitelist, blacklist o unknown\"}", -1);
        return ESP_OK;
    }

    int counts[3] = {0, 0, 0};

    for (int t = 0; t < 3; t++) {
        char key_pattern[32];
        snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", keys[t]);
        const char *kp = strstr(body, key_pattern);
        if (!kp) continue;
        const char *p = kp + strlen(key_pattern);
        while (*p && *p != '[') p++;
        if (*p != '[') continue;
        p++;

        int depth = 0;
        bool in_string = false;
        bool escape = false;

        for (const char *q = p; *q; q++) {
            char c = *q;
            if (escape) { escape = false; continue; }
            if (c == '\\' && in_string) { escape = true; continue; }
            if (c == '"') { in_string = !in_string; continue; }
            if (in_string) continue;
            if (c == '[') depth++;
            else if (c == ']') { if (depth == 0) break; depth--; }
            else if (c == '{' && depth == 0) counts[t]++;
        }
    }

    ESP_LOGI(TAG, "import: whitelist=%d blacklist=%d unknown=%d",
             counts[0], counts[1], counts[2]);

    if (counts[0] > NW_LIST_MAX_ENTRIES ||
        counts[1] > NW_LIST_MAX_ENTRIES ||
        counts[2] > NW_LIST_MAX_ENTRIES) {
        ESP_LOGW(TAG, "import rejected: too many entries per list");
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char err[160];
        snprintf(err, sizeof(err),
            "{\"ok\":false,\"error\":\"troppi MAC per lista (max %d)\"}",
            NW_LIST_MAX_ENTRIES);
        httpd_resp_send(req, err, -1);
        return ESP_OK;
    }

    for (int t = 0; t < 3; t++) {
        mac_list_clear(types[t]);
    }

    int imported[3] = {0, 0, 0};

    for (int t = 0; t < 3; t++) {
        char key_pattern[32];
        snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", keys[t]);
        const char *kp = strstr(body, key_pattern);
        if (!kp) continue;
        const char *p = kp + strlen(key_pattern);
        while (*p && *p != '[') p++;
        if (*p != '[') continue;
        p++;

        int depth = 0;
        bool in_string = false;
        bool escape = false;
        const char *obj_start = NULL;

        for (const char *q = p; *q; q++) {
            char c = *q;
            if (escape) { escape = false; continue; }
            if (c == '\\' && in_string) { escape = true; continue; }
            if (c == '"') { in_string = !in_string; continue; }
            if (in_string) continue;
            if (c == '[') depth++;
            else if (c == ']') { if (depth == 0) break; depth--; }
            else if (c == '{' && depth == 0 && !obj_start) obj_start = q;
            else if (c == '}' && depth == 0 && obj_start) {
                size_t block_len = (size_t)(q - obj_start) + 1;
                if (block_len > 511) block_len = 511;

                char block[512];
                memcpy(block, obj_start, block_len);
                block[block_len] = '\0';

                char mac_str[32] = {0};
                const char *mp = strstr(block, "\"mac\"");
                if (mp) {
                    mp = strchr(mp, ':');
                    if (mp) {
                        mp = strchr(mp, '"');
                        if (mp) {
                            mp++;
                            const char *me = strchr(mp, '"');
                            if (me && (size_t)(me - mp) < sizeof(mac_str)) {
                                memcpy(mac_str, mp, me - mp);
                                mac_str[me - mp] = '\0';
                            }
                        }
                    }
                }

                char desc[NW_LIST_DESC_MAX_LEN * 2 + 2] = {0};
                const char *dp = strstr(block, "\"desc\"");
                if (dp) {
                    dp = strchr(dp, ':');
                    if (dp) {
                        dp = strchr(dp, '"');
                        if (dp) {
                            dp++;
                            const char *de = strchr(dp, '"');
                            if (de && (size_t)(de - dp) < sizeof(desc)) {
                                memcpy(desc, dp, de - dp);
                                desc[de - dp] = '\0';
                            }
                        }
                    }
                }

                uint8_t mac[6];
                if (string_to_mac(mac_str, mac)) {
                    if (mac_list_add(types[t], mac, desc) == ESP_OK) {
                        imported[t]++;
                    }
                }

                obj_start = NULL;
            }
        }
    }

    free(body);

    for (int t = 0; t < 3; t++) {
        mac_list_save(types[t]);
    }

    ESP_LOGI(TAG, "import completed: whitelist=%d blacklist=%d unknown=%d",
             imported[0], imported[1], imported[2]);

    httpd_resp_set_type(req, "application/json");
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"imported\":{\"whitelist\":%d,\"blacklist\":%d,\"unknown\":%d}}",
             imported[0], imported[1], imported[2]);
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* ============================================================
 * HANDLER: log (Fase 3.5)
 * ============================================================ */

static esp_err_t handler_logs_page(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return redirect_to_login(req);
    }
    return serve_static_file(req, "/logs.html");
}

static esp_err_t handler_api_logs(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    size_t cap = 2304;
    char *raw = malloc(cap);
    if (!raw) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t n = log_buffer_read(raw, cap);

    size_t bufsize = n * 2 + 256;
    if (bufsize < 1024) bufsize = 1024;
    if (bufsize > 8192) bufsize = 8192;

    char *json = malloc(bufsize);
    if (!json) {
        free(raw);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t pos = 0;
    pos += snprintf(json + pos, bufsize - pos, "{\"count\":0,\"lines\":[");

    int count = 0;
    char line_buf[256];
    size_t line_len = 0;

    for (size_t i = 0; i <= n; i++) {
        char c = (i < n) ? raw[i] : '\n';

        if (c == '\n' || c == '\r' || line_len >= sizeof(line_buf) - 1) {
            if (line_len == 0) continue;

            line_buf[line_len] = '\0';

            char esc[512];
            json_escape(esc, sizeof(esc), line_buf);

            if (pos < bufsize - 100) {
                pos += snprintf(json + pos, bufsize - pos,
                                "%s\"%s\"", count > 0 ? "," : "", esc);
                count++;
            }

            line_len = 0;
            if (c == '\r') continue;
        } else {
            line_buf[line_len++] = c;
        }
    }

    pos += snprintf(json + pos, bufsize - pos, "]}");

    char *count_ptr = strstr(json, "\"count\":0");
    if (count_ptr) {
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "\"count\":%d", count);
        size_t old_len = strlen("\"count\":0");
        size_t new_len = strlen(count_str);
        if (new_len == old_len) {
            memcpy(count_ptr, count_str, old_len);
        }
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, json, pos);

    free(raw);
    free(json);
    return ESP_OK;
}

static esp_err_t handler_api_logs_clear(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Not authenticated", -1);
        return ESP_OK;
    }

    log_buffer_clear();
    ESP_LOGI(TAG, "log buffer cleared from UI");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    return ESP_OK;
}

/* ============================================================
 * HANDLER: index and static
 * ============================================================ */

static esp_err_t handler_index(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return redirect_to_login(req);
    }

    if (auth_must_change_password()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/change-password");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (!wifi_manager_has_credentials()) {
        ESP_LOGI(TAG, "Wi-Fi not configured, redirect to /setup");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    return serve_static_file(req, "/index.html");
}

static esp_err_t handler_static_protected(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return redirect_to_login(req);
    }
    return serve_static_file(req, req->uri);
}

static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (!is_authenticated(req)) {
        return redirect_to_login(req);
    }

    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<!DOCTYPE html><html><body>"
        "<h1>404 - Page not found</h1>"
        "<p><a href=\"/\">Back to home</a></p>"
        "</body></html>", -1);
    return ESP_OK;
}

/* ============================================================
 * INIT / STOP
 * ============================================================ */

esp_err_t web_server_init(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "web server already started");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = NW_HTTP_PORT;
    config.max_uri_handlers = 64;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    ESP_LOGI(TAG, "starting web server on port %d", config.server_port);

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        s_server = NULL;
        return ret;
    }

    /* LOGIN/LOGOUT */
    httpd_uri_t uri_login_get = { .uri = "/login", .method = HTTP_GET,
        .handler = handler_login_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_login_get);

    httpd_uri_t uri_login_post = { .uri = "/api/login", .method = HTTP_POST,
        .handler = handler_api_login, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_login_post);

    httpd_uri_t uri_logout = { .uri = "/api/logout", .method = HTTP_GET,
        .handler = handler_api_logout, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_logout);

    /* HOME */
    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET,
        .handler = handler_index, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_root);

    /* Explicit handlers for .html files that don't match the wildcard */
    httpd_uri_t uri_index_html = { .uri = "/index.html", .method = HTTP_GET,
        .handler = handler_index, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_index_html);

    httpd_uri_t uri_login_html = { .uri = "/login.html", .method = HTTP_GET,
        .handler = handler_login_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_login_html);

    httpd_uri_t uri_change_pwd_html = { .uri = "/change-password.html", .method = HTTP_GET,
        .handler = handler_change_password_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_change_pwd_html);

    httpd_uri_t uri_setup_html = { .uri = "/setup.html", .method = HTTP_GET,
        .handler = handler_setup_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_setup_html);

    httpd_uri_t uri_scan_html = { .uri = "/scan.html", .method = HTTP_GET,
        .handler = handler_scan_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_scan_html);

    httpd_uri_t uri_lists_html = { .uri = "/lists.html", .method = HTTP_GET,
        .handler = handler_lists_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_lists_html);

    httpd_uri_t uri_settings_html = { .uri = "/settings.html", .method = HTTP_GET,
        .handler = handler_settings_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_html);

    httpd_uri_t uri_logs_html = { .uri = "/logs.html", .method = HTTP_GET,
        .handler = handler_logs_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_logs_html);

    httpd_uri_t uri_favicon = { .uri = "/favicon.ico", .method = HTTP_GET,
        .handler = handler_static_protected, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_favicon);

    /* CHANGE PASSWORD */
    httpd_uri_t uri_chpwd_get = { .uri = "/change-password", .method = HTTP_GET,
        .handler = handler_change_password_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_chpwd_get);

    httpd_uri_t uri_chpwd_post = { .uri = "/api/change-password", .method = HTTP_POST,
        .handler = handler_api_change_password, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_chpwd_post);

    /* WIFI SETUP */
    httpd_uri_t uri_setup_get = { .uri = "/setup", .method = HTTP_GET,
        .handler = handler_setup_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_setup_get);

    httpd_uri_t uri_wifi_scan = { .uri = "/api/wifi/scan", .method = HTTP_POST,
        .handler = handler_api_wifi_scan, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_wifi_scan);

    httpd_uri_t uri_wifi_test = { .uri = "/api/wifi/test", .method = HTTP_POST,
        .handler = handler_api_wifi_test, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_wifi_test);

    httpd_uri_t uri_wifi_save = { .uri = "/api/wifi/save", .method = HTTP_POST,
        .handler = handler_api_wifi_save, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_wifi_save);

    /* ARP SCAN */
    httpd_uri_t uri_scan_get = { .uri = "/scan", .method = HTTP_GET,
        .handler = handler_scan_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_scan_get);

    httpd_uri_t uri_scan_now = { .uri = "/api/scan/now", .method = HTTP_POST,
        .handler = handler_api_scan_now, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_scan_now);

    httpd_uri_t uri_scan_status = { .uri = "/api/scan/status", .method = HTTP_GET,
        .handler = handler_api_scan_status, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_scan_status);

    httpd_uri_t uri_scan_last = { .uri = "/api/scan/last", .method = HTTP_GET,
        .handler = handler_api_scan_last, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_scan_last);

    /* MAC LISTS */
    httpd_uri_t uri_lists_page = { .uri = "/lists", .method = HTTP_GET,
        .handler = handler_lists_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_lists_page);

    httpd_uri_t uri_lists_all = { .uri = "/api/lists", .method = HTTP_GET,
        .handler = handler_api_lists, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_lists_all);

    httpd_uri_t uri_lists_add = { .uri = "/api/lists/add", .method = HTTP_POST,
        .handler = handler_api_lists_add, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_lists_add);

    httpd_uri_t uri_lists_remove = { .uri = "/api/lists/remove", .method = HTTP_POST,
        .handler = handler_api_lists_remove, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_lists_remove);

    httpd_uri_t uri_lists_clear = { .uri = "/api/lists/clear", .method = HTTP_POST,
        .handler = handler_api_lists_clear, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_lists_clear);

    httpd_uri_t uri_lists_move = { .uri = "/api/lists/move", .method = HTTP_POST,
        .handler = handler_api_lists_move, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_lists_move);

    httpd_uri_t uri_lists_move_all = { .uri = "/api/lists/move-all", .method = HTTP_POST,
        .handler = handler_api_lists_move_all, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_lists_move_all);

    httpd_uri_t uri_lists_desc = { .uri = "/api/lists/desc", .method = HTTP_POST,
        .handler = handler_api_lists_desc, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_lists_desc);

    /* SETTINGS */
    httpd_uri_t uri_settings_page = { .uri = "/settings", .method = HTTP_GET,
        .handler = handler_settings_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_page);

    httpd_uri_t uri_settings_get = { .uri = "/api/settings/get", .method = HTTP_GET,
        .handler = handler_api_settings_get, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_get);

    httpd_uri_t uri_settings_interval = { .uri = "/api/settings/scan-interval", .method = HTTP_POST,
        .handler = handler_api_settings_scan_interval, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_interval);

    httpd_uri_t uri_settings_password = { .uri = "/api/settings/password", .method = HTTP_POST,
        .handler = handler_api_settings_password, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_password);

    httpd_uri_t uri_settings_telegram = { .uri = "/api/settings/telegram", .method = HTTP_POST,
        .handler = handler_api_settings_telegram, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_telegram);

    httpd_uri_t uri_settings_tg_test = { .uri = "/api/settings/telegram/test", .method = HTTP_POST,
        .handler = handler_api_settings_telegram_test, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_tg_test);

    httpd_uri_t uri_settings_tg_clear = { .uri = "/api/settings/telegram/clear", .method = HTTP_POST,
        .handler = handler_api_settings_telegram_clear, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_tg_clear);

    httpd_uri_t uri_settings_webhook = { .uri = "/api/settings/webhook", .method = HTTP_POST,
        .handler = handler_api_settings_webhook, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_webhook);

    httpd_uri_t uri_settings_wh_test = { .uri = "/api/settings/webhook/test", .method = HTTP_POST,
        .handler = handler_api_settings_webhook_test, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_wh_test);

    httpd_uri_t uri_settings_wh_clear = { .uri = "/api/settings/webhook/clear", .method = HTTP_POST,
        .handler = handler_api_settings_webhook_clear, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_wh_clear);

    httpd_uri_t uri_settings_factory = { .uri = "/api/settings/factory-reset", .method = HTTP_POST,
        .handler = handler_api_settings_factory_reset, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_settings_factory);

    /* BACKUP/RESTORE */
    httpd_uri_t uri_backup_export = { .uri = "/api/backup/export", .method = HTTP_GET,
        .handler = handler_api_backup_export, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_backup_export);

    httpd_uri_t uri_backup_import = { .uri = "/api/backup/import", .method = HTTP_POST,
        .handler = handler_api_backup_import, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_backup_import);

    /* LOG */
    httpd_uri_t uri_logs_page = { .uri = "/logs", .method = HTTP_GET,
        .handler = handler_logs_page, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_logs_page);

    httpd_uri_t uri_api_logs = { .uri = "/api/logs", .method = HTTP_GET,
        .handler = handler_api_logs, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_api_logs);

    httpd_uri_t uri_api_logs_clear = { .uri = "/api/logs/clear", .method = HTTP_POST,
        .handler = handler_api_logs_clear, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_api_logs_clear);

    /* STATIC */
    httpd_uri_t uri_static = { .uri = "/*", .method = HTTP_GET,
        .handler = handler_static_protected, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &uri_static);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);

    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!s_server) return ESP_OK;
    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;
    return ret;
}

bool web_server_is_running(void)
{
    return s_server != NULL;
}