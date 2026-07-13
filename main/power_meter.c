/*
 * BL0942 power meter driver — see power_meter.h.
 *
 * Protocol (polled mode): the host writes {READ|addr, FULL_PACKET} and the
 * BL0942 replies with a 23-byte packet starting with header 0x55. All multi-
 * byte registers are little-endian; WATT is signed. The checksum is the low
 * byte of ~(readcmd + packet[0..21]).
 *
 * Scaling constants below are the BL0942 reference-design defaults (1 mR shunt,
 * 390k x5 / 510R divider) as used by ESPHome/Tasmota. The Shelly 1PM Gen4 uses
 * a similar front-end but the exact shunt/divider must be verified: calibrate
 * these against a known load if the reported values are off by a fixed factor.
 */

#include "power_meter.h"

#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_meter";

/* ---- BL0942 protocol constants ---- */
#define BL0942_UART_PORT     UART_NUM_1
#define BL0942_BAUD          9600          /* Shelly 1PM Gen4 (per ESPHome device DB) */
#define BL0942_ADDR          0x00
#define BL0942_READ_CMD      0x58
#define BL0942_FULL_PACKET   0xAA
#define BL0942_PACKET_HEADER 0x55
#define BL0942_PACKET_LEN    23

/* ---- Reference-design scaling (calibrate on hardware if needed) ---- */
#define BL0942_UREF   15883.34116f   /* v_rms  -> volts   */
#define BL0942_IREF   251065.6814f   /* i_rms  -> amps    */
#define BL0942_PREF   623.0270705f   /* watt   -> watts   */
#define BL0942_EREF   5347.484240f   /* cf_cnt -> kWh     */

#define READ_INTERVAL_MS  2000

static power_meter_cb_t s_cb;
static power_meter_reading_t s_last;
static bool s_have_reading;

static double  s_energy_counts;   /* accumulated cf_cnt (wrap-corrected) */
static uint32_t s_prev_cf;
static bool    s_prev_cf_valid;

static inline uint32_t u24_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static inline int32_t i24_le(const uint8_t *p)
{
    uint32_t v = u24_le(p);
    if (v & 0x800000u) v |= 0xFF000000u;   /* sign-extend 24 -> 32 */
    return (int32_t)v;
}

static bool decode_packet(const uint8_t *b, power_meter_reading_t *out)
{
    if (b[0] != BL0942_PACKET_HEADER) {
        ESP_LOGW(TAG, "bad header 0x%02X", b[0]);
        return false;
    }

    uint8_t sum = (uint8_t)(BL0942_READ_CMD | BL0942_ADDR);
    for (int i = 0; i < BL0942_PACKET_LEN - 1; i++) sum += b[i];
    sum ^= 0xFF;
    if (sum != b[BL0942_PACKET_LEN - 1]) {
        ESP_LOGW(TAG, "checksum 0x%02X != 0x%02X", sum, b[BL0942_PACKET_LEN - 1]);
        return false;
    }

    uint32_t i_rms = u24_le(&b[1]);
    uint32_t v_rms = u24_le(&b[4]);
    int32_t  watt  = i24_le(&b[10]);
    uint32_t cf    = u24_le(&b[13]);
    uint16_t freq  = (uint16_t)(b[16] | (b[17] << 8));

    /* Accumulate energy from the 24-bit cf pulse counter (wraps at 2^24). */
    if (s_prev_cf_valid) {
        uint32_t delta = (cf - s_prev_cf) & 0xFFFFFFu;
        s_energy_counts += (double)delta;
    }
    s_prev_cf = cf;
    s_prev_cf_valid = true;

    out->voltage_v   = (float)v_rms / BL0942_UREF;
    out->current_a   = (float)i_rms / BL0942_IREF;
    out->power_w     = (float)watt  / BL0942_PREF;
    out->energy_wh   = (float)(s_energy_counts / BL0942_EREF * 1000.0);
    out->frequency_hz = freq ? (1000000.0f / (float)freq) : 0.0f;
    out->valid = true;
    return true;
}

static void pm_task(void *arg)
{
    uint8_t buf[BL0942_PACKET_LEN];
    const uint8_t req[2] = { (uint8_t)(BL0942_READ_CMD | BL0942_ADDR), BL0942_FULL_PACKET };

    while (1) {
        uart_flush_input(BL0942_UART_PORT);
        uart_write_bytes(BL0942_UART_PORT, (const char *)req, sizeof(req));

        int n = uart_read_bytes(BL0942_UART_PORT, buf, BL0942_PACKET_LEN,
                                pdMS_TO_TICKS(300));
        if (n == BL0942_PACKET_LEN) {
            power_meter_reading_t r = {0};
            if (decode_packet(buf, &r)) {
                s_last = r;
                s_have_reading = true;
                ESP_LOGI(TAG, "U=%.1fV I=%.3fA P=%.1fW E=%.1fWh f=%.2fHz",
                         r.voltage_v, r.current_a, r.power_w, r.energy_wh,
                         r.frequency_hz);
                if (s_cb) s_cb(&r);
            }
        } else if (n > 0) {
            ESP_LOGW(TAG, "short frame (%d bytes)", n);
        } else {
            ESP_LOGW(TAG, "no response from BL0942");
        }

        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}

esp_err_t power_meter_init(int uart_tx, int uart_rx, power_meter_cb_t cb)
{
    s_cb = cb;

    const uart_config_t cfg = {
        .baud_rate = BL0942_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(BL0942_UART_PORT, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(uart_param_config(BL0942_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BL0942_UART_PORT, uart_tx, uart_rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    BaseType_t r = xTaskCreate(pm_task, "pm_task", 3072, NULL, 5, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "pm_task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BL0942 init on UART1 TX=GPIO%d RX=GPIO%d @ %d baud",
             uart_tx, uart_rx, BL0942_BAUD);
    return ESP_OK;
}

bool power_meter_get(power_meter_reading_t *out)
{
    if (!s_have_reading || !out) return false;
    *out = s_last;
    return true;
}
