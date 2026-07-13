/*
 * BL0942 power meter driver (Shelly 1PM Gen4).
 *
 * The 1PM variant carries a BL0942 metering IC connected to the ESP32-C6 over
 * UART1 (TX=GPIO6, RX=GPIO7 per the verified 1PM Gen4 pinout). The driver polls
 * the chip periodically for a full measurement packet and exposes voltage,
 * current, active power, accumulated energy and line frequency.
 *
 * Only start this on a PM-capable device (hw_profile()->has_pm). On all other
 * profiles the driver is never initialised and UART1 stays free.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float voltage_v;      /* RMS voltage (V)            */
    float current_a;      /* RMS current (A)            */
    float power_w;        /* active power (W, signed)   */
    float energy_wh;      /* accumulated energy (Wh)    */
    float frequency_hz;   /* line frequency (Hz)        */
    bool  valid;          /* true once a valid frame was decoded */
} power_meter_reading_t;

typedef void (*power_meter_cb_t)(const power_meter_reading_t *r);

/* Start the BL0942 driver on UART1 with the given TX/RX GPIOs and a periodic
 * read task. cb (may be NULL) is invoked on every valid reading. */
esp_err_t power_meter_init(int uart_tx, int uart_rx, power_meter_cb_t cb);

/* Copy the latest reading for diagnostics. Returns false if no valid frame
 * has been decoded yet (or the driver was never started). */
bool power_meter_get(power_meter_reading_t *out);

#ifdef __cplusplus
}
#endif
