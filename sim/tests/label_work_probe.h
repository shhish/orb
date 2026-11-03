#pragma once

#include "lvgl.h"
#include "src/misc/lv_text_private.h"
#include "src/draw/lv_draw_private.h"

bool label_work_font_lookup(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                            uint32_t letter, uint32_t next);
bool label_work_glyph_impl(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                           uint32_t letter, uint32_t next);
const void *label_work_bitmap_impl(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *buffer);
void label_work_size_impl(lv_point_t *size, const char *text, const lv_font_t *font,
                          lv_text_attributes_t *attributes);
uint32_t label_work_line_impl(const char *text, uint32_t length, const lv_font_t *font,
                              int32_t *width, lv_text_attributes_t *attributes);
int32_t label_work_width_impl(const char *text, uint32_t length, const lv_font_t *font,
                              const lv_text_attributes_t *attributes);
void label_work_draw_impl(lv_draw_task_t *task, const lv_draw_label_dsc_t *descriptor,
                          const lv_area_t *coords);
