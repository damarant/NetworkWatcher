/*
 * NetworkWatcher - wifi_manager.h
 * Gestione AP/STA, scansione reti, retry, persistenza credenziali.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * STATI
 * ============================================================ */

typedef enum {
    NW_WIFI_STATE_IDLE = 0,
    NW_WIFI_STATE_AP,
    NW_WIFI_STATE_STA_CONNECTING,
    NW_WIFI_STATE_STA_CONNECTED,
    NW_WIFI_STATE_STA_DISCONNECTED,
    NW_WIFI_STATE_STA_RETRY,
} nw_wifi_state_t;

/* ============================================================
 * EVENTI
 * ============================================================ */

typedef enum {
    NW_WIFI_EVT_AP_STARTED = 0,
    NW_WIFI_EVT_AP_STOPPED,
    NW_WIFI_EVT_STA_CONNECTED,
    NW_WIFI_EVT_STA_GOT_IP,
    NW_WIFI_EVT_STA_DISCONNECTED,
} nw_wifi_event_t;

/* ============================================================
 * STRUTTURE
 * ============================================================ */

#define NW_WIFI_SSID_MAX_LEN     33
#define NW_WIFI_PASS_MAX_LEN     65

typedef struct {
    char     ssid[NW_WIFI_SSID_MAX_LEN];
    int8_t   rssi;
    uint8_t  authmode;
    uint8_t  channel;
} nw_wifi_ap_t;

typedef void (*wifi_manager_cb_t)(nw_wifi_event_t evt, void *data);

/* ============================================================
 * INIT
 * ============================================================ */

esp_err_t wifi_manager_init(wifi_manager_cb_t cb);

/* ============================================================
 * AP MODE
 * ============================================================ */

esp_err_t wifi_manager_start_ap(void);
esp_err_t wifi_manager_stop_ap(void);

/* ============================================================
 * STA MODE
 * ============================================================ */

esp_err_t wifi_manager_connect_sta(const char *ssid, const char *pass);
esp_err_t wifi_manager_disconnect_sta(void);

/* ============================================================
 * SCANSIONE
 * ============================================================ */

esp_err_t wifi_manager_scan(nw_wifi_ap_t *out, size_t max, size_t *found);

/* ============================================================
 * TEST CONNESSIONE (senza salvare)
 * ============================================================ */

/**
 * @brief Testa la connessione a una rete Wi-Fi senza salvare le credenziali.
 *        Usa modalità APSTA per non disconnettere i client dell'AP.
 *
 * @param ssid       SSID della rete da testare
 * @param pass       Password (NULL o "" per rete aperta)
 * @param out_ip     Buffer per IP assegnato (min 16 byte), può essere NULL
 * @param ip_len     Dimensione buffer IP
 * @param out_gw     Buffer per gateway (min 16 byte), può essere NULL
 * @param gw_len     Dimensione buffer gateway
 * @param out_dns    Buffer per DNS (min 16 byte), può essere NULL
 * @param dns_len    Dimensione buffer DNS
 * @param timeout_ms Timeout connessione in millisecondi
 * @return ESP_OK se connesso e IP ottenuto, ESP_FAIL se timeout/errore
 */
esp_err_t wifi_manager_test_sta(const char *ssid, const char *pass,
                                char *out_ip, size_t ip_len,
                                char *out_gw, size_t gw_len,
                                char *out_dns, size_t dns_len,
                                uint32_t timeout_ms);

/**
 * @brief Pulisce dopo un test: disconnette STA e torna in solo AP.
 */
esp_err_t wifi_manager_test_cleanup(void);

/* ============================================================
 * PERSISTENZA
 * ============================================================ */

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *pass);
esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len,
                                        char *pass, size_t pass_len);
esp_err_t wifi_manager_forget_credentials(void);
bool      wifi_manager_has_credentials(void);

/* ============================================================
 * INFO
 * ============================================================ */

nw_wifi_state_t wifi_manager_get_state(void);
esp_err_t       wifi_manager_get_ip(char *out, size_t len);
esp_err_t       wifi_manager_get_mac(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif