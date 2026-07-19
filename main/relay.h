#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_MAX_CH 2

void relay_init(void);

/* Number of relay channels on the active hardware profile (1 or 2). */
int  relay_channel_count(void);

/* Channel-indexed control (ch = 0-based). Out-of-range channels are ignored. */
void relay_set_ch(int ch, bool on);
bool relay_get_ch(int ch);
void relay_toggle_ch(int ch);

/* Convenience wrappers for channel 0 (the single relay on 1/Mini/1PM). */
void relay_set(bool on);
bool relay_get(void);
void relay_toggle(void);

#ifdef __cplusplus
}
#endif
