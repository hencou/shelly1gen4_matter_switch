#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void relay_init(void);
void relay_set(bool on);
bool relay_get(void);
void relay_toggle(void);

#ifdef __cplusplus
}
#endif
