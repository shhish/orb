#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT (1u << 0)
#define MALLOC_CAP_INTERNAL (1u << 1)
#define MALLOC_CAP_SPIRAM (1u << 2)
#define MALLOC_CAP_DMA (1u << 3)

void *heap_caps_malloc(size_t size, uint32_t caps);
void orb_test_free(void *pointer);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
