/*
 * NetworkWatcher - log_buffer.h
 * Ring buffer in RAM per gli ultimi log a livello WARNING/ERROR.
 *
 * Fase 3.5.
 *
 * - Buffer circolare di 2 KB
 * - Solo WARNING + ERROR (Info/Debug/Verbose ignorati)
 * - Volatile: si perde al riavvio
 * - Thread-safe: indice di scrittura atomico, lettura con tolleranza
 *   a piccole incoerenze (accettabile per un log)
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inizializza il ring buffer e installa l'hook su esp_log_set_vprintf.
 *        Da chiamare come PRIMA cosa in app_main, per catturare piu' log.
 *
 * IMPORTANTE: la callback del hook NON usa ESP_LOG* (eviterebbe ricorsione).
 */
esp_err_t log_buffer_init(void);

/**
 * @brief Copia il contenuto del buffer in `out`, in ordine cronologico
 *        (piu' vecchio -> piu' recente).
 *
 * @param out       Buffer di destinazione
 * @param out_len   Dimensione del buffer (incluso terminatore)
 * @return Numero di byte copiati (escluso terminatore)
 */
size_t log_buffer_read(char *out, size_t out_len);

/**
 * @brief Svuota completamente il buffer.
 */
void log_buffer_clear(void);

#ifdef __cplusplus
}
#endif