#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_SPIRAM (1U << 0)
#define MALLOC_CAP_8BIT (1U << 1)

void *heap_caps_malloc(size_t bytes, uint32_t capabilities);
void heap_caps_free(void *pixels);
