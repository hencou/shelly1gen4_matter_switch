#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Pin mapping — adjust via `idf.py menuconfig` */
#define PIN_RELAY           CONFIG_PIN_RELAY
#define PIN_SWITCH_INPUT    CONFIG_PIN_SWITCH_INPUT
#define PIN_ONEWIRE         CONFIG_PIN_ONEWIRE         /* Shelly Add-on data */

/* Bench-mode: tijdens 3V3-USB-UART-testen (zonder 230V op de drukker en
 * zonder DS18B20/LD2410 op de Add-on) hangen GPIO10, GPIO16 en GPIO17 floating.
 * GPIO10-ANYEDGE → ISR-storm; GPIO16/17 zijn ook de UART0 TX/RX pins op de
 * ESP32-C6 — als sensors.c ze herconfigureert sterft de serial output.
 * BENCH_MODE=1:
 *   - GPIO10 krijgt interne pull-up (rust-toestand high = released)
 *   - sensors_init() slaat temp_task en occ_task over (GPIO16/17 = UART0)
 * BENCH_MODE=0 (productie): originele paden, externe pull op GPIO10
 * gedreven door 230V-optocoupler is dan correct.
 *
 * Build: idf.py build -DBENCH_MODE=1  (of pas deze default aan)
 * NB: in productie (230V + Add-on) MOET dit 0 zijn!
 */
#ifndef BENCH_MODE
#define BENCH_MODE 0
#endif

/* Add-on inputs — TTP223 en LD2410 hebben elk een eigen GPIO, altijd actief. */
#define PIN_TOUCH_INPUT     CONFIG_PIN_TOUCH_INPUT      /* TTP223 capacitive touch (GPIO12) */
#define PIN_LD2410_INPUT    CONFIG_PIN_LD2410_INPUT      /* HLK-LD2410 occupancy   (GPIO17) */

#define PIN_STATUS_LED      CONFIG_PIN_STATUS_LED      /* Shelly Add-on LED, -1 = uit */

/* Kconfig bool: alleen gedefinieerd wanneer y; afwezig (=n) → 0. */
#ifdef CONFIG_STATUS_LED_ACTIVE_HIGH
#define STATUS_LED_ACTIVE_HIGH 1
#else
#define STATUS_LED_ACTIVE_HIGH 0
#endif

/* Timings */
#define LONG_PRESS_MS       CONFIG_LONG_PRESS_MS
#define OCC_DEBOUNCE_MS     CONFIG_OCC_DEBOUNCE_MS
#define TEMP_REPORT_INT_S   CONFIG_TEMP_REPORT_INTERVAL_S
/* 6x klik = mode toggle (universeel op alle 3 inputs):
 *   - Matter mode + 6x  -> reboot in OTA mode (dedicated)
 *   - OTA mode    + 6x  -> factory reset (wipe nvs + chip_kvs) -> Matter mode
 * Hufter-proof: zelfde gesture op zelfde knop voor beide richtingen.
 * Lang ingedrukt houden levert GEEN factory reset op (te risicovol bij
 * een wand-schakelaar die per ongeluk lang aan blijft staan). */
#define MODE_TOGGLE_CLICKS      6
#define MODE_TOGGLE_WINDOW_MS   2500

/* Matter endpoints */
#define EP_SWITCH_DRUKKER   1
#define EP_TEMPERATURE      2
#define EP_OCCUPANCY        3
#define EP_RELAY            4
#define EP_SWITCH_STATE     5

/* Logical input source identifiers.
 * Alle 3 inputs hebben uniform gedrag (zie on_button_event in app_main.cpp):
 *   - SHORT_PRESS       -> Matter Toggle naar EP1 bound apparaten (momentknop)
 *   - CONTACT_CLOSED    -> Matter On naar EP5 bound apparaten (state-follow)
 *   - CONTACT_OPEN      -> Matter Off naar EP5 bound apparaten (state-follow)
 *   - LONG_PRESS_START  -> Matter LevelControl Move (dim up/down)
 *   - LONG_PRESS_STOP   -> Matter LevelControl Stop
 *   - 6x klik           -> mode toggle (Matter <-> OTA, universeel)
 */
typedef enum {
    INPUT_DRUKKER = 0,    /* GPIO10 — System 55 impulsdrukker (active-high in productie,
                           * active-low in BENCH_MODE met interne pull-up) */
    INPUT_TOUCH,          /* TTP223 op Add-on digital input (altijd active-high) */
    INPUT_DEVICE_BTN,     /* GPIO4 — onboard pair-knop (altijd active-low, pull-up) */
    INPUT_COUNT
} input_id_t;

#ifdef __cplusplus
}
#endif
