/*
 * NetworkWatcher - arp_scanner.h
 * Scansione rete via ARP request broadcast + lettura tabella ARP.
 * Task unico con scansione periodica interna.
 *
 * NOTA: nw_arp_scan_batch_t è definito in config.h (per rompere
 *       la dipendenza circolare con mac_list).
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

/**
 * @brief Inizializza il componente e crea il task unico di scansione.
 */
esp_err_t arp_scanner_init(void);

/**
 * @brief Richiede una scansione immediata (non bloccante).
 */
esp_err_t arp_scanner_start_scan(void);

/**
 * @brief Restituisce true se una scansione è in corso.
 */
bool arp_scanner_is_scanning(void);

/**
 * @brief Copia l'ultimo risultato disponibile.
 */
esp_err_t arp_scanner_get_last_result(nw_arp_scan_batch_t *out);

/**
 * @brief Imposta l'intervallo tra scansioni automatiche in minuti.
 */
void arp_scanner_set_interval(int32_t minutes);

/**
 * @brief Restituisce l'intervallo corrente in minuti.
 */
int32_t arp_scanner_get_interval(void);

#ifdef __cplusplus
}
#endif