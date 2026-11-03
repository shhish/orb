#pragma once

#include "lvgl.h"
#include "orb_fonts.h"

/* Presented label ink used by previews and the public layout projection. */
lv_area_t orb_faces_label_ink(lv_obj_t *object);
int orb_faces_text_width(lv_obj_t *object, const char *value);
/* Fit from the start on whole UTF-8 characters, ending in "…". */
void orb_faces_caption_fit(lv_obj_t *object, const char *source, int width, char *out, size_t capacity);
void orb_faces_polar(lv_point_precise_t *point, float degrees, float radius);
lv_obj_t *orb_faces_clock_line(lv_obj_t *parent, lv_point_precise_t points[2], int width, lv_color_t colour);

/* Retained-label geometry shared by full faces and their scaled artwork. */
void orb_faces_at(lv_obj_t *object, int cx, int cy);
void orb_faces_lane_at(int scale, lv_obj_t *object, int cx, int cy);
void orb_faces_ink_set(lv_obj_t *object, lv_color_t colour, lv_opa_t opacity);
void orb_faces_static_text(lv_obj_t *object, char *storage, size_t capacity, const char *value);
lv_obj_t *orb_faces_text_create(lv_obj_t *parent, const char *value, int cx, int cy,
                                 int width, orb_text_style_t style, lv_color_t colour);
void orb_faces_clock_fit(lv_obj_t *object, const char *clock, int cy, bool medium, int width);
void orb_faces_ring_ink(lv_obj_t *ring, bool locked, bool stale);
