/*
 * NetworkWatcher - oui_lookup.h
 * Lookup del produttore (vendor) di un MAC basato sui primi 3 byte (OUI).
 *
 * Tabella ridotta: ~60 produttori più diffusi in ambiente domestico.
 * Se il MAC non matcha nessun OUI noto, ritorna NULL.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ritorna il nome del produttore per il MAC dato, o NULL se sconosciuto.
 *        Il puntatore ritornato è statico, non va liberato.
 *
 * @param mac  6 byte del MAC (OUI nei primi 3)
 * @return     Nome vendor (stringa statica) o NULL
 */
const char *oui_lookup_vendor(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif