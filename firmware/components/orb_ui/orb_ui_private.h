#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "orb_kit.h"
#include "orb_kit_motion.h"
#include "orb_kit_controls_motion.h"

/* Allocation for large, rarely written buffers (orb_ui_config_t). */
void *orb_ui_alloc(size_t size);
void orb_ui_free(void *p);

/* Readers cache ink bounds during layout; fades retain complete visible lines. */
void orb_kit_reader_fades(lv_obj_t *reader, int32_t *top, int32_t *bottom);

/* Kit owns the press timer; glass only updates its prepared material. */
bool orb_glass_kit_press(lv_obj_t *obj, int32_t progress);
void orb_glass_kit_resize(lv_obj_t *obj);

/* Design 2.0 uses centreline radii. LVGL arcs use the outside edge, so the
 * widget's radius includes half its stroke. This distinction keeps every
 * gauge and its 30 px knob concentric on both RGB565 and host displays. */
typedef struct {
    lv_obj_t *obj, *knob, *glass, *value_label, *level_icon, *level_title;
    const lv_style_t *value_text_style;
    int32_t radius, start, sweep, min, max, step, detent;
    orb_delight_spring_t spring;
    uint32_t duration;
    orb_kit_ring_format_cb_t formatter;
    void *format_context;
    orb_kit_ring_present_cb_t presenter;
    void *present_context;
    char target_text[96], number_prefix[32], number_suffix[32];
    float number_factor;
    unsigned number_decimals;
    bool number_text;
    bool arrival, moving;
    orb_kit_detent_cb_t callback;
    void *callback_data;
    lv_color_t accent;
    int32_t value_x, value_y;
    bool slider, level, external_label;
} kit_ring_t;

void *kit_event_state(lv_obj_t *obj, lv_event_cb_t callback);
lv_obj_t *kit_knob_create(lv_obj_t *parent, int32_t diameter, lv_obj_t **glass);
lv_obj_t *kit_ring_create(lv_obj_t *parent, int32_t radius, int32_t width, lv_color_t accent);
void kit_ring_present(kit_ring_t *ring);
void kit_ring_value_set(kit_ring_t *ring, int32_t value, bool animated);
void kit_ring_label_position(kit_ring_t *ring);
void kit_ring_event(lv_event_t *e);
void kit_ring_pointer_event(lv_event_t *event);

/* Kit rows: a live value shown through a chord lane narrower than its label
 * is truncated to `width` label pixels with an ellipsis (0 disables). */
void orb_kit_value_fit(lv_obj_t *label, int32_t width);

/* orb_kit_controls.c: a list row's floating switch at its chord scale. */
void orb_kit_switch_row_glass(lv_obj_t *toggle);
void orb_kit_switch_row_present(lv_obj_t *toggle, int32_t width, lv_opa_t opacity);

/* Compose finite glyph motion with a floating row disc shoulder pose. */
void orb_kit_disc_light_up_pose(lv_obj_t *disc, int32_t scale);
bool orb_kit_disc_light_up_matches(lv_obj_t *disc, lv_color_t colour, lv_color_t ink);
void orb_kit_live_ink_opa(lv_obj_t *label, lv_opa_t opacity);

/* Construct presentation state before allocation-free retained-model updates. */
void orb_kit_disc_light_up_prepare(lv_obj_t *disc);
void orb_kit_breathe_prepare(lv_obj_t *label);
void orb_kit_value_roll_prepare(lv_obj_t *label);
