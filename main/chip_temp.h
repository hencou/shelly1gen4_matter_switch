#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the ESP32-C6 built-in temperature sensor (the Shelly's internal SoC
 * temperature). Returns true and writes degrees Celsius to *out on success.
 * A single sensor handle is shared process-wide (the SoC has only one), so this
 * is safe to call from both the web diagnostics and the Lua script engine. */
bool chip_temp_read(float *out);

#ifdef __cplusplus
}
#endif
