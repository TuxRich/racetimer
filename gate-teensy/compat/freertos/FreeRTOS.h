/* gate.c and settings.c guard their state with FreeRTOS mutexes, because
 * on the WROOM the C5 links, the web server and the UI are separate tasks.
 * The Teensy runs all of them from loop(), none from an interrupt, so the
 * locks have nothing to guard (semphr.h). GPL-3.0-or-later. */
#pragma once

typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0
