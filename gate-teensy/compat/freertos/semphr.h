/* GPL-3.0-or-later. See FreeRTOS.h. */
#pragma once
#include "FreeRTOS.h"

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    static char lock;
    return &lock;
}

static inline int xSemaphoreTake(SemaphoreHandle_t lock, unsigned ticks) {
    (void)lock;
    (void)ticks;
    return 1;
}

static inline int xSemaphoreGive(SemaphoreHandle_t lock) {
    (void)lock;
    return 1;
}
