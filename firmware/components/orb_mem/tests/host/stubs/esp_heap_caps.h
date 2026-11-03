#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_DMA 2u
#define MALLOC_CAP_SPIRAM 4u
#define MALLOC_CAP_8BIT 8u

typedef struct {
    size_t total_free_bytes;
    size_t minimum_free_bytes;
    size_t largest_free_block;
} multi_heap_info_t;

typedef void (*esp_alloc_failed_hook_t)(size_t size, uint32_t caps, const char *function);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback);
esp_err_t heap_caps_monitor_local_minimum_free_size_start(void);
esp_err_t heap_caps_monitor_local_minimum_free_size_stop(void);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps);
void *heap_caps_calloc(size_t count, size_t size, uint32_t caps);
