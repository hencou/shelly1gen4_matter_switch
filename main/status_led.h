#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_OFF = 0,         /* permanent uit */
    STATUS_LED_ON,              /* permanent aan */
    STATUS_LED_SLOW_BLINK,      /* 1 Hz, 50% duty — pairing/commissioning */
    STATUS_LED_FAST_BLINK,      /* 5 Hz, 50% duty — OTA / werk-in-uitvoering */
    STATUS_LED_HEARTBEAT,       /* korte flash elke 2 s — idle/online */
} status_led_pattern_t;

/*
 * Init de status-LED op de geconfigureerde GPIO (PIN_STATUS_LED).
 * Als de pin negatief is, of init faalt, dan no-op (alle set_pattern calls
 * worden silently genegeerd).
 */
esp_err_t status_led_init(void);

/* Wissel actieve patroon. Direct effect. */
void status_led_set(status_led_pattern_t pattern);

/* Een eenmalige korte puls (~50 ms aan) — overlapt huidig patroon kort. */
void status_led_blip(void);

#ifdef __cplusplus
}
#endif
