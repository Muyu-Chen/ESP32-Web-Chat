#pragma once

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                      void *arg, unsigned priority, TaskHandle_t *handle);
void vTaskDelay(uint32_t ticks);
