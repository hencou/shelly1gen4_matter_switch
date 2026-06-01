#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OTA module for the Shelly 1 Gen4 custom firmware.
 *
 * Flow:
 *   1) During normal operation: WiFi OFF, only Thread/Matter.
 *   2) User trigger (6x clicks on pushbutton, or remote trigger) ->
 *      ota_request_at_next_boot() sets flag in NVS and reboots.
 *   3) At boot: ota_handle_pending() inspects the flag.
 *      - If saved WiFi creds exist -> direct STA OTA.
 *      - Otherwise -> SoftAP "shelly-ota-XXXXXX" with HTTP form on
 *        http://192.168.4.1/ to enter SSID/pass/URL once.
 *   4) On success: esp_restart() -> new firmware boots, Thread resumes.
 *   5) On failure: ESP-IDF rollback does not mark new app as valid;
 *      after 3rd failed boot it reverts to the previous slot.
 */

/* Call early in app_main, before the Matter stack or large components. */
void ota_handle_pending(void);

/* Set OTA flag in NVS and reboot the device. */
void ota_request_at_next_boot(void);

/* Save WiFi creds + URL in NVS (can also be done via web form). */
esp_err_t ota_save_credentials(const char *ssid, const char *password,
                               const char *firmware_url);

/* Mark current firmware as valid so ESP-IDF does not roll back.
 * Call after successful boot + Thread/Matter join. */
void ota_mark_app_valid(void);

#ifdef __cplusplus
}
#endif
