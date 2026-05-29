/*
 * Sensor-taken:
 *   1) DS18B20 op 1-Wire (PIN_ONEWIRE): bitbang 1-Wire master, elke
 *      TEMP_REPORT_INT_S seconden een conversie + ReadScratchpad.
 *      Rapporteert centi-graden Celsius (ZCL Temperature Measurement format).
 *   2) HLK-LD2410 OT2 pin (PIN_OCCUPANCY): GPIO-interrupt, debounced,
 *      rapporteert occupied=true/false.
 *
 * De 1-Wire-driver hier is een minimale implementatie die één DS18B20 op
 * de bus verwacht. Voor multi-drop bussen kan je ESP-IDF's driver/onewire
 * gebruiken via de component manager.
 */

#include "sensors.h"
#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "sensors";

/* ========================== 1-Wire / DS18B20 ========================== */

#define OW_PIN  PIN_ONEWIRE

static inline void ow_low(void)  { gpio_set_direction(OW_PIN, GPIO_MODE_OUTPUT); gpio_set_level(OW_PIN, 0); }
static inline void ow_hi(void)   { gpio_set_direction(OW_PIN, GPIO_MODE_INPUT);  }
static inline int  ow_read(void) { return gpio_get_level(OW_PIN); }

static bool ow_reset(void)
{
    ow_low();
    esp_rom_delay_us(480);
    ow_hi();
    esp_rom_delay_us(70);
    bool present = !ow_read();
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int b)
{
    ow_low();
    if (b) {
        esp_rom_delay_us(6);
        ow_hi();
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        ow_hi();
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    ow_low();
    esp_rom_delay_us(6);
    ow_hi();
    esp_rom_delay_us(9);
    int v = ow_read();
    esp_rom_delay_us(55);
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
    /* configure 1-Wire pin with pull-up (external 4.7k recommended) */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << OW_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

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

#if BENCH_MODE
    ESP_LOGW(TAG, "BENCH_MODE: DS18B20 temp_task overgeslagen (geen sensor op 1-Wire)");
#else
    xTaskCreate(temp_task, "temp_task", 3072, NULL, 5, NULL);
#endif

    s_occ_cb  = occ_cb;
    s_occ_q = xQueueCreate(8, sizeof(int64_t));
    xTaskCreate(occ_task,  "occ_task",  2560, NULL, 6, NULL);
}
