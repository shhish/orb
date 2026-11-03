#pragma once
#include "FreeRTOS.h"
int xTaskCreateWithCaps(void (*entry)(void *), const char *name, unsigned stack,
                        void *context, unsigned priority, TaskHandle_t *handle,
                        unsigned capabilities);
uint32_t ulTaskNotifyTake(int clear, uint32_t wait);
void xTaskNotifyGive(TaskHandle_t task);

TaskHandle_t xTaskGetCurrentTaskHandle(void);
