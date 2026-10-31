#pragma once
#include "FreeRTOS.h"
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t m, unsigned) { m->lock(); return pdTRUE; }
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t m) { m->unlock(); return pdTRUE; }
