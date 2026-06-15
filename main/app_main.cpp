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
#include <platform/ConnectivityManager.h>

static const char *TAG = "app";

extern "C" void on_button_event(input_id_t id, button_event_t evt)
{
    ESP_LOGI(TAG, "button id=%d evt=%d", id, evt);

    if (evt == BTN_EVT_MODE_TOGGLE) {
        if (ota_wifi_persistent_get()) {
            /* WiFi is already active — just open management dashboard.
             * No need to disable Thread (coexistence handles both). */
            ESP_LOGW(TAG, "MODE_TOGGLE from input %d -> WiFi already persistent, opening dashboard", id);
            ota_enable_wifi_runtime();
        } else {
            ESP_LOGW(TAG, "MODE_TOGGLE from input %d -> disabling Thread, enabling WiFi", id);
            matter_disable_thread();
            ota_enable_wifi_runtime();
        }
        return;
    }

    /* All button behavior is handled by Lua scripts */
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

    /* Load script slot types from NVS BEFORE matter_start —
     * endpoints are created dynamically based on slot configuration. */
    script_slot_type_t slot_types[SCRIPT_MAX_SLOTS];
    script_engine_load_slot_types(slot_types, SCRIPT_MAX_SLOTS);

    /* WiFi persistent + TBR: prepare netifs BEFORE matter_start() (TBR backbone
     * must be set before OpenThread starts), but only ACTIVATE after we confirm
     * the device is commissioned.  This prevents WiFi/Thread coexistence from
     * interfering with BLE commissioning. */
    bool wifi_persistent = ota_wifi_persistent_get();
    bool tbr_mode = ota_tbr_mode_get();

    if (wifi_persistent) {
        /* Create netifs before matter_start() — TBR backbone must be set before
         * OpenThread starts.  Actual WiFi connection is deferred until we confirm
         * the device is commissioned. */
        ESP_LOGI(TAG, "BOOT-STEP: WiFi persistent — creating netifs early (activation deferred)");
        ota_wifi_ensure_netifs();

        if (tbr_mode) {
            esp_netif_t *sta = ota_get_wifi_sta_netif();
            if (sta) {
                matter_set_tbr_backbone(sta);
                ESP_LOGI(TAG, "BOOT-STEP: TBR backbone netif set");
            }
        }
    }

    /* Matter MUST start before button_driver_init / sensors_init:
     * those install GPIO ISRs and FreeRTOS tasks that immediately call
     * Matter APIs via callbacks. */
    ESP_ERROR_CHECK(matter_start(slot_types, SCRIPT_MAX_SLOTS));
    ESP_LOGI(TAG, "BOOT-STEP: matter_start() done");

    bool commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;

    /* WiFi persistent + TBR only activate when commissioned (fabric present).
     * Before commissioning the radio must be free for BLE. */
    if (wifi_persistent && commissioned) {
        if (tbr_mode) {
            matter_tbr_init();
            ESP_LOGI(TAG, "BOOT-STEP: Thread Border Router initialized");
        }
        ota_enable_wifi_runtime();
        ESP_LOGI(TAG, "BOOT-STEP: WiFi runtime started (persistent, commissioned)");
    } else if (wifi_persistent && !commissioned) {
        ESP_LOGW(TAG, "BOOT-STEP: WiFi persistent ON but not commissioned — deferred until after commissioning");
    }

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

    if (commissioned) {
        status_led_set(STATUS_LED_HEARTBEAT);
        ESP_LOGI(TAG, "BOOT-STEP: status_led -> HEARTBEAT (commissioned)");
    } else {
        status_led_set(STATUS_LED_SLOW_BLINK);
        ESP_LOGI(TAG, "BOOT-STEP: status_led -> SLOW_BLINK (not commissioned)");
    }

    /* Mark current image as valid so bootloader rollback does not
     * trigger when booting from a fresh OTA. */
    ota_mark_app_valid();
    ESP_LOGI(TAG, "BOOT-STEP: ota_mark_app_valid done");

    /* Smart boot: decide between WiFi-setup mode and BLE-commissioning mode.
     *
     * Not commissioned + no scripts → WiFi setup mode:
     *   User needs the management dashboard to configure endpoints/scripts.
     *   Disable BLE advertising (radio conflict) and start WiFi.
     *
     * Not commissioned + scripts configured → BLE commissioning mode:
     *   User has set up endpoints via the dashboard and rebooted.
     *   Let BLE advertising run so the phone can discover and commission.
     *
     * Commissioned → normal operation:
     *   WiFi persistent/TBR already started above; otherwise 6× press. */
    if (!commissioned) {
        bool has_slots = false;
        for (int i = 0; i < SCRIPT_MAX_SLOTS; i++) {
            if (slot_types[i] != SLOT_TYPE_NONE) { has_slots = true; break; }
        }
        if (!has_slots) {
            ESP_LOGI(TAG, "Not commissioned, no scripts — WiFi setup mode (BLE off)");
            chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false);
            ota_enable_wifi_runtime();
        } else {
            ESP_LOGI(TAG, "Not commissioned, scripts configured — BLE commissioning mode");
        }
    }

    ESP_LOGI(TAG, "Shelly 1 Gen4 Matter Switch running");
}
