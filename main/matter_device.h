#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Matter-stack starten met 5 endpoints:
 *   EP1 = OnOff Light Switch (drukker) + Binding cluster — Toggle
 *   EP2 = Temperature Sensor
 *   EP3 = Occupancy Sensor
 *   EP4 = OnOff Light (relais) — server, aanstuurbaar vanuit HA
 *   EP5 = OnOff Light Switch (state-follow) + Binding — On/Off volgt contact
 */
esp_err_t matter_start(void);

/* Commando-emit naar bound nodes/groups (via Binding cluster). */
void matter_send_onoff_toggle(uint16_t local_endpoint_id);
void matter_send_onoff_on(uint16_t local_endpoint_id);
void matter_send_onoff_off(uint16_t local_endpoint_id);
void matter_send_level_move(uint16_t local_endpoint_id, bool up, uint8_t rate);
void matter_send_level_stop(uint16_t local_endpoint_id);

/* Sensor-attribuut updates voor Matter-server-clusters. */
void matter_update_temperature(int16_t centi_c);
void matter_update_occupancy(bool occupied);

/* Factory reset → wist Matter NVS, leaves de fabric, reboot. */
void matter_factory_reset(void);

/* Endpoint-ID lookups voor app_main.c (na matter_start). */
uint16_t matter_ep_drukker(void);
uint16_t matter_ep_state(void);
uint16_t matter_ep_relay(void);

#ifdef __cplusplus
}
#endif
