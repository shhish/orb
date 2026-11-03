/* Level (arc slider) controls, split from orb_kit_controls.c for the size guard rail. */
#include "orb_kit.h"
#include "orb_ui_private.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "src/misc/lv_text_private.h"
#include "src/misc/lv_area_private.h"

static int32_t angle_normalise(int32_t angle)
{
    angle %= 360;
    return angle < 0 ? angle + 360 : angle;
}

static void ring_fonts(lv_observer_t *observer, lv_subject_t *subject)
{
    LV_UNUSED(subject);
    kit_ring_t *ring = kit_event_state(lv_observer_get_target_obj(observer), kit_ring_event);
    if (!ring || !ring->value_label) return;
    orb_kit_value_set(ring->value_label, lv_label_get_text(ring->value_label), NULL);
    kit_ring_label_position(ring);
}

lv_obj_t *orb_kit_rim_slider_create(lv_obj_t *parent, int32_t radius, int32_t width,
                                   lv_color_t accent, int32_t min, int32_t max,
                                   int32_t detent_step, orb_kit_detent_cb_t cb, void *user_data)
{
    if (min >= max || detent_step < 0) return NULL;
    lv_obj_t *obj = kit_ring_create(parent, radius, width, accent);
    if (!obj) return NULL;
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    ring->slider = true;
    ring->min = min;
    ring->max = max;
    ring->step = detent_step;
    ring->callback = cb;
    ring->callback_data = user_data;
    lv_arc_set_range(obj, min, max);
    /* Absolute touch tracking, not an implicit volume/brightness slew. */
    lv_arc_set_change_rate(obj, 36000);
    lv_obj_set_clickable(obj, true);
    lv_obj_set_adv_hittest(obj, true);
    lv_obj_set_overflow_visible(obj, true);
    lv_obj_set_ext_click_area(obj, 16);
    ring->knob = kit_knob_create(obj, ORB_KIT_KNOB, &ring->glass);
    lv_obj_refresh_ext_draw_size(obj);
    lv_obj_add_event_cb(obj, kit_ring_pointer_event, LV_EVENT_PRESSING | LV_EVENT_PREPROCESS, ring);
    lv_obj_add_event_cb(obj, kit_ring_pointer_event, LV_EVENT_KEY | LV_EVENT_PREPROCESS, ring);
    lv_obj_add_event_cb(obj, kit_ring_pointer_event, LV_EVENT_ROTARY | LV_EVENT_PREPROCESS, ring);
    orb_kit_rim_slider_set_angles(obj, 135, 270);
    kit_ring_value_set(ring, min, false);
    return obj;
}

void orb_kit_rim_slider_set_angles(lv_obj_t *obj, int32_t start, int32_t sweep)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring || !ring->slider || sweep == 0 || sweep < -360 || sweep > 360) return;
    ring->start = angle_normalise(start);
    ring->sweep = sweep;
    lv_arc_set_mode(obj, sweep < 0 ? LV_ARC_MODE_REVERSE : LV_ARC_MODE_NORMAL);
    lv_arc_set_rotation(obj, angle_normalise(sweep < 0 ? start + sweep : start));
    lv_arc_set_bg_angles(obj, 0, LV_ABS(sweep));
    kit_ring_present(ring);
}

void orb_kit_rim_slider_set_value(lv_obj_t *obj, int32_t value, bool animated)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring || !ring->slider) return;
    kit_ring_value_set(ring, value, animated);
    if (ring->step > 0) ring->detent = ((int32_t)ring->spring.target - ring->min + ring->step / 2) / ring->step;
}

int32_t orb_kit_rim_slider_get_value(lv_obj_t *obj)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    return ring && ring->slider ? (int32_t)lroundf(ring->spring.target) : 0;
}

lv_obj_t *orb_kit_level_create(lv_obj_t *parent, lv_color_t accent, int32_t min, int32_t max,
                               int32_t step, orb_kit_detent_cb_t cb, void *user_data)
{
    lv_obj_t *obj = orb_kit_rim_slider_create(parent, 213, 14, accent, min, max, step, cb, user_data);
    if (!obj) return NULL;
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    ring->level = true;
    ring->value_y = 380;
    orb_kit_rim_slider_set_angles(obj, 145, -110);
    char text[24], reservation[24];
    snprintf(text, sizeof(text), "%ld%%", (long)min);
    snprintf(reservation, sizeof(reservation), "%ld%%", (long)max);
    orb_kit_ring_set_text(obj, text, reservation);
    orb_kit_level_set_text_style(obj, ORB_TEXT_CAPTION2, ORB_TEXT_EMPHASIZED);
    lv_subject_add_observer_obj(orb_fonts_subject(), ring_fonts, obj, NULL);
    return obj;
}

void orb_kit_level_set_value(lv_obj_t *obj, int32_t value, bool animated)
{
    orb_kit_rim_slider_set_value(obj, value, animated);
}

void orb_kit_level_set_text(lv_obj_t *obj, const char *text, const char *reserve_template)
{
    orb_kit_ring_set_text(obj, text, reserve_template);
}

void orb_kit_level_set_value_center(lv_obj_t *obj, int32_t x, int32_t y)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring) return;
    ring->value_x = x;
    ring->value_y = y;
    kit_ring_label_position(ring);
}

void orb_kit_level_set_value_y(lv_obj_t *obj, int32_t y)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (ring) orb_kit_level_set_value_center(obj, ring->value_x, y);
}

void orb_kit_level_set_text_style(lv_obj_t *obj, orb_text_style_t style, orb_text_variant_t variant)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring || !ring->value_label) return;
    variant |= ORB_TEXT_TABULAR;
    const lv_style_t *text_style = orb_text_style(style, variant);
    /* Rescue faces can alias roles. Cache the attached semantic style rather
     * than infer its identity from pixels; unchanged preview ticks stay idle. */
    if (ring->value_text_style == text_style &&
        lv_obj_get_style_text_font(ring->value_label, 0) == orb_font_variant(style, variant) &&
        lv_obj_get_style_text_letter_space(ring->value_label, 0) == orb_text_style_info(style)->letter_space &&
        lv_obj_get_style_text_line_space(ring->value_label, 0) == orb_text_line_space(style, variant)) return;
    orb_text_apply(ring->value_label, style, variant, 0);
    ring->value_text_style = text_style;
    /* Explicit type/template configuration is an idle operation. Reusing the
     * public setter retains the reservation without changing it per tick. */
    orb_kit_value_set(ring->value_label, lv_label_get_text(ring->value_label), NULL);
    kit_ring_label_position(ring);
}

void orb_kit_level_set_icon(lv_obj_t *obj, const char *glyph)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring) return;
    if (!ring->level_icon) {
        ring->level_icon = lv_label_create(obj);
        orb_icon_apply(ring->level_icon, ORB_TEXT_TITLE3, ORB_TEXT_EMPHASIZED, 0);
        lv_obj_set_style_text_color(ring->level_icon, ring->accent, 0);
        lv_obj_set_clickable(ring->level_icon, false);
        lv_obj_set_scrollable(ring->level_icon, false);
    }
    lv_label_set_text(ring->level_icon, glyph ? glyph : "");
    kit_ring_label_position(ring);
}

void orb_kit_level_set_state(lv_obj_t *obj, lv_color_t fill, lv_color_t value, bool known)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring || !ring->level) return;
    /* Called on every preview tick: unchanged styles must not invalidate. */
    if (!lv_color_eq(lv_obj_get_style_arc_color(obj, LV_PART_INDICATOR), fill)) {
        lv_obj_set_style_arc_color(obj, fill, LV_PART_MAIN);
        lv_obj_set_style_arc_color(obj, fill, LV_PART_INDICATOR);
    }
    /* An unknown level has no fill or knob to assert a value (HIG §8). */
    lv_opa_t fill_opa = known ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (lv_obj_get_style_arc_opa(obj, LV_PART_INDICATOR) != fill_opa)
        lv_obj_set_style_arc_opa(obj, fill_opa, LV_PART_INDICATOR);
    if (ring->knob && lv_obj_is_hidden(ring->knob) == known) lv_obj_set_hidden(ring->knob, !known);
    if (!ring->value_label) return;
    lv_opa_t opa = lv_color_eq(value, ORB_KIT_SECONDARY) ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER;
    if (!lv_color_eq(lv_obj_get_style_text_color(ring->value_label, 0), value))
        lv_obj_set_style_text_color(ring->value_label, value, 0);
    if (lv_obj_get_style_text_opa(ring->value_label, 0) != opa) lv_obj_set_style_text_opa(ring->value_label, opa, 0);
    if (ring->level_icon && !lv_color_eq(lv_obj_get_style_text_color(ring->level_icon, 0), value)) {
        lv_obj_set_style_text_color(ring->level_icon, value, 0); /* the glyph reads with its value */
        lv_obj_set_style_text_opa(ring->level_icon, opa, 0);
    }
}

void orb_kit_level_set_value_hidden(lv_obj_t *obj, bool hidden)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (ring && ring->value_label) lv_obj_set_hidden(ring->value_label, hidden);
}

static void ring_title_deleted(lv_event_t *event)
{
    kit_ring_t *ring = lv_event_get_user_data(event);
    ring->level_title = NULL;
}

void orb_kit_level_set_title(lv_obj_t *obj, const char *text)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring) return;
    if (!ring->level_title) {
        /* The level is a bottom arc, so its title shares every sibling's
         * 200px rim baseline rather than the full-ring inset radius. */
        ring->level_title = orb_kit_rim_title_create(lv_obj_get_parent(obj), text, ring->accent);
        if (ring->level_title)
            lv_obj_add_event_cb(ring->level_title, ring_title_deleted, LV_EVENT_DELETE, ring);
    } else orb_kit_rim_text_set(ring->level_title, text, NULL);
}
