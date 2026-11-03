#pragma once

#include "orb_ui.h"

/* Fixed compositions used by shell recovery, Card/Play and the render demo.
 * Keep their existing appearance distinct from the Design 2.0 capsules.
 * Returned roots own their children; callers never inspect child topology. */
lv_obj_t *orb_kit_recovery_button_create(lv_obj_t *parent, const char *label,
                                       int32_t cx, int32_t cy, int32_t width);
lv_obj_t *orb_kit_demo_button_create(lv_obj_t *parent, const char *text,
                                   int32_t cx, int32_t cy, int32_t width,
                                   orb_glass_kind_t kind, lv_color_t tint);
/* The shell installs input/feedback before material and text, preserving the
 * original callback order. Finish once, immediately after that installation. */
lv_obj_t *orb_kit_card_action_create(lv_obj_t *parent);
void orb_kit_card_action_finish(lv_obj_t *obj, const char *caption, bool primary,
                                bool disabled, lv_color_t tint, bool materialise);
/* A clipped, passive indication of the captured rim contact, with no lease. */
lv_obj_t *orb_kit_contact_mark_create(lv_obj_t *parent);
void orb_kit_contact_mark_move(lv_obj_t *mark, int x, int y);
