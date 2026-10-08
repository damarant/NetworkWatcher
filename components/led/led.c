/*
 * NetworkWatcher - led.c
 * LED control with asynchronous patterns.
 * Polarity configurable via NW_LED_ACTIVE_LOW in config.h.
 */

#include "led.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "led";

static led_pattern_t s_current_pattern = LED_PATTERN_OFF;
static volatile bool s_initialized = false;
static TaskHandle_t  s_led_task = NULL;

/* Flag for led_pulse() */
static volatile bool s_pulse_request = false;

/* ============================================================
 * HELPER: apply physical LED state
 * ============================================================ */

static inline void led_hw_set(bool on)
{
#if NW_LED_ACTIVE_LOW
    /* Active low: GPIO=0 on, GPIO=1 off */
    gpio_set_level(NW_LED_GPIO, on ? 0 : 1);
#else
    /* Active high: GPIO=1 on, GPIO=0 off */
    gpio_set_level(NW_LED_GPIO, on ? 1 : 0);
#endif
}

/* ============================================================
 * PATTERN TASK
 * ============================================================ */

static void led_task(void *arg)
{
    ESP_LOGI(TAG, "LED task started");

    while (1) {
        if (s_pulse_request) {
            s_pulse_request = false;
            led_hw_set(true);
            vTaskDelay(pdMS_TO_TICKS(50));
            led_hw_set(false);
        }

        switch (s_current_pattern) {

        case LED_PATTERN_OFF:
            led_hw_set(false);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;

        case LED_PATTERN_ON:
            led_hw_set(true);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;

        case LED_PATTERN_BLINK_SLOW:
            led_hw_set(true);
            vTaskDelay(pdMS_TO_TICKS(500));
            led_hw_set(false);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case LED_PATTERN_BLINK_FAST:
            led_hw_set(true);
            vTaskDelay(pdMS_TO_TICKS(100));
            led_hw_set(false);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case LED_PATTERN_HEARTBEAT:
            led_hw_set(true);
            vTaskDelay(pdMS_TO_TICKS(80));
            led_hw_set(false);
            vTaskDelay(pdMS_TO_TICKS(120));
            led_hw_set(true);
            vTaskDelay(pdMS_TO_TICKS(80));
            led_hw_set(false);
            vTaskDelay(pdMS_TO_TICKS(1720));
            break;

        case LED_PATTERN_ERROR:
            for (int i = 0; i < 3; i++) {
                led_hw_set(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                led_hw_set(false);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;

        case LED_PATTERN_SCAN:
            led_hw_set(true);
            vTaskDelay(pdMS_TO_TICKS(30));
            led_hw_set(false);
            vTaskDelay(pdMS_TO_TICKS(470));
            break;

        default:
            led_hw_set(false);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }
    }
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

esp_err_t led_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "led already initialized");
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << NW_LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    led_hw_set(false);

    BaseType_t ok = xTaskCreate(
        led_task,
        "led_task",
        2048,
        NULL,
        5,
        &s_led_task
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "LED task creation failed");
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "LED initialized on GPIO%d (active %s)",
             NW_LED_GPIO, NW_LED_ACTIVE_LOW ? "LOW" : "HIGH");
    return ESP_OK;
}

void led_set_pattern(led_pattern_t pattern)
{
    if (!s_initialized) return;
    s_current_pattern = pattern;
}

led_pattern_t led_get_pattern(void)
{
    return s_current_pattern;
}

void led_on(void)
{
    led_set_pattern(LED_PATTERN_ON);
}

void led_off(void)
{
    led_set_pattern(LED_PATTERN_OFF);
}

void led_pulse(void)
{
    s_pulse_request = true;
}