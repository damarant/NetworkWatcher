/*
 * NetworkWatcher - time_service.h
 * Sincronizzazione SNTP senza task dedicato (risparmio RAM).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inizializza SNTP. Non crea task, usa esp_timer per i retry.
 */
esp_err_t time_service_init(void);

/**
 * @brief Ritorna true se l'orario è stato sincronizzato.
 */
bool time_service_is_synced(void);

/**
 * @brief Ritorna il timestamp Unix corrente, o 0 se non sincronizzato.
 */
time_t time_service_now(void);

#ifdef __cplusplus
}
#endif