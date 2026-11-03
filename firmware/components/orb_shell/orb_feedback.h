#pragma once
#include "lvgl.h"

/* Shell content feedback; renderer/material ownership remains in orb_ui. */
void orb_feedback_press(lv_obj_t *obj, bool icon);
/* A drag loses its press immediately, without a release spring or flash. */
void orb_feedback_cancel(lv_obj_t *obj);
/* Draws a short tick (or x) with a semantic colour; never conveys state alone. */
void orb_feedback_check(lv_obj_t *parent, lv_color_t color, bool deny);
