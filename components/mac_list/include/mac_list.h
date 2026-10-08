/*
 * NetworkWatcher - mac_list.h
 * Gestione liste MAC (whitelist, blacklist, unknown).
 *
 * Fase 3.3:
 *  - Tracking ultimi IP visti (RAM, non persistito)
 *  - mac_list_get_ip() per la UI (colonna IP sotto il MAC per gli unknown)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mac_list_init(void);
esp_err_t mac_list_load(nw_list_type_t type);
esp_err_t mac_list_save(nw_list_type_t type);
esp_err_t mac_list_add(nw_list_type_t type, const uint8_t mac[6], const char *desc);
esp_err_t mac_list_remove(nw_list_type_t type, const uint8_t mac[6]);
esp_err_t mac_list_move(nw_list_type_t from, nw_list_type_t to, const uint8_t mac[6]);
esp_err_t mac_list_update_desc(nw_list_type_t type, const uint8_t mac[6], const char *desc);
esp_err_t mac_list_clear(nw_list_type_t type);
esp_err_t mac_list_find(const uint8_t mac[6], nw_list_type_t *found_in);
bool      mac_list_contains(nw_list_type_t type, const uint8_t mac[6]);
size_t    mac_list_count(nw_list_type_t type);
const nw_list_entry_t *mac_list_get(nw_list_type_t type, size_t index);

/**
 * @brief Processa un batch ARP: aggiorna le liste e (opzionalmente)
 *        compila un delta con i nuovi unknown e i match in blacklist.
 *        Aggiorna anche la cache IP interni (per mac_list_get_ip).
 */
int       mac_list_process_scan(const nw_arp_scan_batch_t *scan,
                                nw_scan_delta_t *delta_out);

/**
 * @brief Ritorna l'ultimo IP visto per un MAC (come stringa "192.168.1.100").
 *        Se il MAC non e' nella cache, ritorna ESP_ERR_NOT_FOUND.
 *        La cache e' in RAM e viene riempita dalle scansioni ARP.
 */
esp_err_t mac_list_get_ip(const uint8_t mac[6], char *out, size_t out_len);

bool      mac_list_blacklist_present(void);
bool      mac_list_is_time_synced(void);
void      mac_list_set_time_synced(bool synced);
const char *mac_list_type_name(nw_list_type_t type);
int        mac_list_type_from_name(const char *name);
void       mac_to_string(const uint8_t mac[6], char out[18]);
bool       string_to_mac(const char *str, uint8_t out[6]);

#ifdef __cplusplus
}
#endif