#pragma once

#include "orb_rim_math.h"
#include "lvgl.h"

/* Retained flat content, never glass. Only the caller's touched knob becomes
 * glass. No timers or ambient invalidation are created (hig.md §§1/2/6). */
void orb_rim_draw_segment(const orb_rim_config_t *config, lv_layer_t *layer,
                          lv_point_t origin, float start, float end,
                          int radius, int width, lv_color_t colour);
void orb_rim_draw_ticks(const orb_rim_config_t *config, lv_layer_t *layer,
                        lv_point_t origin, unsigned steps, int selected,
                        lv_color_t selected_colour, lv_color_t secondary);
void orb_rim_invalidate(const orb_rim_config_t *config, lv_obj_t *canvas,
                        float before, float after);

