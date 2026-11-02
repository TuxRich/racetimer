/* The clock gate.c reads, ESP-IDF's esp_timer, on the Teensy: microseconds
 * since boot, 64 bits (main.cpp). GPL-3.0-or-later. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
