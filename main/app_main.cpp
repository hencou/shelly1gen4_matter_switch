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
#include <string.h>

#include "app_config.h"
#include "button.h"
#include "relay.h"
#include "sensors.h"
#include "ota.h"
#include "status_led.h"
#include "script_engine.h"
}

#include "matter_device.h"
#include <app/server/Server.h>
#include <credentials/GroupDataProvider.h>
#include <lib/core/CHIPError.h>
#include <lib/core/DataModelTypes.h>
#include <lib/support/Span.h>

static const char *TAG = "app";

/* Alternate dim direction per long-press (1 shared state for all inputs
 * because they all use the same Matter endpoint / binding). */
static bool s_dim_up = true;
/* Alternate color-temperature direction per short-long gesture. */
static bool s_ct_warmer = true;

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

    case BTN_EVT_DOUBLE_PRESS:
        matter_send_color_temp_set(ep, DEFAULT_COLOR_TEMP_MIREDS);
        status_led_blip();
        status_led_blip();
        break;

    case BTN_EVT_LONG_PRESS_START:
        matter_send_level_move(ep, s_dim_up, 50);
        s_dim_up = !s_dim_up;
        break;

    case BTN_EVT_LONG_PRESS_STOP:
        matter_send_level_stop(ep);
        break;

    case BTN_EVT_SHORT_LONG_START:
        matter_send_color_temp_move(ep, s_ct_warmer, 50);
        s_ct_warmer = !s_ct_warmer;
        break;

    case BTN_EVT_SHORT_LONG_STOP:
        matter_send_color_temp_stop(ep);
        break;

    case BTN_EVT_MODE_TOGGLE:
        /* 6x click in Matter mode: enable WiFi alongside Thread so the
         * management dashboard is reachable for configuration. WiFi is
         * non-persistent — after reboot only Thread remains active. */
        ESP_LOGW(TAG, "MODE_TOGGLE from input %d -> enabling WiFi runtime", id);
        ota_enable_wifi_runtime();
        break;

    case BTN_EVT_CONTACT_CLOSED:
        matter_send_onoff_on(ep_sf);
        break;

    case BTN_EVT_CONTACT_OPEN:
        matter_send_onoff_off(ep_sf);
        break;
    }

    /* Forward event to script engine for TRIGGER_BUTTON_EVENT scripts */
    script_engine_button_event(id, evt);
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
    bench_mode_init();

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
    // MULTICAST GROUP KEY — install KeySet 1 + GroupKeyMap
    // =========================================================================
    // The IPK (KeySet 0) cannot be used directly for group encryption on this
    // SDK version (returns CHIP_ERROR_INTERNAL).  We install our own KeySet 1
    // with a fixed epoch key directly via the GroupDataProvider API.
    // The same key must be installed on the lamps via the setup script.
    {
        using namespace chip::Credentials;
        GroupDataProvider *provider = GetGroupDataProvider();
        if (provider != nullptr) {
            // Shared 128-bit epoch key — must match the script's --epoch-key
            static const uint8_t kGroupEpochKey[16] = {
                0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
                0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf
            };

            for (const auto & fabricInfo : chip::Server::GetInstance().GetFabricTable()) {
                chip::FabricIndex idx = fabricInfo.GetFabricIndex();

                // GetCompressedFabricIdBytes fills a MutableByteSpan
                uint8_t cfid_buf[sizeof(uint64_t)];
                chip::MutableByteSpan cfid_span(cfid_buf);
                if (fabricInfo.GetCompressedFabricIdBytes(cfid_span) != CHIP_NO_ERROR) {
                    ESP_LOGW(TAG, "GroupKeySet 1: fabric %u cannot get CompressedFabricId", idx);
                    continue;
                }

                // Install KeySet 1 with our epoch key
                GroupDataProvider::KeySet keySet;
                keySet.keyset_id    = 1;
                keySet.policy       = GroupDataProvider::SecurityPolicy::kTrustFirst;
                keySet.num_keys_used = 1;
                keySet.epoch_keys[0].start_time = 1; // 1 µs = always valid
                memcpy(keySet.epoch_keys[0].key, kGroupEpochKey, 16);

                CHIP_ERROR err = provider->SetKeySet(idx, cfid_span, keySet);
                if (err == CHIP_NO_ERROR) {
                    ESP_LOGI(TAG, "GroupKeySet 1: fabric %u installed OK", idx);
                } else {
                    ESP_LOGW(TAG, "GroupKeySet 1: fabric %u FAILED %" CHIP_ERROR_FORMAT, idx, err.Format());
                }

                // GroupKeyMap (group → KeySet mapping) is NOT written here.
                // The setup script (create_matter_cluster_group.py) writes it
                // for the correct group ID.  Persisted in NVS — survives reboot.
            }
        } else {
            ESP_LOGE(TAG, "GroupKeyMap: GroupDataProvider is null");
        }
    }
    // =========================================================================
    
    /* Script engine — must init after Matter (needs endpoints), before buttons */
    script_engine_init();
    script_engine_start();
    ESP_LOGI(TAG, "BOOT-STEP: script_engine started");

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

    /* Auto-enable WiFi when device is not yet commissioned so the
     * management dashboard is immediately reachable for setup. */
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        ESP_LOGI(TAG, "Not commissioned — auto-enabling WiFi for setup");
        ota_enable_wifi_runtime();
    }

    ESP_LOGI(TAG, "Shelly 1 Gen4 Matter Switch running");
}
