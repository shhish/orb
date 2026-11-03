#pragma once

#include <stddef.h>

/* Snapshot bytes share MODEL; the remaining view payload shares WORKSPACE. */
void *orb_home_payload_create(size_t view_bytes, size_t model_bytes);
void orb_home_payload_destroy(void *pointer, size_t view_bytes, size_t model_bytes);
