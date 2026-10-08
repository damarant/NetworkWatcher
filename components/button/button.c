/*
 * NetworkWatcher - button.c
 * Button press detection (BOOT/GPIO0).
 */

#include "button.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "button";

static btn_callback_t  s_callback = NULL;
static TaskHandle_t    s_task     = NULL;
static volatile bool   s_pressed  = false;

/* ============================================================
 * HELPER: read physical button state
 * GPIO0 is active low: 0 = pressed, 1 = released
 * ============================================================ */

static inline bool btn_hw_pressed(void)
{
    return gpio_get_level(NW_BUTTON_GPIO) == 0;
}

/* ============================================================
 * HELPER: notify event to caller
 * ============================================================ */

static void notify(btn_event_t ev)
{
    if (ev == BTN_EVENT_NONE) return;
    ESP_LOGI(TAG, "event: %d", (int)ev);
    if (s_callback) {
        s_callback(ev);
    }
}

/* ============================================================
 * MAIN DETECTION TASK
 * ============================================================ */

static void button_task(void *arg)
{
    ESP_LOGI(TAG, "button task started (GPIO%d)", NW_BUTTON_GPIO);

    const TickType_t poll_ticks = pdMS_TO_TICKS(20);

    bool    was_pressed         = false;
    int64_t press_start_us      = 0;
    int64_t release_time_us     = 0;

    int     short_click_count   = 0;
    bool    long_fired          = false;

    bool    in_sequence         = false;
    int64_t sequence_start_us   = 0;

    while (1) {
        bool now_pressed = btn_hw_pressed();
        int64_t now_us   = esp_timer_get_time();

        /* --- Rising edge (press started) --- */
        if (now_pressed && !was_pressed) {
            press_start_us = now_us;
            long_fired     = false;
            s_pressed      = true;
        }

        /* --- Long press --- */
        if (now_pressed && !long_fired) {
            int64_t held_ms = (now_us - press_start_us) / 1000;
            if (held_ms >= NW_BUTTON_LONG_PRESS_MS) {
                long_fired = true;

                if (in_sequence) {
                    in_sequence = false;
                    notify(BTN_EVENT_SEQUENCE_CANCELLED);
                }

                short_click_count = 0;
                notify(BTN_EVENT_LONG_PRESS);
            }
        }

        /* --- Falling edge (release) --- */
        if (!now_pressed && was_pressed) {
            int64_t held_ms = (now_us - press_start_us) / 1000;
            s_pressed = false;

            if (!long_fired && held_ms >= NW_BUTTON_DEBOUNCE_MS
                            && held_ms < NW_BUTTON_LONG_PRESS_MS) {

                if (in_sequence) {
                    in_sequence = false;
                    short_click_count = 0;
                    notify(BTN_EVENT_SEQUENCE_CONFIRMED);
                } else {
                    if (release_time_us > 0 &&
                        (now_us - release_time_us) / 1000 < NW_BUTTON_SEQUENCE_GAP_MS) {
                        short_click_count++;
                    } else {
                        short_click_count = 1;
                    }

                    if (short_click_count >= NW_BUTTON_SEQUENCE_COUNT) {
                        in_sequence = true;
                        sequence_start_us = now_us;
                        short_click_count = 0;
                        ESP_LOGW(TAG, "sequence detected: press again within %d ms to confirm reset",
                                 NW_BUTTON_RESET_CONFIRM_MS);
                        notify(BTN_EVENT_SEQUENCE_3);
                    } else {
                        notify(BTN_EVENT_SHORT_PRESS);
                    }
                }

                release_time_us = now_us;
            }
        }

        /* --- Reset confirmation timeout --- */
        if (in_sequence) {
            int64_t elapsed_ms = (now_us - sequence_start_us) / 1000;
            if (elapsed_ms >= NW_BUTTON_RESET_CONFIRM_MS) {
                in_sequence = false;
                short_click_count = 0;
                notify(BTN_EVENT_SEQUENCE_CANCELLED);
            }
        }

        /* --- Short clicks timeout --- */
        if (short_click_count > 0 && !in_sequence) {
            int64_t elapsed_ms = (now_us - release_time_us) / 1000;
            if (elapsed_ms >= NW_BUTTON_SEQUENCE_GAP_MS) {
                short_click_count = 0;
            }
        }

        was_pressed = now_pressed;
        vTaskDelay(poll_ticks);
    }
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

esp_err_t button_init(btn_callback_t callback)
{
    s_callback = callback;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << NW_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t ok = xTaskCreate(
        button_task,
        "button_task",
        3072,
        NULL,
        5,
        &s_task
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "button task creation failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "button initialized");
    return ESP_OK;
}

bool button_is_pressed(void)
{
    return s_pressed;
}