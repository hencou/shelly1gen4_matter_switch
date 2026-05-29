#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback signatures — aangeroepen door sensor-tasks bij nieuwe waarde */
typedef void (*temp_cb_t)(int16_t temp_centi_c);       /* 0.01 °C units, ZCL native */
typedef void (*occupancy_cb_t)(bool occupied);

void sensors_init(temp_cb_t temp_cb, occupancy_cb_t occ_cb);

#ifdef __cplusplus
}
#endif
