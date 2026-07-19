#include "chip_temp.h"

#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static temperature_sensor_handle_t s_tsens = NULL;
static SemaphoreHandle_t s_lock = NULL;

bool chip_temp_read(float *out)
{
    if (!out) return false;

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool ok = false;
    if (!s_tsens) {
        /* Use a range that fits a single hardware measurement range (the C6
         * sensor rejects a span wider than one range). -10..80 covers normal
         * SoC operating temperatures with the best accuracy. */
        temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&tc, &s_tsens) == ESP_OK) {
            temperature_sensor_enable(s_tsens);
        } else {
            s_tsens = NULL;
        }
    }
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, out) == ESP_OK) {
        ok = true;
    }

    xSemaphoreGive(s_lock);
    return ok;
}
