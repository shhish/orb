#include "orb_kit.h"
#include "orb_ui_private.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "src/misc/lv_text_private.h"
#include "src/misc/lv_area_private.h"

typedef struct {
    lv_obj_t *obj, *knob, *glass, *small_glass;
    lv_color_t accent;
    orb_delight_spring_t spring;
    float scale;
    lv_opa_t opacity;
    bool checked, touched;
} kit_switch_t;

void kit_ring_label_position(kit_ring_t *ring)
{
    if (!ring->value_label || ring->external_label) return;
    lv_obj_update_layout(ring->value_label);
    int32_t origin = ORB_SCREEN_R - lv_obj_get_width(ring->obj) / 2;
    int32_t x = ring->value_x - origin - lv_obj_get_width(ring->value_label) / 2;
    int32_t y = ring->value_y - origin - lv_obj_get_height(ring->value_label) / 2;
    if (lv_obj_get_x(ring->value_label) != x || lv_obj_get_y(ring->value_label) != y)
        lv_obj_set_pos(ring->value_label, x, y);
    if (ring->level_icon) {
        lv_obj_update_layout(ring->level_icon);
        lv_obj_set_pos(ring->level_icon,
                       ring->value_x - origin - lv_obj_get_width(ring->level_icon) / 2,
                       ring->value_y - origin - lv_obj_get_height(ring->value_label) / 2 -
                       lv_obj_get_height(ring->level_icon) - 12);
    }
}

static void ring_format(kit_ring_t *ring, float value, char *text, size_t capacity)
{
    if (ring->formatter) ring->formatter(value, text, capacity, ring->format_context);
    else if (ring->number_text)
        snprintf(text, capacity, "%s%.*f%s", ring->number_prefix, (int)ring->number_decimals,
                 (double)(value * ring->number_factor), ring->number_suffix);
    else if (ring->level) snprintf(text, capacity, "%ld%%", (long)lroundf(value));
    else snprintf(text, capacity, "%s", ring->target_text);
}

static void ring_label_value(kit_ring_t *ring)
{
    if (!ring->value_label || (!ring->level && !ring->formatter && !ring->number_text)) return;
    char text[96];
    ring_format(ring, ring->spring.target, ring->target_text, sizeof(ring->target_text));
    ring_format(ring, ring->spring.value, text, sizeof(text));
    orb_kit_value_set(ring->value_label, text, NULL);
}

static void switch_event(lv_event_t *e);
static void ring_label_event(lv_event_t *event)
{
    kit_ring_t *ring = lv_event_get_user_data(event);
    ring->value_label = NULL;
    ring->external_label = false;
}

void *kit_event_state(lv_obj_t *obj, lv_event_cb_t callback)
{
    if (!obj) return NULL;
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(descriptor) == callback) return lv_event_dsc_get_user_data(descriptor);
    }
    return NULL;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    return obj;
}

static void knob_touch(lv_obj_t *knob, lv_obj_t *glass, bool touched)
{
    if (!knob || !glass) return;
    /* HIG §§1/7: the knob is ordinary white content until touched. Its glass
     * is prepared at construction, so a contact never constructs a material. */
    lv_obj_set_style_bg_opa(knob, touched ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    orb_glass_set_amount(glass, touched ? 1.f : 0.f);
}

lv_obj_t *kit_knob_create(lv_obj_t *parent, int32_t diameter, lv_obj_t **glass)
{
    lv_obj_t *knob = plain(parent);
    lv_obj_set_size(knob, diameter, diameter);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    *glass = plain(knob);
    lv_obj_set_size(*glass, diameter, diameter);
    orb_glass_apply(*glass, ORB_GLASS_REGULAR, lv_color_white());
    orb_glass_set_amount(*glass, 0);
    return knob;
}

void kit_ring_present(kit_ring_t *ring)
{
    int32_t value = (int32_t)lroundf(ring->slider ? ring->spring.value : ring->spring.value * 10.f);
    lv_arc_set_value(ring->obj, value);
    if (ring->presenter) ring->presenter(ring->spring.value, ring->spring.target, ring->present_context);
    if (ring->knob) {
        float fraction = (ring->spring.value - ring->min) / (ring->max - ring->min);
        float angle = (ring->start + ring->sweep * fraction) * (float)M_PI / 180.f;
        int32_t centre = lv_obj_get_width(ring->obj) / 2;
        lv_obj_set_pos(ring->knob,
                       centre + (int32_t)lroundf(ring->radius * cosf(angle)) - ORB_KIT_KNOB / 2,
                       centre + (int32_t)lroundf(ring->radius * sinf(angle)) - ORB_KIT_KNOB / 2);
    }
}

static const char s_ring_motion_key;

static bool ring_tick(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    kit_ring_t *ring = context;
    if (finish || elapsed >= ring->duration) {
        orb_delight_spring_reset(&ring->spring, ring->spring.target);
        ring->moving = false;
    } else {
        orb_delight_spring_step(&ring->spring, dt / 1000.f,
                                 ring->duration / 1000.f, .15f);
        ring->spring.value = LV_CLAMP(ring->min, ring->spring.value, ring->max);
    }
    kit_ring_present(ring);
    ring_label_value(ring);
    return ring->moving;
}

void kit_ring_value_set(kit_ring_t *ring, int32_t value, bool animated)
{
    if (!ring) return;
    value = LV_CLAMP(ring->min, value, ring->max);
    /* An unchanged projection preserves the prepared arrival fence. Explicit
     * final-value setters still cancel it for Back, restore and touch. */
    if (animated && ring->spring.target == value) return;
    if (ring->arrival) orb_kit_arrival_complete(ring->obj, false);
    ring->arrival = false;
    orb_delight_spring_target(&ring->spring, value);
    if (!animated || orb_ui_reduce_motion()) {
        orb_kit_motion_cancel(ring->obj, &s_ring_motion_key, false);
        ring->moving = false;
        orb_delight_spring_reset(&ring->spring, value);
        kit_ring_present(ring);
        ring_label_value(ring);
        return;
    }
    ring->duration = ORB_KIT_VALUE_MS;
    ring->moving = true;
    ring_label_value(ring);
    orb_kit_motion_start(ring->obj, &s_ring_motion_key, ring_tick, ring);
}

static void ring_arrival(void *context, bool animate)
{
    kit_ring_t *ring = context;
    if (!ring->arrival) return;
    ring->arrival = false;
    ring->duration = 420;
    ring->moving = animate;
    if (animate) orb_kit_motion_start(ring->obj, &s_ring_motion_key, ring_tick, ring);
    else ring_tick(ring, 0, 0, true);
}

void orb_kit_ring_set_arrival(lv_obj_t *obj, int32_t value)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring) return;
    kit_ring_value_set(ring, value, false);
    if (orb_ui_reduce_motion() || ring->spring.target == ring->min) return;
    orb_delight_spring_reset(&ring->spring, ring->min);
    orb_delight_spring_target(&ring->spring, LV_CLAMP(ring->min, value, ring->max));
    ring->arrival = true;
    if (ring->step > 0)
        ring->detent = ((int32_t)lroundf(ring->spring.target) - ring->min + ring->step / 2) / ring->step;
    kit_ring_present(ring);
    ring_label_value(ring);
    orb_kit_arrival_prepare(obj, ring_arrival, ring);
}

void orb_kit_rim_slider_set_arrival(lv_obj_t *obj, int32_t value)
{
    orb_kit_ring_set_arrival(obj, value);
}

void orb_kit_level_set_arrival(lv_obj_t *obj, int32_t value)
{
    orb_kit_ring_set_arrival(obj, value);
}

void orb_kit_ring_set_formatter(lv_obj_t *obj, orb_kit_ring_format_cb_t callback, void *context)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring) return;
    ring->formatter = callback;
    ring->format_context = context;
    ring_label_value(ring);
}

void orb_kit_ring_set_presenter(lv_obj_t *obj, orb_kit_ring_present_cb_t callback, void *context)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring) return;
    ring->presenter = callback;
    ring->present_context = context;
    if (callback) callback(ring->spring.value, ring->spring.target, context);
}

float orb_kit_ring_get_presented_value(lv_obj_t *obj)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    return ring ? ring->spring.value : 0;
}

bool orb_kit_ring_semantic_value(lv_obj_t *obj, int32_t *out)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring || !out) return false;
    *out = (int32_t)lroundf(ring->spring.target * (ring->slider ? 1.f : 10.f));
    return true;
}

bool orb_kit_ring_semantic_text(lv_obj_t *label, const char **out)
{
    if (!label || !out) return false;
    kit_ring_t *ring = kit_event_state(label, ring_label_event);
    if (!ring) ring = kit_event_state(lv_obj_get_parent(label), kit_ring_event);
    if (!ring || ring->value_label != label) return false;
    *out = ring->target_text;
    return true;
}

static void ring_detent(kit_ring_t *ring)
{
    if (!ring->slider || !ring->callback || ring->step <= 0) return;
    int32_t value = (int32_t)lroundf(ring->spring.value);
    int32_t detent = (value - ring->min + ring->step / 2) / ring->step;
    if (detent == ring->detent) return;
    ring->detent = detent;
    /* Callback can delete the control. Keep this as the final operation. */
    ring->callback(ring->obj, LV_MIN(ring->max, ring->min + detent * ring->step), ring->callback_data);
}

static void ring_pointer_start(kit_ring_t *ring)
{
    lv_indev_t *input = lv_indev_active();
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_area_t area;
    lv_indev_get_point(input, &point);
    lv_obj_get_coords(ring->obj, &area);
    float x = point.x - (area.x1 + lv_obj_get_width(ring->obj) / 2);
    float y = point.y - (area.y1 + lv_obj_get_height(ring->obj) / 2);
    float angle = atan2f(y, x) * 180.f / (float)M_PI;
    float travel = ring->sweep > 0 ? angle - ring->start : ring->start - angle;
    travel = fmodf(travel + 720.f, 360.f);
    float sweep = LV_ABS(ring->sweep);
    if (travel > sweep) {
        float from_start = LV_MIN(travel, 360.f - travel);
        float from_end = LV_MIN(travel - sweep, 360.f - travel + sweep);
        travel = from_start <= from_end ? 0 : sweep;
    }
    int32_t value = (int32_t)lroundf(ring->min + travel * (ring->max - ring->min) / sweep);
    kit_ring_value_set(ring, value, false);
    /* LVGL's first PRESSING sample has zero elapsed time and otherwise cannot
     * leave the old value. A slider follows the initial absolute finger point.
     * Detents can delete the control; this must be the final operation. */
    ring_detent(ring);
}

void kit_ring_pointer_event(lv_event_t *event)
{
    kit_ring_t *ring = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_KEY || lv_event_get_code(event) == LV_EVENT_ROTARY) {
        kit_ring_value_set(ring, (int32_t)lroundf(ring->spring.target), false);
        return;
    }
    lv_indev_t *input = lv_indev_active();
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
    /* The native arc maps its integer atan2 approximation a second time.
     * Around six o'clock that can briefly emit 50 then 48 for the same finger
     * sample: a backwards panel write during an increasing drag. Own pointer
     * mapping once, before the class handler, using the exact shared angles.
     * Keys/encoders still use the native arc's normal VALUE_CHANGED path. */
    lv_event_stop_processing(event);
    ring_pointer_start(ring);
}

void kit_ring_event(lv_event_t *e)
{
    kit_ring_t *ring = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        if (ring->external_label && ring->value_label)
            lv_obj_remove_event_cb_with_user_data(ring->value_label, ring_label_event, ring);
        if (ring->level_title) lv_obj_delete(ring->level_title);
        orb_kit_motion_cancel(ring->obj, &s_ring_motion_key, false);
        orb_ui_free(ring);
    } else if (code == LV_EVENT_VALUE_CHANGED && ring->slider) {
        orb_kit_motion_cancel(ring->obj, &s_ring_motion_key, false);
        ring->moving = ring->arrival = false;
        orb_delight_spring_reset(&ring->spring, lv_arc_get_value(ring->obj));
        kit_ring_present(ring);
        ring_label_value(ring);
        ring_detent(ring);
    } else if (code == LV_EVENT_PRESSED) {
        /* Input owns the true value, never an unfinished arrival sample. */
        kit_ring_value_set(ring, (int32_t)lroundf(ring->spring.target), false);
        if (ring->slider) {
            knob_touch(ring->knob, ring->glass, true);
            ring_pointer_start(ring);
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && ring->slider) {
        knob_touch(ring->knob, ring->glass, false);
    } else if (code == LV_EVENT_REFR_EXT_DRAW_SIZE && ring->knob) {
        /* The 30px knob overhangs the 14px track by 8px. Without this the band
         * renderer skips the arc's subtree past its box and flattens the knob
         * at 6 o'clock (HIG §3: nothing clipped). */
        lv_event_set_ext_draw_size(e, ORB_KIT_KNOB / 2);
    }
}

lv_obj_t *kit_ring_create(lv_obj_t *parent, int32_t radius, int32_t width, lv_color_t accent)
{
    if (radius <= 0 || width <= 0 || width > radius) return NULL;
    kit_ring_t *ring = orb_ui_alloc(sizeof(*ring));
    if (!ring) return NULL;
    memset(ring, 0, sizeof(*ring));
    lv_obj_t *obj = lv_arc_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_size(obj, radius * 2 + width, radius * 2 + width);
    lv_obj_center(obj);
    lv_obj_set_style_arc_width(obj, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(obj, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(obj, accent, LV_PART_MAIN);
    lv_obj_set_style_arc_color(obj, accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(obj, 46, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(obj, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(obj, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(obj, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(obj, 0, LV_PART_KNOB);
    lv_arc_set_range(obj, 0, 1000);
    lv_arc_set_rotation(obj, 270);
    lv_arc_set_bg_angles(obj, 0, 360);
    lv_arc_set_value(obj, 0);
    ring->obj = obj;
    ring->accent = accent;
    ring->value_x = ORB_SCREEN_R;
    ring->value_y = ORB_SCREEN_R;
    ring->radius = radius;
    ring->start = 270;
    ring->sweep = 360;
    ring->max = 100;
    lv_obj_add_event_cb(obj, kit_ring_event, LV_EVENT_ALL, ring);
    return obj;
}

lv_obj_t *orb_kit_ring_create(lv_obj_t *parent, int32_t radius, int32_t width, lv_color_t accent)
{
    return kit_ring_create(parent, radius, width, accent);
}

void orb_kit_ring_set_value(lv_obj_t *obj, int32_t percent, bool animated)
{
    kit_ring_value_set(kit_event_state(obj, kit_ring_event), percent, animated);
}

static void ring_number_text(kit_ring_t *ring, const char *text)
{
    ring->number_text = false;
    if (!text || ring->formatter) return;
    const char *number = text;
    while (*number && !isdigit((unsigned char)*number) && *number != '-') number++;
    if (!*number || number - text >= (ptrdiff_t)sizeof(ring->number_prefix)) return;
    char *end;
    float value = strtof(number, &end);
    if (end == number || strlen(end) >= sizeof(ring->number_suffix)) return;
    for (const char *p = end; *p; p++) if (isdigit((unsigned char)*p)) return;
    unsigned decimals = 0;
    const char *point = strchr(number, '.');
    if (point && point < end) decimals = (unsigned)(end - point - 1);
    if (decimals > 2) return;
    memcpy(ring->number_prefix, text, (size_t)(number - text));
    ring->number_prefix[number - text] = 0;
    snprintf(ring->number_suffix, sizeof(ring->number_suffix), "%s", end);
    ring->number_factor = ring->spring.target ? value / ring->spring.target : 1.f;
    ring->number_decimals = decimals;
    ring->number_text = true;
}

void orb_kit_ring_set_value_label(lv_obj_t *obj, lv_obj_t *label, const char *reserve_template)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring || ring->value_label == label) return;
    if (ring->external_label && ring->value_label)
        lv_obj_remove_event_cb_with_user_data(ring->value_label, ring_label_event, ring);
    else if (ring->value_label) lv_obj_delete(ring->value_label);
    ring->value_label = label;
    ring->external_label = label != NULL;
    if (!label) return;
    lv_obj_add_event_cb(label, ring_label_event, LV_EVENT_DELETE, ring);
    snprintf(ring->target_text, sizeof(ring->target_text), "%s", lv_label_get_text(label));
    ring_number_text(ring, ring->target_text);
    orb_kit_value_set(label, ring->target_text, reserve_template);
    ring_label_value(ring);
}

void orb_kit_ring_set_text(lv_obj_t *obj, const char *text, const char *reserve_template)
{
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!ring) return;
    if (!ring->value_label) {
        ring->value_label = lv_label_create(obj);
        orb_text_apply(ring->value_label, ORB_TEXT_LARGE_TITLE, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 0);
        lv_obj_set_style_text_color(ring->value_label, ring->accent, 0);
        lv_obj_set_style_text_align(ring->value_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_clickable(ring->value_label, false);
        lv_obj_set_scrollable(ring->value_label, false);
    }
    snprintf(ring->target_text, sizeof(ring->target_text), "%s", text ? text : "");
    ring_number_text(ring, text);
    orb_kit_value_set(ring->value_label, text, reserve_template);
    if (ring->moving || ring->arrival) ring_label_value(ring);
    kit_ring_label_position(ring);
}

static const char s_switch_motion_key;

static bool switch_tick(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    kit_switch_t *control = context;
    if (finish || elapsed >= 500 || orb_delight_spring_settled(&control->spring))
        orb_delight_spring_reset(&control->spring, control->spring.target);
    else orb_delight_spring_step(&control->spring, dt / 1000.f, .28f, .14f);
    lv_obj_set_x(control->knob, (int32_t)lroundf(control->spring.value * control->scale));
    return !orb_delight_spring_settled(&control->spring);
}

static void switch_present(kit_switch_t *control)
{
    int32_t diameter = (int32_t)lroundf(38 * control->scale);
    lv_obj_set_size(control->knob, diameter, diameter);
    lv_obj_set_pos(control->knob, (int32_t)lroundf(control->spring.value * control->scale),
                   (int32_t)lroundf(4 * control->scale));
    lv_obj_set_style_bg_opa(control->obj, control->opacity, 0);
    lv_obj_set_style_bg_opa(control->knob, control->touched ? LV_OPA_TRANSP : control->opacity, 0);
    bool small = control->scale < .9f;
    orb_glass_set_amount(control->glass, control->touched && !small ? 1.f : 0.f);
    if (control->small_glass) orb_glass_set_amount(control->small_glass, control->touched && small ? 1.f : 0.f);
}

void orb_kit_switch_set_checked(lv_obj_t *obj, bool checked, bool animated)
{
    kit_switch_t *control = kit_event_state(obj, switch_event);
    if (!control) return;
    if (animated && control->checked == checked) return;
    control->checked = checked;
    orb_delight_spring_target(&control->spring, checked ? 38.f : 4.f);
    lv_obj_set_style_bg_color(obj, checked ? control->accent : ORB_GRAY5, 0);
    if (checked) lv_obj_add_state(obj, LV_STATE_CHECKED);
    else lv_obj_remove_state(obj, LV_STATE_CHECKED);
    if (!animated || orb_ui_reduce_motion()) {
        orb_kit_motion_cancel(obj, &s_switch_motion_key, false);
        orb_delight_spring_reset(&control->spring, control->spring.target);
        lv_obj_set_x(control->knob, (int32_t)lroundf(control->spring.value * control->scale));
    } else {
        orb_kit_motion_start(obj, &s_switch_motion_key, switch_tick, control);
    }
}

static void switch_event(lv_event_t *e)
{
    kit_switch_t *control = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        orb_kit_motion_cancel(control->obj, &s_switch_motion_key, false);
        orb_ui_free(control);
    } else if (code == LV_EVENT_CLICKED && !lv_obj_has_state(control->obj, LV_STATE_DISABLED)) {
        orb_kit_switch_set_checked(control->obj, !control->checked, true);
        lv_obj_send_event(control->obj, LV_EVENT_VALUE_CHANGED, NULL);
    } else if (code == LV_EVENT_PRESSED) {
        orb_kit_motion_cancel(control->obj, &s_switch_motion_key, true);
        control->touched = true;
        switch_present(control);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CANCEL) {
        control->touched = false;
        switch_present(control);
    }
}

lv_obj_t *orb_kit_switch_create(lv_obj_t *parent, bool checked, lv_color_t accent)
{
    kit_switch_t *control = orb_ui_alloc(sizeof(*control));
    if (!control) return NULL;
    memset(control, 0, sizeof(*control));
    lv_obj_t *obj = plain(parent);
    lv_obj_set_clickable(obj, true);
    lv_obj_set_size(obj, 80, 46);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, ORB_GRAY5, 0);
    control->obj = obj;
    control->accent = accent;
    control->scale = 1.f;
    control->opacity = LV_OPA_COVER;
    control->knob = kit_knob_create(obj, 38, &control->glass);
    lv_obj_set_y(control->knob, 4);
    orb_kit_press_apply(obj);
    lv_obj_add_event_cb(obj, switch_event, LV_EVENT_ALL, control);
    orb_kit_switch_set_checked(obj, checked, false);
    return obj;
}

bool orb_kit_switch_get_checked(lv_obj_t *obj)
{
    kit_switch_t *control = kit_event_state(obj, switch_event);
    return control && control->checked;
}

/* Kit rows (orb_kit_row.c) present a list's floating switch at the row's
 * current chord scale. The small glass is the 82% shoulder pose (HIG §2.2). */
void orb_kit_switch_row_glass(lv_obj_t *toggle)
{
    kit_switch_t *control = kit_event_state(toggle, switch_event);
    if (!control || control->small_glass) return;
    control->small_glass = plain(control->knob);
    lv_obj_set_size(control->small_glass, 31, 31);
    orb_glass_apply(control->small_glass, ORB_GLASS_REGULAR, lv_color_white());
    orb_glass_set_amount(control->small_glass, 0);
}

void orb_kit_switch_row_present(lv_obj_t *toggle, int32_t width, lv_opa_t opacity)
{
    kit_switch_t *control = kit_event_state(toggle, switch_event);
    if (!control) return;
    if (control->scale == width / 80.f && control->opacity == opacity) return;
    control->scale = width / 80.f;
    control->opacity = opacity;
    switch_present(control);
}
