#pragma once

#include <stddef.h>

/* Raw, opaque copy using the same Espressif S3 SIMD kernel as LVGL. Source
 * and destination may have arbitrary pixel alignment; no bytes are swapped. */
void orb_simd_copy_rgb565(void *destination, const void *source, size_t pixels);
