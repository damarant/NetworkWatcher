/*
 * NetworkWatcher - storage.h
 * Wrapper per NVS (chiave-valore) e LittleFS (file).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * INIZIALIZZAZIONE
 * ============================================================ */

/**
 * @brief Inizializza NVS e monta LittleFS.
 *        Da chiamare una sola volta all'avvio, prima di ogni altra
 *        funzione di storage.
 *
 * @return ESP_OK in caso di successo, altrimenti errore.
 */
esp_err_t storage_init(void);

/* ============================================================
 * NVS - STRINGHE
 * ============================================================ */

/**
 * @brief Legge una stringa dalla NVS.
 *
 * @param key    Chiave NVS
 * @param out    Buffer di destinazione
 * @param len    Dimensione del buffer (incluso terminatore)
 * @return ESP_OK se trovata, ESP_ERR_NVS_NOT_FOUND se assente
 */
esp_err_t storage_get_string(const char *key, char *out, size_t len);

/**
 * @brief Scrive una stringa nella NVS.
 */
esp_err_t storage_set_string(const char *key, const char *value);

/* ============================================================
 * NVS - INTERI
 * ============================================================ */

/**
 * @brief Legge un intero (32 bit) dalla NVS.
 *
 * @param key        Chiave NVS
 * @param out        Puntatore al valore di destinazione
 * @param default_v  Valore di default se la chiave non esiste
 */
esp_err_t storage_get_int(const char *key, int32_t *out, int32_t default_v);

/**
 * @brief Scrive un intero (32 bit) nella NVS.
 */
esp_err_t storage_set_int(const char *key, int32_t value);

/* ============================================================
 * NVS - BOOLEAN
 * ============================================================ */

esp_err_t storage_get_bool(const char *key, bool *out, bool default_v);
esp_err_t storage_set_bool(const char *key, bool value);

/* ============================================================
 * NVS - BLOB
 * ============================================================ */

esp_err_t storage_get_blob(const char *key, void *out, size_t len);
esp_err_t storage_set_blob(const char *key, const void *data, size_t len);

/* ============================================================
 * NVS - CANCELLAZIONE
 * ============================================================ */

/**
 * @brief Cancella una chiave dalla NVS.
 */
esp_err_t storage_erase_key(const char *key);

/**
 * @brief Cancella l'intero namespace NVS del progetto.
 *        Usata dal reset di fabbrica.
 */
esp_err_t storage_erase_all(void);

/* ============================================================
 * LITTLEFS - FILE
 * ============================================================ */

/**
 * @brief Legge un file di testo in un buffer allocato dinamicamente.
 *        Il chiamante deve liberare il buffer con free().
 *
 * @param path   Percorso assoluto (es. "/storage/whitelist.json")
 * @param out    Puntatore al buffer allocato
 * @param len    Lunghezza del contenuto letto (escluso terminatore)
 * @return ESP_OK in caso di successo
 */
esp_err_t storage_read_file(const char *path, char **out, size_t *len);

/**
 * @brief Scrive un buffer in un file (sovrascrive se esiste).
 */
esp_err_t storage_write_file(const char *path, const char *data, size_t len);

/**
 * @brief Elimina un file.
 */
esp_err_t storage_delete_file(const char *path);

/**
 * @brief Verifica se un file esiste.
 */
bool storage_file_exists(const char *path);

/**
 * @brief Restituisce la dimensione di un file in byte, o -1 se non esiste.
 */
long storage_file_size(const char *path);

/* ============================================================
 * LITTLEFS - INFO
 * ============================================================ */

/**
 * @brief Ottiene spazio totale e libero della partizione LittleFS.
 */
esp_err_t storage_get_fs_info(size_t *total, size_t *used);

#ifdef __cplusplus
}
#endif
