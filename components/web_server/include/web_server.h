/*
 * NetworkWatcher - web_server.h
 * HTTP server per la UI web.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inizializza e avvia il web server HTTP sulla porta 80.
 *        I file statici vengono serviti da /storage/www/.
 *
 * @return ESP_OK in caso di successo
 */
esp_err_t web_server_init(void);

/**
 * @brief Ferma il web server.
 */
esp_err_t web_server_stop(void);

/**
 * @brief Restituisce true se il server e' in esecuzione.
 */
bool web_server_is_running(void);

#ifdef __cplusplus
}
#endif