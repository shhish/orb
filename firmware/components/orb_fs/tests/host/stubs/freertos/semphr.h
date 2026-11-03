#pragma once
#include <stdint.h>
typedef int StaticSemaphore_t;
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
int xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t ticks);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
