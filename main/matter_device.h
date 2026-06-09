#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the Matter stack with 5 endpoints:
 *   EP1 = OnOff Light Switch (pushbutton) + Binding cluster — Toggle
 *   EP2 = OnOff Light Switch (state-follow) + Binding — On/Off follows contact
 *   EP3 = Temperature Sensor
 *   EP4 = Occupancy Sensor
 *   EP5 = OnOff Light (relay) — server, controllable from HA
 */
esp_err_t matter_start(void);

/* Command emit to bound nodes/groups (via Binding cluster). */
void matter_send_onoff_toggle(uint16_t local_endpoint_id);
void matter_send_onoff_on(uint16_t local_endpoint_id);
void matter_send_onoff_off(uint16_t local_endpoint_id);
void matter_send_level_move(uint16_t local_endpoint_id, bool up, uint8_t rate);
void matter_send_level_stop(uint16_t local_endpoint_id);
void matter_send_color_temp_set(uint16_t local_endpoint_id, uint16_t mireds);
void matter_send_color_temp_move(uint16_t local_endpoint_id, bool warmer, uint16_t rate);
void matter_send_color_temp_stop(uint16_t local_endpoint_id);

/* Sensor attribute updates for Matter server clusters. */
void matter_update_temperature(int16_t centi_c);
void matter_update_occupancy(bool occupied);

/* Factory reset → wipes Matter NVS, leaves the fabric, reboot. */
void matter_factory_reset(void);

/* Endpoint ID lookups for app_main.c (after matter_start). */
uint16_t matter_ep_pushbutton(void);
uint16_t matter_ep_state(void);
uint16_t matter_ep_relay(void);

#ifdef __cplusplus
}
#endif
