/*
 * NetworkWatcher - log_buffer.c
 * Ring buffer di log WARNING/ERROR in RAM.
 *
 * Fase 3.5.
 *
 * Design:
 *  - Hook su esp_log_set_vprintf: riceve ogni riga di log gia' formattata
 *    (o il formato+args, a seconda della versione IDF; gestiamo entrambi)
 *  - Filtra solo W/E
 *  - Scrive in un ring buffer di 2 KB
 *  - Nessun mutex: indice write-only dal hook, read-only dal web server
 */

#include "log_buffer.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LOG_BUF_SIZE    2048
#define LOG_LINE_MAX    200

static char              s_buffer[LOG_BUF_SIZE];
static volatile size_t   s_write_pos = 0;
static volatile size_t   s_total_written = 0;
static vprintf_like_t    s_original_vprintf = NULL;
static bool              s_initialized = false;

/* ============================================================
 * HELPER: append riga al ring buffer
 * ============================================================ */

static void log_buffer_append_line(const char *line, size_t len)
{
    if (!line || len == 0) return;
    if (len > LOG_LINE_MAX) len = LOG_LINE_MAX;

    /* Assicura newline finale */
    bool need_newline = (line[len - 1] != '\n');

    for (size_t i = 0; i < len; i++) {
        s_buffer[s_write_pos] = line[i];
        s_write_pos = (s_write_pos + 1) % LOG_BUF_SIZE;
        s_total_written++;
    }
    if (need_newline) {
        s_buffer[s_write_pos] = '\n';
        s_write_pos = (s_write_pos + 1) % LOG_BUF_SIZE;
        s_total_written++;
    }
}

/* ============================================================
 * HOOK vprintf
 *
 * NOTA CRITICA: non chiamare MAI ESP_LOG* qui dentro, altrimenti
 * ricorsione infinita.
 * ============================================================ */

static int log_buffer_vprintf(const char *fmt, va_list args)
{
    /* 1. Stampa sempre su console (con copia della va_list) */
    va_list args_console;
    va_copy(args_console, args);
    int ret = s_original_vprintf
                ? s_original_vprintf(fmt, args_console)
                : vprintf(fmt, args_console);
    va_end(args_console);

    /* 2. Se non inizializzato, salta la cattura */
    if (!s_initialized || !fmt) return ret;

    /* 3. Formatta in un buffer locale per analisi.
     *    In ESP-IDF v6, `fmt` può essere già la stringa formattata
     *    oppure il formato + args. vsnprintf gestisce entrambi i casi. */
    char line[LOG_LINE_MAX + 1];
    va_list args_fmt;
    va_copy(args_fmt, args);
    int n = vsnprintf(line, sizeof(line), fmt, args_fmt);
    va_end(args_fmt);

    if (n <= 0) return ret;

    /* 4. Filtra solo WARNING (W) e ERROR (E) */
    if ((line[0] == 'W' || line[0] == 'E') &&
        (line[1] == ' ' || line[1] == '(' || line[1] == '\0')) {
        log_buffer_append_line(line, (size_t)n);
    }

    return ret;
}

/* ============================================================
 * API PUBBLICA
 * ============================================================ */

esp_err_t log_buffer_init(void)
{
    if (s_initialized) return ESP_OK;

    memset(s_buffer, 0, sizeof(s_buffer));
    s_write_pos = 0;
    s_total_written = 0;

    /* Registra l'hook. Da questo momento in poi ogni riga passa di qui. */
    s_original_vprintf = esp_log_set_vprintf(log_buffer_vprintf);

    s_initialized = true;
    return ESP_OK;
}

size_t log_buffer_read(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;

    /* Snapshot degli indici (lettura "quasi" consistente) */
    size_t wp    = s_write_pos;
    size_t total = s_total_written;

    size_t start;
    size_t available;

    if (total < LOG_BUF_SIZE) {
        /* Buffer non ancora wrappato: dal principio a write_pos */
        start = 0;
        available = (wp > total) ? total : wp;
    } else {
        /* Buffer pieno o wrappato: da write_pos in poi (più vecchio) */
        start = wp;
        available = LOG_BUF_SIZE;
    }

    if (available > out_len - 1) available = out_len - 1;

    for (size_t i = 0; i < available; i++) {
        size_t idx = (start + i) % LOG_BUF_SIZE;
        out[i] = s_buffer[idx];
    }
    out[available] = '\0';

    return available;
}

void log_buffer_clear(void)
{
    memset(s_buffer, 0, sizeof(s_buffer));
    s_write_pos = 0;
    s_total_written = 0;
}