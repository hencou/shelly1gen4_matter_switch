/*
 * Runtime hardware profile selection. See hw_config.h.
 *
 * Pin assignments per device. The 1/Mini/1PM pins are taken from published
 * Gen4 GPIO documentation; the 2PM pins follow the ESPHome device config (see
 * the 2PM note below and README.md — only the 1 Gen4 is confirmed on hardware
 * here):
 *
 *   Function      | 1 Gen4 | 1 Mini Gen4 | 1PM Gen4              | 2PM Gen4
 *   Relay         | GPIO5  | GPIO10      | GPIO4                | GPIO5 + GPIO3
 *   Switch input  | GPIO10 | GPIO12      | GPIO10               | GPIO11 + GPIO10
 *   Button        | GPIO4  | GPIO22      | GPIO1                | GPIO12
 *   Status LED    | GPIO15 | GPIO5       | GPIO0                | GPIO0   (all active-low)
 *   Power meter   | -      | -           | BL0942 UART1         | ADE7953 I2C
 *                 |        |             | TX=GPIO6 RX=GPIO7    | SDA=GPIO6 SCL=GPIO7 IRQ=GPIO19
 *   Add-on        | yes    | no          | yes                  | yes
 *
 * Only the Mini lacks the Shelly Plus Add-on connector; the 1 Gen4, 1PM Gen4
 * and 2PM Gen4 all expose it (1-Wire/touch/analog on GPIO9/16/17/18).
 */

#include "hw_config.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "hw_config";
static const char *NVS_NS  = "hw";
static const char *NVS_KEY = "dev_type";

static const hw_profile_t s_profiles[HW_TYPE_COUNT] = {
    [HW_1_GEN4] = {
        .type = HW_1_GEN4, .name = "Shelly 1 Gen4",
        .relay_gpio = 5, .relay2_gpio = -1,
        .switch_gpio = 10, .switch2_gpio = -1, .button_gpio = 4,
        .led_gpio = 15, .led_active_high = false,
        .has_addon = true, .has_pm = false, .pm_type = PM_NONE,
        .pm_uart_tx = -1, .pm_uart_rx = -1,
        .pm_i2c_sda = -1, .pm_i2c_scl = -1, .pm_i2c_irq = -1,
    },
    [HW_1_MINI_GEN4] = {
        .type = HW_1_MINI_GEN4, .name = "Shelly 1 Mini Gen4",
        .relay_gpio = 10, .relay2_gpio = -1,
        .switch_gpio = 12, .switch2_gpio = -1, .button_gpio = 22,
        .led_gpio = 5, .led_active_high = false,
        .has_addon = false, .has_pm = false, .pm_type = PM_NONE,
        .pm_uart_tx = -1, .pm_uart_rx = -1,
        .pm_i2c_sda = -1, .pm_i2c_scl = -1, .pm_i2c_irq = -1,
    },
    [HW_1PM_GEN4] = {
        .type = HW_1PM_GEN4, .name = "Shelly 1PM Gen4",
        .relay_gpio = 4, .relay2_gpio = -1,
        .switch_gpio = 10, .switch2_gpio = -1, .button_gpio = 1,
        .led_gpio = 0, .led_active_high = false,
        .has_addon = true, .has_pm = true, .pm_type = PM_BL0942,
        .pm_uart_tx = 6, .pm_uart_rx = 7,
        .pm_i2c_sda = -1, .pm_i2c_scl = -1, .pm_i2c_irq = -1,
    },
    [HW_2PM_GEN4] = {
        /* Shelly Plus 2PM Gen4 — pins per the working ESPHome device config
         * (relays on GPIO5/GPIO3, switches on GPIO11/GPIO10). The human-readable
         * "GPIO Pinout" table on esphome.io swaps relay/switch on these four
         * pins; VERIFY on real hardware before connecting mains. */
        .type = HW_2PM_GEN4, .name = "Shelly 2PM Gen4",
        .relay_gpio = 5, .relay2_gpio = 3,
        .switch_gpio = 11, .switch2_gpio = 10, .button_gpio = 12,
        .led_gpio = 0, .led_active_high = false,
        .has_addon = true, .has_pm = true, .pm_type = PM_ADE7953,
        .pm_uart_tx = -1, .pm_uart_rx = -1,
        .pm_i2c_sda = 6, .pm_i2c_scl = 7, .pm_i2c_irq = 19,
    },
};

static const hw_profile_t *s_active = &s_profiles[HW_1_GEN4];

const hw_profile_t *hw_profile_for(hw_device_type_t type)
{
    if ((int)type < 0 || (int)type >= HW_TYPE_COUNT) return &s_profiles[HW_1_GEN4];
    return &s_profiles[type];
}

void hw_config_init(void)
{
    uint8_t v = HW_1_GEN4;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &v);
        nvs_close(h);
    }
    if (v >= HW_TYPE_COUNT) v = HW_1_GEN4;
    s_active = &s_profiles[v];
    ESP_LOGI(TAG, "device type = %d (%s): relay=GPIO%d switch=GPIO%d button=GPIO%d led=GPIO%d addon=%d pm=%d",
             s_active->type, s_active->name, s_active->relay_gpio, s_active->switch_gpio,
             s_active->button_gpio, s_active->led_gpio, s_active->has_addon, s_active->has_pm);
}

const hw_profile_t *hw_profile(void)
{
    return s_active;
}

esp_err_t hw_device_type_set(hw_device_type_t type)
{
    if ((int)type < 0 || (int)type >= HW_TYPE_COUNT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY, (uint8_t)type);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "device type saved: %d (%s)", type, s_profiles[type].name);
    return err;
}
