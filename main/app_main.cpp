/*
 * Shelly 1 Gen4 — Matter Switch firmware
 *
 * Entrypoint. Reuses button/relay/sensors/ota modules from the
 * Zigbee project; only the Matter stack part differs.
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
#include <credentials/GroupDataProviderImpl.h>

static const char *TAG = "app";

/* Alternate dim direction per long-press (1 shared state for all inputs
 * because they all use the same Matter endpoint / binding). */
static bool s_dim_up = true;

extern "C" void on_button_event(input_id_t id, button_event_t evt)
{
    /* EP1 = toggle (momentary pushbutton), EP2 = state-follow (maintained switch).
     * All 3 inputs send to both endpoints; the user chooses via
     * binding which endpoint controls their light/relay. */
    uint16_t ep = matter_ep_pushbutton();
    uint16_t ep_sf = matter_ep_state();
    ESP_LOGI(TAG, "button id=%d evt=%d ep=%u ep_sf=%u", id, evt, ep, ep_sf);

    switch (evt) {
    case BTN_EVT_SHORT_PRESS:
        matter_send_onoff_toggle(ep);
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
        /* In Matter mode (we are running here so = Matter active) 6x click
         * means: switch to OTA mode. ota_request_at_next_boot() sets the NVS
         * flag and reboots. On next boot ota_handle_pending() picks the OTA
         * path. (In OTA mode this handler does NOT run; there is a separate
         * counter in ota_handle_pending() that treats 6x click as factory reset.) */
        ESP_LOGW(TAG, "MODE_TOGGLE from input %d -> requesting OTA mode", id);
        ota_request_at_next_boot();
        break;

    case BTN_EVT_CONTACT_CLOSED:
        matter_send_onoff_on(ep_sf);
        break;

    case BTN_EVT_CONTACT_OPEN:
        matter_send_onoff_off(ep_sf);
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
    status_led_set(STATUS_LED_FAST_BLINK);  /* boot/init in progress */

    /* WiFi OTA path takes priority: when flag is set, Matter is NOT started */
    ota_handle_pending();

    relay_init();

    /* Matter MUST start before button_driver_init / sensors_init:
     * those install GPIO ISRs and FreeRTOS tasks that immediately call
     * Matter APIs via callbacks. If the Matter stack and endpoints are not
     * yet initialized (s_ep_pushbutton = 0) switch_send() crashes. On the
     * bench GPIO10 floats (no 230V AC = no external pull reference) ->
     * ANYEDGE ISR fires immediately after arming -> race -> SW_CPU reset. */
    ESP_ERROR_CHECK(matter_start());
    ESP_LOGI(TAG, "BOOT-STEP: matter_start() done, calling button_driver_init");

// =========================================================================
    // MULTICAST FIX: Bind GroupId 0x0001 to the default KeySetId
    // =========================================================================
    ESP_LOGI(TAG, "Configuring local Matter Group Key Mapping...");
    chip::Credentials::GroupDataProvider *provider = chip::Credentials::GetGroupDataProvider();
    if (provider != nullptr) {
        // Fabric index 1 is the default primary fabric (Home Assistant)
        chip::FabricIndex fabricIdx = 1; 
        
        // Map Group 0x0001 to KeySet 0x0001 (default epoch keyset)
        chip::Credentials::GroupDataProvider::GroupKey mapping;
        mapping.mGroupId = 0x0001;
        mapping.mKeySetId = 0x0001;
        
        if (provider->SetGroupKeyAt(fabricIdx, 0, mapping) == CHIP_NO_ERROR) {
            ESP_LOGI(TAG, "Group 0x0001 successfully mapped to KeySet 0x0001");
        } else {
            ESP_LOGE(TAG, "FAILED to write group key mapping to provider");
        }
    } else {
        ESP_LOGE(TAG, "ERROR: Group Data Provider is null");
    }
    // =========================================================================
    
    button_driver_init(on_button_event);
    ESP_LOGI(TAG, "BOOT-STEP: button_driver_init done, calling sensors_init");

    sensors_init(on_temperature, on_occupancy);
    ESP_LOGI(TAG, "BOOT-STEP: sensors_init done");

    /* TODO: switch to STATUS_LED_HEARTBEAT once commissioning is complete.
     * For now: on first boot after flash the BLE pairing window is open,
     * so slow-blink is a clear "pair me" indicator. */
    status_led_set(STATUS_LED_SLOW_BLINK);
    ESP_LOGI(TAG, "BOOT-STEP: status_led -> SLOW_BLINK");

    /* Mark current image as valid so bootloader rollback does not
     * trigger when booting from a fresh OTA. */
    ota_mark_app_valid();
    ESP_LOGI(TAG, "BOOT-STEP: ota_mark_app_valid done");

    ESP_LOGI(TAG, "Shelly 1 Gen4 Matter Switch running");
}
