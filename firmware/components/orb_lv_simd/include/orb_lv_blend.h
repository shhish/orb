#pragma once

/*
 * LVGL's software renderer, with the ESP32-S3's 128-bit SIMD for the two
 * operations that dominate Orb's frames: solid fills and opaque copies, both
 * into RGB565 in the panel's (swapped) byte order. LVGL includes this through
 * CONFIG_LV_DRAW_SW_ASM_CUSTOM_INCLUDE; each hook returns LV_RESULT_INVALID to
 * fall back to LVGL's own loop.
 */

#include "include/lvgl/lv_types.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_result_t orb_blend_fill_rgb565_swapped(lv_draw_sw_blend_fill_dsc_t *dsc);
lv_result_t orb_blend_copy_rgb565(lv_draw_sw_blend_image_dsc_t *dsc);

#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565_SWAPPED(dsc) orb_blend_fill_rgb565_swapped(dsc)
#define LV_DRAW_SW_RGB565_SWAPPED_BLEND_NORMAL_TO_RGB565_SWAPPED(dsc) orb_blend_copy_rgb565(dsc)
#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565(dsc) orb_blend_copy_rgb565(dsc)

#ifdef __cplusplus
}
#endif
