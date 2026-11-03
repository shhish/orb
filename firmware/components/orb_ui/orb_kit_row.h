#pragma once

#include "orb_kit.h"

typedef struct {
    int32_t delta, placed_style;
} kit_flow_t;

typedef struct {
    lv_obj_t *row, *list, *title, *subtitle, *value, *toggle, *anchor, *disc, *live[3];
    lv_obj_t *live_clip[3], *live_disc, *live_glyph;
    lv_display_t *overlay_display;
    char *live_template[3];
    int32_t trailing;
    kit_flow_t flow;
    bool flowed, stacked, switch_below, wrap_title, full_title, live_resume[3];
    orb_text_variant_t title_variant, subtitle_variant, value_variant;
} kit_row_t;

void orb_kit_row_layout(lv_obj_t *row, kit_row_t *state);

/* Private state access for row effect presenters in this component. */
kit_row_t *orb_kit_row_state(lv_obj_t *row);
