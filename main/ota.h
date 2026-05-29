#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OTA-module voor de Shelly 1 Gen4 custom firmware.
 *
 * Stroom:
 *   1) Tijdens normaal bedrijf: WiFi UIT, alleen Zigbee.
 *   2) User-trigger (10x klikken op drukker, of remote ZCL-trigger) ->
 *      ota_request_at_next_boot() zet vlag in NVS en reboot.
 *   3) Bij boot: ota_handle_pending() inspecteert vlag.
 *      - Als opgeslagen WiFi-creds bestaan -> directe STA OTA.
 *      - Anders -> SoftAP "shelly-ota-XXXXXX" met HTTP-form op
 *        http://192.168.4.1/ om SSID/pass/URL eenmalig in te voeren.
 *   4) Na succes: esp_restart() -> nieuwe firmware boot, Zigbee herneemt.
 *   5) Bij failure: ESP-IDF rollback markeert nieuwe app niet als valid;
 *      bij 3e mislukte boot keert het terug naar vorige slot.
 */

/* Roep aan vroeg in app_main, vóór Zigbee-stack of grote componenten. */
void ota_handle_pending(void);

/* Zet OTA-vlag in NVS en reboot het apparaat. */
void ota_request_at_next_boot(void);

/* Sla WiFi-creds + URL op in NVS (kan ook via web-form). */
esp_err_t ota_save_credentials(const char *ssid, const char *password,
                               const char *firmware_url);

/* Markeer huidige firmware als valid zodat ESP-IDF geen rollback doet.
 * Roep aan na succesvolle boot + Zigbee join. */
void ota_mark_app_valid(void);

#ifdef __cplusplus
}
#endif
