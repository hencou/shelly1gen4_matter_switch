/*
 * ADE7953 dual-channel energy metering driver (Shelly Plus 2PM Gen4).
 *
 * The 2PM carries an ADE7953 measuring two channels (A = relay 1, B = relay 2)
 * over I2C (SDA=GPIO6, SCL=GPIO7, IRQ=GPIO19 per the ESPHome device config).
 * The driver polls the chip periodically and reports voltage (shared mains),
 * per-channel current, active power, accumulated energy and line frequency.
 *
 * Reuses power_meter_reading_t so the Matter/dashboard layer is meter-agnostic.
 * Only start this on an ADE7953 profile (hw_profile()->pm_type == PM_ADE7953).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "power_meter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Invoked on every poll with both channels (ch_a = channel A, ch_b = B). */
typedef void (*ade7953_cb_t)(const power_meter_reading_t *ch_a,
                             const power_meter_reading_t *ch_b);

/* Start the ADE7953 driver on the given I2C pins (irq may be -1 = unused). */
esp_err_t ade7953_init(int sda, int scl, int irq, ade7953_cb_t cb);

/* Copy the latest reading for a channel (0 = A, 1 = B). Returns false when no
 * valid reading exists yet or the channel is out of range. */
bool ade7953_get(int ch, power_meter_reading_t *out);

#ifdef __cplusplus
}
#endif
