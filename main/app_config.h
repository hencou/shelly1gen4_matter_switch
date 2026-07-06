#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware version string — displayed on the management dashboard */
#ifndef FW_VERSION
#define FW_VERSION "1.5.3"
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
 * Bench mode ON:
 *   - GPIO10 gets internal pull-up (idle state high = released)
 *   - sensors_init() skips temp_task and occ_task (keeps GPIO9/16/17 free)
 * Bench mode OFF (production): original paths, external pull on GPIO10
 * driven by 230V optocoupler is then correct.
 *
 * BENCH_MODE compile-time default (0=production, 1=bench). Can be
 * overridden at runtime via the Management web page (stored in NVS).
 * Build: idf.py build -DBENCH_MODE=1  (or change this default)
 */
#ifndef BENCH_MODE
#define BENCH_MODE 0
#endif

/* Runtime bench mode flag — set by bench_mode_init() from NVS (with
 * BENCH_MODE compile-time default as fallback). Use this instead of
 * the BENCH_MODE macro in runtime code paths. */
extern int g_bench_mode;
void bench_mode_init(void);

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
/* 6x click = WiFi enable (universal on all 3 inputs):
 *   - Matter mode + 6x  -> enable WiFi alongside Thread (non-persistent)
 *   - OTA mode    + 6x  -> ignored
 * WiFi allows configuration via management dashboard while Thread
 * stays active. WiFi is lost on reboot (only in RAM). */
#define MODE_TOGGLE_CLICKS      6
#define MODE_TOGGLE_WINDOW_MS   2500
/* Double-click detection: after a short press release, wait this long
 * for a second press before dispatching SHORT_PRESS. */
#define DOUBLE_CLICK_WINDOW_MS  400
/* Default color temperature (2700 K = ~370 mireds) sent on double-press. */
#define DEFAULT_COLOR_TEMP_MIREDS  370

/* Matter endpoints */
#define EP_SWITCH_PUSHBUTTON   1
#define EP_SWITCH_STATE     2
#define EP_TEMPERATURE      3
#define EP_OCCUPANCY        4
#define EP_RELAY            5

/* Logical input source identifiers.
 * All 3 inputs have uniform behavior (see on_button_event in app_main.cpp):
 *   - SHORT_PRESS       -> Matter Toggle to EP1 bound devices (momentary)
 *   - DOUBLE_PRESS      -> Matter ColorControl MoveToColorTemperature (2700K default)
 *   - LONG_PRESS_START  -> Matter LevelControl MoveWithOnOff (dim, turns on lamp if off)
 *   - LONG_PRESS_STOP   -> Matter LevelControl Stop
 *   - SHORT_LONG_START  -> Matter ColorControl MoveColorTemperature (warm/cool)
 *   - SHORT_LONG_STOP   -> Matter ColorControl StopMoveStep
 *   - CONTACT_CLOSED    -> Matter On to EP2 bound devices (state-follow)
 *   - CONTACT_OPEN      -> Matter Off to EP2 bound devices (state-follow)
 *   - 6x click          -> enable WiFi alongside Thread (non-persistent)
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
