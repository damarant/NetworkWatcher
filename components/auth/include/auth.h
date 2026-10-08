/*
 * NetworkWatcher - auth.h
 * Autenticazione con SHA-256 + salt, sessioni cookie-based.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * COSTANTI
 * ============================================================ */

#define AUTH_SALT_LEN              16
#define AUTH_HASH_LEN              32      /* SHA-256 output */
#define AUTH_HASH_HEX_LEN          64      /* hex di 32 byte */
#define AUTH_TOKEN_LEN             32      /* byte randomici */
#define AUTH_TOKEN_HEX_LEN         64      /* hex di 32 byte */
#define AUTH_USER_MAX_LEN          32
#define AUTH_PASS_MIN_LEN          8
#define AUTH_PASS_MAX_LEN          64

#define AUTH_MAX_LOGIN_ATTEMPTS    5
#define AUTH_LOCKOUT_SECONDS       300     /* 5 minuti */

#define AUTH_COOKIE_NAME           "nw_session"

/* ============================================================
 * INIT
 * ============================================================ */

/**
 * @brief Inizializza il modulo auth.
 *        Se le credenziali non esistono in NVS, crea quelle di default
 *        (admin/admin) con must_change_password = true.
 */
esp_err_t auth_init(void);

/* ============================================================
 * LOGIN / LOGOUT
 * ============================================================ */

/**
 * @brief Verifica username e password.
 *
 * @param user      Username inserito
 * @param pass      Password inserita
 * @param out_token Buffer dove scrivere il token di sessione (deve essere
 *                  almeno AUTH_TOKEN_HEX_LEN+1)
 * @param token_len Dimensione del buffer
 * @return
 *   - ESP_OK se credenziali valide e sessione creata
 *   - ESP_ERR_INVALID_ARG se argomenti errati
 *   - ESP_ERR_INVALID_STATE se in lockout per troppi tentativi
 *   - ESP_FAIL se credenziali errate
 */
esp_err_t auth_login(const char *user, const char *pass,
                     char *out_token, size_t token_len);

/**
 * @brief Invalida la sessione corrente.
 */
void auth_logout(const char *token);

/* ============================================================
 * VERIFICA SESSIONE
 * ============================================================ */

/**
 * @brief Verifica se il token e' una sessione valida e non scaduta.
 */
bool auth_check_session(const char *token);

/**
 * @brief Estende la scadenza della sessione (refresh ultimo utilizzo).
 */
void auth_touch_session(const char *token);

/* ============================================================
 * CAMBIO PASSWORD
 * ============================================================ */

/**
 * @brief Cambia password dell'utente admin.
 *        Richiede la password attuale per conferma.
 */
esp_err_t auth_change_password(const char *old_pass, const char *new_pass);

/* ============================================================
 * STATO
 * ============================================================ */

/**
 * @brief Restituisce true se l'admin deve ancora cambiare la password.
 */
bool auth_must_change_password(void);

/**
 * @brief Restituisce true se il sistema e' in lockout per troppi tentativi.
 */
bool auth_is_locked_out(void);

/**
 * @brief Secondi rimanenti al termine del lockout (0 se non in lockout).
 */
int auth_lockout_remaining(void);

/**
 * @brief Restituisce lo username admin corrente.
 */
esp_err_t auth_get_username(char *out, size_t len);

/**
 * @brief Rimuove tutte le sessioni (per riavvio/reset).
 */
void auth_clear_sessions(void);

#ifdef __cplusplus
}
#endif
