#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Event-type dat de button-driver uitstuurt.
 * - SHORT_PRESS      fires on release within LONG_PRESS_MS
 * - LONG_PRESS_START fires when held > LONG_PRESS_MS
 * - LONG_PRESS_STOP  fires on release after a LONG_PRESS_START
 * - MODE_TOGGLE      fires after MODE_TOGGLE_CLICKS (6) taps in MODE_TOGGLE_WINDOW_MS
 *                    Universeel op alle 3 inputs. Handler beslist op basis van
 *                    huidige boot-mode:
 *                      - Matter mode -> reboot in OTA mode
 *                      - OTA mode    -> factory reset (wipe nvs + chip_kvs)
 */
typedef enum {
    BTN_EVT_SHORT_PRESS = 0,
    BTN_EVT_LONG_PRESS_START,
    BTN_EVT_LONG_PRESS_STOP,
    BTN_EVT_MODE_TOGGLE,
} button_event_t;

typedef void (*button_cb_t)(input_id_t input, button_event_t evt);

void button_driver_init(button_cb_t cb);

#ifdef __cplusplus
}
#endif
