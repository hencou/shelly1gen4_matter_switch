/*
 * Shelly 1 Gen4 — Matter Switch firmware
 *
 * Entrypoint. Hergebruikt button/relay/sensors/ota modules uit het
 * Zigbee-project; alleen het Matter-stack-deel verschilt.
 */

extern "C" {
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "button.h"
#include "relay.h"
#include "sensors.h"
#include "ota.h"
#include "status_led.h"
}

#include "matter_device.h"

static const char *TAG = "app";

/* Alterneer dim-richting per long-press (1 gedeelde state voor alle inputs
 * omdat ze allemaal dezelfde Matter endpoint / binding gebruiken). */
static bool s_dim_up = true;

extern "C" void on_button_event(input_id_t id, button_event_t evt)
{
    /* Alle 3 inputs sturen via dezelfde Matter endpoint (= drukker EP).
     * Daardoor is er maar één binding nodig in HA voor alle drie de inputs. */
    uint16_t ep = matter_ep_drukker();
    ESP_LOGI(TAG, "button id=%d evt=%d ep=%u", id, evt, ep);

    switch (evt) {
    case BTN_EVT_SHORT_PRESS:
        matter_send_onoff_toggle(ep);
        relay_toggle();              /* altijd lokaal relais tikken */
        status_led_blip();
        break;

    case BTN_EVT_LONG_PRESS_START:
        matter_send_level_move(ep, s_dim_up, 50);
        s_dim_up = !s_dim_up;
        break;

    case BTN_EVT_LONG_PRESS_STOP:
        matter_send_level_stop(ep);
        break;

    case BTN_EVT_MODE_TOGGLE:
        /* In Matter mode (we draaien hier dus = Matter actief) betekent 6x
         * klik: ga naar OTA mode. ota_request_at_next_boot() zet de NVS-vlag
         * en reboot. Bij volgende boot kiest ota_handle_pending() de OTA-path.
         * (In OTA mode draait deze handler NIET; daar zit een eigen counter
         * in ota_handle_pending() die 6x klik als factory reset behandelt.) */
        ESP_LOGW(TAG, "MODE_TOGGLE from input %d -> requesting OTA mode", id);
        ota_request_at_next_boot();
        break;
    }
}

extern "C" void on_temperature(int16_t centi_c)
{
    matter_update_temperature(centi_c);
}

extern "C" void on_occupancy(bool occupied)
{
    matter_update_occupancy(occupied);
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    status_led_init();
    status_led_set(STATUS_LED_FAST_BLINK);  /* boot/init in uitvoering */

    /* WiFi-OTA pad heeft voorrang: bij vlag wordt Matter NIET gestart */
    ota_handle_pending();

    relay_init();

    /* Matter MOET voor button_driver_init / sensors_init starten:
     * die installeren GPIO-ISRs en FreeRTOS-taken die meteen via callbacks
     * Matter API's aanroepen. Als de Matter-stack en endpoints nog niet
     * geinitialiseerd zijn (s_ep_drukker = 0) crasht switch_send(). Op de
     * bench drijft GPIO10 (geen 230V AC = geen externe pull-referentie) ->
     * ANYEDGE-ISR vuurt direct na arming -> race -> SW_CPU reset. */
    ESP_ERROR_CHECK(matter_start());
    ESP_LOGI(TAG, "BOOT-STEP: matter_start() done, calling button_driver_init");

    button_driver_init(on_button_event);
    ESP_LOGI(TAG, "BOOT-STEP: button_driver_init done, calling sensors_init");

    sensors_init(on_temperature, on_occupancy);
    ESP_LOGI(TAG, "BOOT-STEP: sensors_init done");

    /* TODO: schakel naar STATUS_LED_HEARTBEAT zodra commissioning klaar is.
     * Voor nu: bij eerste boot na flash staat de BLE-pairing-window open,
     * dus slow-blink is een duidelijke "pair mij" indicator. */
    status_led_set(STATUS_LED_SLOW_BLINK);
    ESP_LOGI(TAG, "BOOT-STEP: status_led -> SLOW_BLINK");

    /* Markeer huidige image als valid zodat bootloader-rollback niet
     * triggert wanneer we vanaf een verse OTA opstarten. */
    ota_mark_app_valid();
    ESP_LOGI(TAG, "BOOT-STEP: ota_mark_app_valid done");

    ESP_LOGI(TAG, "Shelly 1 Gen4 Matter Switch running");
}
