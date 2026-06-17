#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_OFF = 0,         /* permanently off */
    STATUS_LED_ON,              /* permanently on */
    STATUS_LED_SLOW_BLINK,      /* 1 Hz, 50% duty — pairing/commissioning */
    STATUS_LED_FAST_BLINK,      /* 5 Hz, 50% duty — OTA / work in progress */
    STATUS_LED_HEARTBEAT,       /* short flash every 2 s — idle/online */
} status_led_pattern_t;

/*
 * Initialize the status LED on the configured GPIO (PIN_STATUS_LED).
 * If the pin is negative or init fails, this is a no-op (all set_pattern
 * calls are silently ignored).
 */
esp_err_t status_led_init(void);

/* Switch active pattern. Immediate effect. */
void status_led_set(status_led_pattern_t pattern);

/* A one-shot short pulse (~50 ms on) — briefly overlaps current pattern. */
void status_led_blip(void);

#ifdef __cplusplus
}
#endif
