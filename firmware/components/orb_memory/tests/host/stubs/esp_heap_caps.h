#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 0x400u
#define MALLOC_CAP_8BIT 0x4u
void *heap_caps_malloc(size_t bytes, unsigned caps);
void heap_caps_free(void *pointer);
size_t heap_caps_get_free_size(unsigned caps);
