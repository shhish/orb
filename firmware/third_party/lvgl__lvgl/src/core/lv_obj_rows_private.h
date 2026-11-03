/* ORB PATCH: optional glyph-at-size presentation of simple matte rows. */
#ifndef LV_OBJ_ROWS_PRIVATE_H
#define LV_OBJ_ROWS_PRIVATE_H

#include "../lvgl_public.h"

typedef struct lv_obj_rows_t lv_obj_rows_t;
lv_obj_rows_t * lv_obj_rows_create(lv_obj_t * obj, lv_color_t matte, lv_draw_buf_malloc_cb_t alloc,
                                  lv_draw_buf_free_cb_t release, lv_obj_t * focus, uint16_t preview_scale);
void lv_obj_rows_destroy(lv_obj_rows_t * rows);
bool lv_obj_rows_draw(lv_obj_rows_t * rows, lv_layer_t * layer, const lv_area_t * area,
                     uint16_t scale, lv_opa_t opa, lv_color_t matte);
bool lv_obj_rows_sample(lv_obj_rows_t * rows, const lv_area_t * area, uint16_t scale,
                       int32_t x, int32_t y, lv_color32_t * color);
bool lv_obj_rows_sample_area(lv_obj_rows_t * rows, const lv_area_t * area, uint16_t scale,
                            const lv_area_t * sample, lv_color32_t * color);

#endif
