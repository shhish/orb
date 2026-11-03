/** ORB: leased, byte-verified backdrop cache; internal renderer interface. */
#ifndef LV_DRAW_SW_BLUR_CACHE_PRIVATE_H
#define LV_DRAW_SW_BLUR_CACHE_PRIVATE_H
#include "lv_draw_sw.h"
#if LV_USE_DRAW_SW

typedef struct _lv_blur_cache_entry_t lv_blur_cache_entry_t;
typedef enum {
    LV_BLUR_CACHE_BYPASS,
    LV_BLUR_CACHE_HIT,
    LV_BLUR_CACHE_MISS
} lv_blur_cache_result_t;

void lv_draw_sw_blur_cache_init(void);
void lv_draw_sw_blur_cache_deinit(void);
lv_blur_cache_result_t lv_draw_sw_blur_cache_begin(const lv_draw_task_t * task,
    const lv_draw_blur_dsc_t * dsc, const lv_area_t * coords, const lv_area_t * clipped,
    uint8_t * source, uint32_t stride, uint32_t pixel_size, lv_blur_cache_entry_t ** lease);
void lv_draw_sw_blur_cache_finish(lv_blur_cache_entry_t * lease, const uint8_t * source,
                                  uint32_t stride, bool rendered);
#endif
#endif
