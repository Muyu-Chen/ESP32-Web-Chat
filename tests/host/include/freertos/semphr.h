#pragma once

#include "FreeRTOS.h"

typedef int *SemaphoreHandle_t;
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks);
void xSemaphoreGive(SemaphoreHandle_t mutex);
