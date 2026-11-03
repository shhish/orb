#pragma once

#include "orb_kit.h"

/* Private to the kit: a live value label's reserved lane (orb_kit_value.c).
 * Buttons, chips and discs in orb_kit.c share the same live-value contract. */
typedef struct {
    lv_obj_t *label;
    char *reservation, *text;
    size_t text_capacity;
    int32_t width, minimum_width;
    const lv_font_t *measured_font;
    int32_t measured_spacing, measured_max_width;
    unsigned template_measures, updates;
    char *semantic;       /* Untruncated live text while a fitted lane is set. */
    int32_t fit;          /* Visible lane width in label pixels; 0 = none. */
    char *fitted;
    int32_t *fit_prefix;
    size_t semantic_capacity;
    const lv_font_t *fit_font;
    int32_t fit_spacing, fit_full_width, fit_ellipsis_width;
    bool tabular;
    char projection[96];
    bool projected;
} kit_value_t;

kit_value_t *orb_kit_value_find(lv_obj_t *label);
void orb_kit_value_measure(kit_value_t *value);

/* Normal updates and chord/font changes retire an in-flight digit roll. */
void orb_kit_value_roll_finish(lv_obj_t *label);
bool orb_kit_value_roll_set_ink_opa(lv_obj_t *label, lv_opa_t opacity);
