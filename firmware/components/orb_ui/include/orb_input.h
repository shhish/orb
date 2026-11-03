#pragma once

#include "lvgl.h"

/* Set once at creation on the developer pointer shared by USB and Wi-Fi.
 * Consequential UI handlers inspect the activating pointer, never a transport. */
void orb_input_mark_synthetic(lv_indev_t *input);
bool orb_input_is_synthetic(const lv_indev_t *input);
bool orb_input_active_is_synthetic(void);

/* Refuse a synthetic activation and briefly invite a physical answer. Does
 * not change selection, model state, focus or the current surface. UI task only. */
bool orb_input_require_physical(lv_obj_t *surface);

