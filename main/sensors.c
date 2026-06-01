/*
 * Sensor-taken:
 *   1) DS18B20 via dual-pin 1-Wire (PIN_ONEWIRE_TX + PIN_ONEWIRE_RX):
 *      De Shelly Plus Add-on gebruikt een ISO7221A galvanische isolator die
 *      het bidirectionele 1-Wire protocol opsplitst in aparte TX (output) en
 *      RX (input) lijnen. TX = GPIO9 (data out), RX = GPIO16 (data in).
 *      Elke TEMP_REPORT_INT_S seconden een conversie + ReadScratchpad.
 *      Rapporteert centi-graden Celsius (ZCL Temperature Measurement format).
 *   2) HLK-LD2410 OT2 pin (PIN_LD2410_INPUT): GPIO-interrupt, debounced,
 *      rapporteert occupied=true/false.
 */

#include "sensors.h"
#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "sensors";

/* ========================== Dual-pin 1-Wire / DS18B20 ========================== */
/* De Shelly Plus Add-on gebruikt een ISO7221A dual digital isolator.
 * TX pin (GPIO9)  = output: ESP32 stuurt commando's naar de DS18B20
 * RX pin (GPIO16) = input:  ESP32 leest antwoorden van de DS18B20
 * Standaard 1-Wire (single-pin) werkt niet door de galvanische isolatie. */

#define OW_TX  PIN_ONEWIRE_TX
#define OW_RX  PIN_ONEWIRE_RX

static inline void ow_tx_low(void)  { gpio_set_level(OW_TX, 0); }
static inline void ow_tx_high(void) { gpio_set_level(OW_TX, 1); }
static inline int  ow_rx_read(void) { return gpio_get_level(OW_RX); }

static bool ow_reset(void)
{
    /* Wacht tot bus idle (RX=HIGH) */
    uint8_t retries = 125;
    do {
        if (--retries == 0) return false;
        esp_rom_delay_us(2);
    } while (!ow_rx_read());

    /* 480 µs reset-puls via TX */
    ow_tx_low();
    esp_rom_delay_us(480);
    ow_tx_high();
    esp_rom_delay_us(70);
    bool present = !ow_rx_read();
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int b)
{
    ow_tx_low();
    if (b) {
        esp_rom_delay_us(10);
        ow_tx_high();
        esp_rom_delay_us(55);
    } else {
        esp_rom_delay_us(65);
        ow_tx_high();
        esp_rom_delay_us(5);
    }
}

static int ow_read_bit(void)
{
    ow_tx_low();
    esp_rom_delay_us(3);
    ow_tx_high();
    esp_rom_delay_us(9);
    int v = ow_rx_read();
    esp_rom_delay_us(53);
    return v;
}

static void ow_write_byte(uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(b & 1);
        b >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (ow_read_bit() << i);
    }
    return v;
}

static bool ds18b20_read_centi_c(int16_t *out)
{
    if (!ow_reset()) {
        return false;
    }
    ow_write_byte(0xCC);  /* Skip ROM */
    ow_write_byte(0x44);  /* Convert T */
    vTaskDelay(pdMS_TO_TICKS(800));  /* 12-bit max conversion */

    if (!ow_reset()) {
        return false;
    }
    ow_write_byte(0xCC);
    ow_write_byte(0xBE);  /* Read Scratchpad */

    uint8_t lsb = ow_read_byte();
    uint8_t msb = ow_read_byte();
    /* skip remaining bytes */
    for (int i = 0; i < 7; i++) (void)ow_read_byte();

    int16_t raw = (int16_t)((msb << 8) | lsb);
    /* raw is in 1/16 °C. Convert to centi-°C:  raw * 100 / 16 */
    *out = (int16_t)(((int32_t)raw * 100) / 16);
    return true;
}

static temp_cb_t s_temp_cb;
static void temp_task(void *arg)
{
    /* GPIO16 is standaard UART0 TX op de ESP32-C6. Verwijder de UART0
     * driver zodat de peripheral GPIO16 loslaat voordat we hem als
     * 1-Wire RX pin herconfigureren. Zonder deze stap blijft UART0 de pin
     * bezet en reageert de DS18B20 niet. Na uart_driver_delete is J6
     * TXD niet meer bruikbaar — dat is acceptabel in productie (BENCH_MODE=0). */
    uart_driver_delete(UART_NUM_0);

    /* TX pin: output, idle high */
    gpio_config_t tx_cfg = {
        .pin_bit_mask = (1ULL << OW_TX),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&tx_cfg);
    ow_tx_high();

    /* RX pin: input (Add-on heeft eigen 4.7kΩ pull-up via isolator) */
    gpio_reset_pin(OW_RX);
    gpio_config_t rx_cfg = {
        .pin_bit_mask = (1ULL << OW_RX),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&rx_cfg);

    while (1) {
        int16_t centi = 0;
        if (ds18b20_read_centi_c(&centi)) {
            if (s_temp_cb) s_temp_cb(centi);
            ESP_LOGI(TAG, "temp = %d.%02d °C", centi / 100, abs(centi % 100));
        } else {
            ESP_LOGW(TAG, "DS18B20 read failed");
        }
        vTaskDelay(pdMS_TO_TICKS(TEMP_REPORT_INT_S * 1000));
    }
}

/* ========================== Occupancy / LD2410 ========================== */

static QueueHandle_t    s_occ_q;
static occupancy_cb_t   s_occ_cb;

static void IRAM_ATTR occ_isr(void *arg)
{
    int64_t t = esp_timer_get_time();
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_occ_q, &t, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

static void occ_task(void *arg)
{
    /* GPIO17 is standaard UART0 RX op de ESP32-C6 — de UART driver zet een
     * interne pull-up. gpio_reset_pin() disconnecteert de UART peripheral
     * en reset alle pulls, zodat onze gpio_config met pull_down pakt. */
    gpio_reset_pin(PIN_LD2410_INPUT);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LD2410_INPUT),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&cfg);
    gpio_isr_handler_add(PIN_LD2410_INPUT, occ_isr, NULL);

    int64_t last = 0;
    int last_level = -1;
    for (;;) {
        int64_t t;
        if (xQueueReceive(s_occ_q, &t, portMAX_DELAY) != pdTRUE) continue;
        if ((t - last) / 1000 < OCC_DEBOUNCE_MS) continue;
        last = t;
        int level = gpio_get_level(PIN_LD2410_INPUT);
        if (level != last_level) {
            last_level = level;
            if (s_occ_cb) s_occ_cb(level == 1);
            ESP_LOGI(TAG, "occupancy = %s", level ? "occupied" : "clear");
        }
    }
}

/* ========================== init ========================== */

void sensors_init(temp_cb_t temp_cb, occupancy_cb_t occ_cb)
{
    s_temp_cb = temp_cb;
    s_occ_cb  = occ_cb;

#if BENCH_MODE
    /* BENCH_MODE: skip sensor-tasks zodat GPIO16 (U0TXD) en GPIO17 (U0RXD)
     * beschikbaar blijven voor UART0 serial debugging via J6 header.
     * Op de ESP32-C6 zijn GPIO16/17 de standaard UART0-pins; temp_task en
     * occ_task herconfigureren ze als 1-Wire RX resp. GPIO-input, wat de
     * seriële output doodt. GPIO9 (1-Wire TX) wordt ook vrijgehouden. */
    ESP_LOGW(TAG, "BENCH_MODE: sensor-tasks overgeslagen (GPIO9/16/17 vrijgehouden)");
#else
    xTaskCreate(temp_task, "temp_task", 3072, NULL, 5, NULL);
    s_occ_q = xQueueCreate(8, sizeof(int64_t));
    xTaskCreate(occ_task,  "occ_task",  2560, NULL, 6, NULL);
#endif
}
