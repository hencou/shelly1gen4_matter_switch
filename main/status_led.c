/*
 * Eenvoudige status-LED driver met esp_timer-callback.
 * Patronen worden in software gegenereerd (geen LEDC nodig) — voldoende
 * voor 1-5 Hz blink op de Shelly Add-on LED.
 *
 * Thread-safety: set/blip kunnen vanuit elke task gecalled worden;
 * de esp_timer callback draait altijd op dezelfde timer-task.
 */

#include "status_led.h"
#include "app_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "status_led";

static bool s_enabled = false;
static int  s_pin = -1;
static bool s_active_high = true;

static status_led_pattern_t s_pattern = STATUS_LED_OFF;
static esp_timer_handle_t   s_timer = NULL;
static bool                 s_phase = false;       /* huidige aan/uit-fase */
static int64_t              s_blip_end_us = 0;     /* puls-deadline (0 = inactief) */

static inline void led_set_raw(bool on)
{
    if (!s_enabled) return;
    int level = (on == s_active_high) ? 1 : 0;
    gpio_set_level((gpio_num_t)s_pin, level);
}

static void schedule_next(uint32_t period_us);

static void timer_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();

    /* Een actieve blip houdt LED kort aan ongeacht patroon. */
    if (s_blip_end_us != 0) {
        if (now < s_blip_end_us) {
            led_set_raw(true);
            schedule_next(10 * 1000);  /* check elke 10 ms */
            return;
        }
        s_blip_end_us = 0;
        /* val door naar normaal patroon */
    }

    switch (s_pattern) {
    case STATUS_LED_OFF:
        led_set_raw(false);
        return;  /* timer niet rescheduelen */
    case STATUS_LED_ON:
        led_set_raw(true);
        return;
    case STATUS_LED_SLOW_BLINK:
        s_phase = !s_phase;
        led_set_raw(s_phase);
        schedule_next(500 * 1000);   /* 1 Hz */
        break;
    case STATUS_LED_FAST_BLINK:
        s_phase = !s_phase;
        led_set_raw(s_phase);
        schedule_next(100 * 1000);   /* 5 Hz */
        break;
    case STATUS_LED_HEARTBEAT:
        if (s_phase) {
            led_set_raw(false);
            s_phase = false;
            schedule_next(1950 * 1000);  /* 1.95 s donker */
        } else {
            led_set_raw(true);
            s_phase = true;
            schedule_next(50 * 1000);    /* 50 ms aan */
        }
        break;
    }
}

static void schedule_next(uint32_t period_us)
{
    if (!s_timer) return;
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, period_us);
}

esp_err_t status_led_init(void)
{
    s_pin = PIN_STATUS_LED;
    s_active_high = STATUS_LED_ACTIVE_HIGH;

    if (s_pin < 0) {
        ESP_LOGI(TAG, "Status LED uitgeschakeld (PIN_STATUS_LED=%d)", s_pin);
        return ESP_OK;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(%d) faalde: %s", s_pin, esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t targs = {
        .callback = &timer_cb,
        .name     = "status_led",
    };
    err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create faalde: %s", esp_err_to_name(err));
        return err;
    }

    s_enabled = true;
    led_set_raw(false);
    ESP_LOGI(TAG, "Status LED actief op GPIO%d (active_%s)",
             s_pin, s_active_high ? "high" : "low");
    return ESP_OK;
}

void status_led_set(status_led_pattern_t pattern)
{
    if (!s_enabled) return;
    s_pattern = pattern;
    s_phase = false;
    /* Trigger callback direct zodat nieuwe patroon meteen ingaat. */
    schedule_next(1000);  /* 1 ms */
}

void status_led_blip(void)
{
    if (!s_enabled) return;
    s_blip_end_us = esp_timer_get_time() + 50 * 1000;  /* 50 ms */
    schedule_next(1000);
}
