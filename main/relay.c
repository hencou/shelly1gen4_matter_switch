/*
 * Local relay on GPIO5. Controlled via Matter EP4 (OnOff Light server).
 * State is persisted in NVS for power-outage recovery.
 */

#include "relay.h"
#include "app_config.h"
#include "hw_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "relay";
static const char *NVS_NAMESPACE = "relay";
static const char *NVS_KEY_STATE = "state";

static bool s_state;
static int  s_relay_gpio = -1;

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_STATE, s_state ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static bool restore(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    nvs_get_u8(h, NVS_KEY_STATE, &v);
    nvs_close(h);
    return v != 0;
}

void relay_init(void)
{
    s_relay_gpio = hw_profile()->relay_gpio;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << s_relay_gpio),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);

    s_state = restore();
    gpio_set_level(s_relay_gpio, s_state ? 1 : 0);
    ESP_LOGI(TAG, "relay init on GPIO%d, state=%s", s_relay_gpio, s_state ? "ON" : "OFF");
}

void relay_set(bool on)
{
    s_state = on;
    gpio_set_level(s_relay_gpio, on ? 1 : 0);
    persist();
}

bool relay_get(void)
{
    return s_state;
}

void relay_toggle(void)
{
    relay_set(!s_state);
}
