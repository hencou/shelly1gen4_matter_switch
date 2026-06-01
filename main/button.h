#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Event type emitted by the button driver.
 * - SHORT_PRESS      fires on release within LONG_PRESS_MS
 * - LONG_PRESS_START fires when held > LONG_PRESS_MS
 * - LONG_PRESS_STOP  fires on release after a LONG_PRESS_START
 * - MODE_TOGGLE      fires after MODE_TOGGLE_CLICKS (6) taps in MODE_TOGGLE_WINDOW_MS
 *                    Universal across all 3 inputs. Handler decides based on
 *                    current boot mode:
 *                      - Matter mode -> reboot into OTA mode
 *                      - OTA mode    -> factory reset (wipe nvs + chip_kvs)
 * - CONTACT_CLOSED   fires on every press/close edge (state-following)
 * - CONTACT_OPEN     fires on every release/open edge (state-following)
 *   These two events enable EP5 (state-follow switch): bind EP5 for a
 *   maintained switch (On/Off follows contact), bind EP1 for momentary (Toggle).
 */
typedef enum {
    BTN_EVT_SHORT_PRESS = 0,
    BTN_EVT_LONG_PRESS_START,
    BTN_EVT_LONG_PRESS_STOP,
    BTN_EVT_MODE_TOGGLE,
    BTN_EVT_CONTACT_CLOSED,
    BTN_EVT_CONTACT_OPEN,
} button_event_t;

typedef void (*button_cb_t)(input_id_t input, button_event_t evt);

void button_driver_init(button_cb_t cb);

#ifdef __cplusplus
}
#endif
