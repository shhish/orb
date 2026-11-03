/** ORB: bounded retained exact material presentation; UI-thread seams. */
#ifndef LV_OBJ_MATERIAL_PRIVATE_H
#define LV_OBJ_MATERIAL_PRIVATE_H
#include "../lvgl_public.h"
void lv_obj_material_count_kernel(void);
void lv_obj_material_count_solid(void);
uint64_t lv_obj_material_preparation_begin(void);
void lv_obj_material_preparation_end(uint64_t started);
#if defined(CONFIG_ORB_DISPLAY_RETAINED_MATERIAL) && CONFIG_ORB_DISPLAY_RETAINED_MATERIAL
void lv_obj_material_note_invalidation(const lv_obj_t * obj);
void lv_obj_material_note_motion(lv_display_t * display);
bool lv_obj_material_managed(const lv_draw_blur_dsc_t * dsc);
bool lv_obj_material_skip_blur(const lv_draw_blur_dsc_t * dsc);
bool lv_obj_material_filter_get(const lv_draw_task_t * task, const lv_draw_blur_dsc_t * dsc,
                               const lv_area_t * coords, const lv_area_t * clipped,
                               const uint8_t * source, uint32_t stride, uint32_t px_size,
                               uint8_t * small, size_t bytes);
void lv_obj_material_filter_store(const lv_draw_blur_dsc_t * dsc, const uint8_t * small, size_t bytes);
void lv_obj_material_store_solid(const lv_draw_blur_dsc_t * dsc, const lv_area_t * coords,
                               const lv_area_t * clipped, const uint8_t * source, uint32_t stride,
                               const uint8_t rgb[3], size_t bytes);
bool lv_obj_material_cover(lv_obj_t * obj, const lv_area_t * area);
void lv_obj_material_restore(lv_layer_t * layer, lv_obj_t * obj);
void lv_obj_material_expand_draw(lv_obj_t * obj, lv_area_t * area);
#else
static inline void lv_obj_material_note_invalidation(const lv_obj_t * obj) { LV_UNUSED(obj); }
static inline void lv_obj_material_note_motion(lv_display_t * display) { LV_UNUSED(display); }
static inline bool lv_obj_material_managed(const lv_draw_blur_dsc_t * dsc) { LV_UNUSED(dsc); return false; }
static inline bool lv_obj_material_skip_blur(const lv_draw_blur_dsc_t * dsc) { LV_UNUSED(dsc); return false; }
static inline bool lv_obj_material_filter_get(const lv_draw_task_t * task, const lv_draw_blur_dsc_t * dsc,
    const lv_area_t * coords, const lv_area_t * clipped, const uint8_t * source,
    uint32_t stride, uint32_t px_size, uint8_t * small, size_t bytes)
{
    LV_UNUSED(task); LV_UNUSED(dsc); LV_UNUSED(coords); LV_UNUSED(clipped); LV_UNUSED(source);
    LV_UNUSED(stride); LV_UNUSED(px_size); LV_UNUSED(small); LV_UNUSED(bytes); return false;
}
static inline void lv_obj_material_filter_store(const lv_draw_blur_dsc_t * dsc, const uint8_t * small, size_t bytes)
{ LV_UNUSED(dsc); LV_UNUSED(small); LV_UNUSED(bytes); }
static inline void lv_obj_material_store_solid(const lv_draw_blur_dsc_t * dsc, const lv_area_t * coords,
    const lv_area_t * clipped, const uint8_t * source, uint32_t stride, const uint8_t rgb[3], size_t bytes)
{ LV_UNUSED(dsc); LV_UNUSED(coords); LV_UNUSED(clipped); LV_UNUSED(source); LV_UNUSED(stride); LV_UNUSED(rgb); LV_UNUSED(bytes); }
static inline bool lv_obj_material_cover(lv_obj_t * obj, const lv_area_t * area)
{
    LV_UNUSED(obj); LV_UNUSED(area); return false;
}
static inline void lv_obj_material_expand_draw(lv_obj_t * obj, lv_area_t * area)
{ LV_UNUSED(obj); LV_UNUSED(area); }
static inline void lv_obj_material_restore(lv_layer_t * layer, lv_obj_t * obj)
{
    LV_UNUSED(layer); LV_UNUSED(obj);
}
#endif
#endif
