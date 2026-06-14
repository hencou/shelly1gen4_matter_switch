#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "app_config.h"
#include "button.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Lua scripting engine for configurable Matter endpoints.
 *
 * Each "slot" represents one Matter endpoint with an associated Lua script.
 * The script determines the endpoint's behavior based on hardware inputs.
 *
 * Configuration is stored in NVS and loaded at boot. Scripts can be edited
 * via the management web interface.
 */

#define SCRIPT_MAX_SLOTS    8
#define SCRIPT_MAX_SIZE     2048   /* max bytes per script */
#define SCRIPT_NAME_LEN     32

/* Endpoint types that can be created dynamically */
typedef enum {
    SLOT_TYPE_NONE = 0,
    SLOT_TYPE_ONOFF_TOGGLE,       /* OnOff Client — sends Toggle to bound devices */
    SLOT_TYPE_ONOFF_STATE,        /* OnOff Client — sends On/Off following input state */
    SLOT_TYPE_DIMMER,             /* LevelControl Client — dimming via binding */
    SLOT_TYPE_OCCUPANCY,          /* OccupancySensing Server — reports to HA */
    SLOT_TYPE_ILLUMINANCE,        /* IlluminanceMeasurement Server — reports to HA */
    SLOT_TYPE_TEMPERATURE,        /* TemperatureMeasurement Server — reports to HA */
    SLOT_TYPE_RELAY,              /* OnOff Light Server — controls internal relay */
} script_slot_type_t;

/* Script execution trigger */
typedef enum {
    TRIGGER_PERIODIC = 0,         /* run every N ms */
    TRIGGER_INPUT_CHANGE,         /* run on any input state change */
    TRIGGER_BUTTON_EVENT,         /* run on button press/release events */
} script_trigger_t;

/* Slot configuration (persisted in NVS) */
typedef struct {
    script_slot_type_t type;
    script_trigger_t trigger;
    uint16_t period_ms;           /* for TRIGGER_PERIODIC */
    char name[SCRIPT_NAME_LEN];   /* user-friendly name */
    char script[SCRIPT_MAX_SIZE]; /* Lua source code */
} script_slot_config_t;

/* Initialize the script engine (call after matter_start) */
esp_err_t script_engine_init(void);

/* Load all slot configs from NVS and create endpoints + start scripts */
esp_err_t script_engine_start(void);

/* Get slot configuration (for web interface) */
esp_err_t script_slot_get(uint8_t slot, script_slot_config_t *cfg);

/* Save slot configuration to NVS and restart the script */
esp_err_t script_slot_set(uint8_t slot, const script_slot_config_t *cfg);

/* Clear a slot (remove endpoint, stop script) */
esp_err_t script_slot_clear(uint8_t slot);

/* Feed a button event into the script engine (called from button ISR callback) */
void script_engine_button_event(input_id_t input, button_event_t evt);

/* Feed sensor data into the script engine */
void script_engine_temperature_update(int16_t centi_c);
void script_engine_occupancy_update(bool occupied);

/* Get endpoint ID for a slot (0 if not active) */
uint16_t script_engine_get_endpoint(uint8_t slot);

/* Get number of active slots */
uint8_t script_engine_active_slots(void);

#ifdef __cplusplus
}
#endif
