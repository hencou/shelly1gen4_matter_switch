#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "script_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the Matter stack with dynamic endpoints based on script slot configs.
 * Each non-NONE slot type creates a corresponding Matter endpoint.
 *
 * @param slot_types  Array of slot types (SCRIPT_MAX_SLOTS elements)
 * @param num_slots   Number of elements in slot_types
 */
esp_err_t matter_start(const script_slot_type_t *slot_types, uint8_t num_slots);

/**
 * Get the Matter endpoint ID assigned to a script slot.
 * Returns 0 if the slot has no endpoint (type == NONE).
 */
uint16_t matter_get_slot_endpoint(uint8_t slot);

/* Command emit to bound nodes/groups (via Binding cluster). */
void matter_send_onoff_toggle(uint16_t local_endpoint_id);
void matter_send_onoff_on(uint16_t local_endpoint_id);
void matter_send_onoff_off(uint16_t local_endpoint_id);
void matter_send_level_move(uint16_t local_endpoint_id, bool up, uint8_t rate);
void matter_send_level_move_to_level(uint16_t local_endpoint_id, uint8_t level, uint16_t transition_ds);
void matter_send_level_stop(uint16_t local_endpoint_id);
void matter_send_color_temp_set(uint16_t local_endpoint_id, uint16_t mireds);
void matter_send_color_temp_move(uint16_t local_endpoint_id, bool warmer, uint16_t rate);
void matter_send_color_temp_stop(uint16_t local_endpoint_id);

/* Sensor attribute updates for Matter server clusters.
 * These iterate all dynamic endpoints to find matching types. */
void matter_update_temperature(int16_t centi_c);
void matter_update_occupancy(bool occupied);

/* Update relay OnOff attribute (report to HA). Called from script engine. */
void matter_update_relay_onoff(bool on);

/* Disable Thread radio so WiFi can use the 2.4 GHz radio exclusively. */
void matter_disable_thread(void);

/* Set the WiFi STA netif as OpenThread Border Router backbone.
 * Must be called BEFORE matter_start(). */
void matter_set_tbr_backbone(void *wifi_sta_netif);

/* Initialize Thread Border Router features after matter_start().
 * Routes IPv6 between WiFi backbone and Thread mesh. */
esp_err_t matter_tbr_init(void);

/* Factory reset → wipes Matter NVS, leaves the fabric, reboot. */
void matter_factory_reset(void);

/* Open a Basic Commissioning Window (180 s) without factory reset.
 * Allows adding the device to another fabric or re-commissioning. */
void matter_open_commissioning_window(void);

#ifdef __cplusplus
}
#endif
