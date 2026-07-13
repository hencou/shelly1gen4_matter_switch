#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime hardware profile.
 *
 * The Gen4 line shares one ESP32-C6 module (ESP-Shelly-C68F) but wires the
 * relay, wall-switch input, onboard button and status LED to different GPIOs
 * per device. Pin assignments (verified on real hardware) come from the
 * automatous-io/shelly-1-gen4-matter-thread project's docs/GPIO.md.
 *
 * The active profile is chosen at runtime (stored in NVS, selectable on the
 * management dashboard) instead of compile time, so one firmware image serves
 * every supported device. Changing it only remaps GPIOs — our Matter data
 * model is a generic switch/relay, so no re-commissioning is required.
 */
typedef enum {
    HW_1_GEN4      = 0,   /* Shelly 1 Gen4 (full size, Add-on connector) */
    HW_1_MINI_GEN4 = 1,   /* Shelly 1 Mini Gen4 */
    HW_1PM_GEN4    = 2,   /* Shelly 1PM Gen4 (BL0942 power meter) */
    HW_TYPE_COUNT
} hw_device_type_t;

typedef struct {
    hw_device_type_t type;
    const char *name;        /* human-readable, for dashboard/logs */
    int relay_gpio;
    int switch_gpio;         /* wall-switch / pushbutton input */
    int button_gpio;         /* onboard pair button (active-low) */
    int led_gpio;            /* status LED (-1 = none) */
    bool led_active_high;
    bool has_addon;          /* Shelly Plus Add-on inputs (1-Wire/touch/analog) */
    bool has_pm;             /* BL0942 power meter present */
    int pm_uart_tx;          /* BL0942 UART TX (valid when has_pm) */
    int pm_uart_rx;          /* BL0942 UART RX (valid when has_pm) */
} hw_profile_t;

/* Load the active device type from NVS and select the matching profile.
 * Call once early in app_main, before any driver init. Falls back to
 * HW_1_GEN4 when nothing is stored or the stored value is invalid. */
void hw_config_init(void);

/* The active profile (valid after hw_config_init). */
const hw_profile_t *hw_profile(void);

/* Persist a new device type to NVS. Caller reboots afterwards so the new
 * profile takes effect. */
esp_err_t hw_device_type_set(hw_device_type_t type);

/* Profile table lookup for a given type (for the dashboard listing). */
const hw_profile_t *hw_profile_for(hw_device_type_t type);

#ifdef __cplusplus
}
#endif
