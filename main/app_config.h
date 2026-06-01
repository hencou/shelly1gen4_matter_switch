#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Pin mapping — adjust via `idf.py menuconfig` */
#define PIN_RELAY           CONFIG_PIN_RELAY
#define PIN_SWITCH_INPUT    CONFIG_PIN_SWITCH_INPUT
#define PIN_ONEWIRE_TX      CONFIG_PIN_ONEWIRE_TX      /* 1-Wire TX / data out (GPIO9) via ISO7221A */
#define PIN_ONEWIRE_RX      CONFIG_PIN_ONEWIRE_RX      /* 1-Wire RX / data in  (GPIO16) via ISO7221A */

/* Bench mode: during 3V3-USB-UART testing (without 230V on the pushbutton and
 * without DS18B20/LD2410 on the Add-on) GPIO10, GPIO9, GPIO16 and GPIO17 float.
 * GPIO10-ANYEDGE → ISR storm; GPIO16/17 are also the UART0 TX/RX pins on the
 * ESP32-C6 — if sensors.c reconfigures them, serial output dies.
 * BENCH_MODE=1:
 *   - GPIO10 gets internal pull-up (idle state high = released)
 *   - sensors_init() skips temp_task and occ_task (keeps GPIO9/16/17 free)
 * BENCH_MODE=0 (production): original paths, external pull on GPIO10
 * driven by 230V optocoupler is then correct.
 *
 * Build: idf.py build -DBENCH_MODE=1  (or change this default)
 * NB: in production (230V + Add-on) this MUST be 0!
 */
#ifndef BENCH_MODE
#define BENCH_MODE 0
#endif

/* Add-on inputs — always active. */
#define PIN_TOUCH_INPUT     CONFIG_PIN_TOUCH_INPUT      /* TTP223 capacitive touch (GPIO18) */
#define PIN_LD2410_INPUT    CONFIG_PIN_LD2410_INPUT      /* Analog IN — PWM duty cycle (GPIO17) */

#define PIN_STATUS_LED      CONFIG_PIN_STATUS_LED      /* Shelly Add-on LED, -1 = disabled */

/* Kconfig bool: only defined when y; absent (=n) → 0. */
#ifdef CONFIG_STATUS_LED_ACTIVE_HIGH
#define STATUS_LED_ACTIVE_HIGH 1
#else
#define STATUS_LED_ACTIVE_HIGH 0
#endif

/* Timings */
#define LONG_PRESS_MS       CONFIG_LONG_PRESS_MS
#define OCC_DEBOUNCE_MS     CONFIG_OCC_DEBOUNCE_MS
#define TEMP_REPORT_INT_S   CONFIG_TEMP_REPORT_INTERVAL_S
/* Mode toggle — two gestures, either triggers OTA mode (Matter→OTA)
 * or factory reset (OTA→Matter):
 *   1) 6× click within 2.5 s  (original method)
 *   2) Hold any button for 10 s (alternative — works reliably even
 *      when many bindings are active, because no binding commands
 *      are sent for a sustained hold unlike 6× rapid toggles)
 * Long press does NOT trigger factory reset (too risky with
 * a wall switch that accidentally stays pressed), but the 10 s
 * very-long-press is intentional enough to be safe. */
#define MODE_TOGGLE_CLICKS      6
#define MODE_TOGGLE_WINDOW_MS   2500
#define VERY_LONG_PRESS_MS      10000   /* 10 s hold → OTA mode */

/* Matter endpoints */
#define EP_SWITCH_PUSHBUTTON   1
#define EP_TEMPERATURE      2
#define EP_OCCUPANCY        3
#define EP_RELAY            4
#define EP_SWITCH_STATE     5

/* Logical input source identifiers.
 * All 3 inputs have uniform behavior (see on_button_event in app_main.cpp):
 *   - SHORT_PRESS       -> Matter Toggle to EP1 bound devices (momentary)
 *   - CONTACT_CLOSED    -> Matter On to EP5 bound devices (state-follow)
 *   - CONTACT_OPEN      -> Matter Off to EP5 bound devices (state-follow)
 *   - LONG_PRESS_START  -> Matter LevelControl Move (dim up/down)
 *   - LONG_PRESS_STOP   -> Matter LevelControl Stop
 *   - 6x click          -> mode toggle (Matter <-> OTA, universal)
 *   - hold 10 s         -> mode toggle (alternative, avoids binding load)
 */
typedef enum {
    INPUT_PUSHBUTTON = 0,    /* GPIO10 — System 55 pushbutton (active-high in production,
                           * active-low in BENCH_MODE with internal pull-up) */
    INPUT_TOUCH,          /* TTP223 on Add-on digital input (always active-high) */
    INPUT_DEVICE_BTN,     /* GPIO4 — onboard pair button (always active-low, pull-up) */
    INPUT_COUNT
} input_id_t;

#ifdef __cplusplus
}
#endif
