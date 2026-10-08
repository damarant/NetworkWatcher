/*
 * NetworkWatcher - mdns_service.h
 * mDNS: annuncio del dispositivo come network-watcher.local
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inizializza mDNS e annuncia il servizio HTTP.
 *        Deve essere chiamato DOPO che la rete è attiva
 *        (AP o STA con IP assegnato).
 *
 * @return ESP_OK in caso di successo
 */
esp_err_t mdns_service_init(void);

/**
 * @brief Ferma e libera mDNS.
 */
void mdns_service_stop(void);

/**
 * @brief Restituisce l'hostname mDNS configurato (es. "network-watcher.local").
 */
const char *mdns_service_get_hostname(void);

#ifdef __cplusplus
}
#endif