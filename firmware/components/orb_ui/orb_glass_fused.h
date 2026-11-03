#pragma once

#include "lvgl.h"

/* The only persistent live material payload is a reduced BGR map. Its input
 * includes the 16px halo from HIG §2.2 and is independent of output bands. */
typedef struct {
    uint8_t *pixels;
    size_t capacity;
    lv_area_t sample;
    uint16_t width, height;
    uint8_t factor;
} orb_glass_fused_t;

bool orb_glass_fused_prepare(orb_glass_fused_t *map, lv_obj_t *obj, const lv_area_t *shape,
                             int32_t radius);
void orb_glass_fused_release(orb_glass_fused_t *map);
void orb_glass_fused_note_fallback(void);
bool orb_glass_fused_draw(const orb_glass_fused_t *map, lv_draw_task_t *task,
                          const lv_draw_blur_dsc_t *dsc, const lv_area_t *shape,
                          const lv_draw_buf_t *dark, const lv_draw_buf_t *rim,
                          int32_t shadow_pad, lv_opa_t amount);
