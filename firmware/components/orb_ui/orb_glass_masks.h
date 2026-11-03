#pragma once

#include "lvgl.h"

#define SWELL_STEPS 4             /* sizes a press swells through, rest included */
#define SWELL 0.06f               /* pressed glass is this much bigger, as Apple's controls */

uint64_t orb_glass_masks_clock_us(void);
void *orb_glass_masks_buffer_alloc(lv_draw_buf_t *b, int32_t w, int32_t h);
void orb_glass_masks_buffer_free(lv_draw_buf_t *b);
void orb_glass_masks_light_vector(float degrees, float *lx, float *ly);
void orb_glass_masks_size(int32_t w, int32_t h, int step, int32_t *mw, int32_t *mh);
