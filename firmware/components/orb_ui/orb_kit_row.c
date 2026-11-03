#include "orb_kit_row.h"
#include "orb_kit_value.h"
#include "orb_kit_effects.h"
#include "orb_ui_private.h"

#include <stdlib.h>
#include <string.h>

#include "src/misc/lv_area_private.h"
#include "src/misc/lv_text_private.h"

/* Kit rows and their lists, split from orb_kit_controls.c for its size guard
 * rail. HIG §§3/4/7: a row is at least 72 px; at the AX3 content size titles
 * and subtitles wrap to two lines and the row grows (watchOS wraps rather
 * than truncating at accessibility sizes), and the list restacks below it. */
#define ROW_LIST_GAP 12

static void row_event(lv_event_t *e);
static void row_list_event(lv_event_t *e);
static void row_overlay_update(kit_row_t *state);

static void *event_state(lv_obj_t *obj, lv_event_cb_t callback)
{
    if (!obj) return NULL;
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(descriptor) == callback) return lv_event_dsc_get_user_data(descriptor);
    }
    return NULL;
}

kit_row_t *orb_kit_row_state(lv_obj_t *row)
{
    return event_state(row, row_event);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    return obj;
}

/* ---- List flow ---------------------------------------------------------------------- */

static void flow_event(lv_event_t *e)
{
    orb_ui_free(lv_event_get_user_data(e));
}

static kit_flow_t *flow_find(lv_obj_t *child, bool create)
{
    kit_row_t *state = event_state(child, row_event);
    if (state) {
        if (!state->flowed && !create) return NULL;
        state->flowed = true;
        return &state->flow;
    }
    kit_flow_t *flow = event_state(child, flow_event);
    if (flow || !create) return flow;
    flow = orb_ui_alloc(sizeof(*flow));
    if (!flow) return NULL;
    *flow = (kit_flow_t){0};
    if (!lv_obj_add_event_cb(child, flow_event, LV_EVENT_DELETE, flow)) {
        orb_ui_free(flow);
        return NULL;
    }
    return flow;
}

typedef struct {
    lv_obj_t *obj;
    int32_t design, style, delta;
    unsigned index;
} kit_flow_item_t;

static int flow_compare(const void *a, const void *b)
{
    /* Child order breaks ties, so the restack is deterministic. */
    const kit_flow_item_t *x = a, *y = b;
    if (x->design != y->design) return x->design < y->design ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

static void grow_event(lv_event_t *e)
{
    orb_ui_free(lv_event_get_user_data(e));
}

/* A plain (non-scrolling) stack sized for 84 px steps, such as a reader's
 * choices, grows with its rows instead of clipping the last ones. The growth
 * is recorded like a push, so a return to 72 px rows restores its height. */
static void row_stack_grow(lv_obj_t *list, int32_t shift, int32_t bottom)
{
    int32_t style = lv_obj_get_style_height(list, 0);
    if (lv_obj_is_scrollable(list) || style <= 0 || style >= LV_COORD_MAX) return;
    kit_flow_t *grow = event_state(list, grow_event);
    int32_t applied = grow && grow->placed_style == style ? grow->delta : 0;
    /* Pushed rows move the stack's end; a last row that grew in place
     * overflows it. Either way the stack keeps its trailing row gap. */
    shift = LV_MAX(shift, bottom + ROW_LIST_GAP - (style - applied));
    shift = LV_MAX(0, shift);
    if (!grow && !shift) return;
    if (!grow) {
        grow = orb_ui_alloc(sizeof(*grow));
        if (!grow) return;
        *grow = (kit_flow_t){0};
        if (!lv_obj_add_event_cb(list, grow_event, LV_EVENT_DELETE, grow)) { orb_ui_free(grow); return; }
    }
    int32_t height = style - applied + shift;
    if (height != style) lv_obj_set_height(list, height);
    *grow = (kit_flow_t){.delta = shift, .placed_style = height};
}

/* Callers place rows at fixed 84 px steps or below the previous child. A row
 * that grew pushes everything after it down until the 12 px row gap holds;
 * the push is recorded, so a later return to 72 px rows restores the caller's
 * own positions. Lists whose rows all stay 72 px never move. */
static void row_restack(lv_obj_t *list)
{
    if (!list) return;
    lv_obj_update_layout(list);
    unsigned count = lv_obj_get_child_count(list);
    kit_flow_item_t *items = count ? orb_ui_alloc(count * sizeof(*items)) : NULL;
    if (!items) return;
    unsigned used = 0;
    for (unsigned i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        if (lv_obj_is_floating(child)) continue;
        kit_flow_t *flow = flow_find(child, false);
        int32_t style = lv_obj_get_style_y(child, 0);
        int32_t delta = flow && flow->placed_style == style ? flow->delta : 0;
        items[used++] = (kit_flow_item_t){child, lv_obj_get_y(child) - delta, style, delta, i};
    }
    qsort(items, used, sizeof(*items), flow_compare);
    int32_t shift = 0, floor = INT32_MIN, bottom = 0;
    for (unsigned i = 0; i < used; i++) {
        kit_flow_item_t *item = &items[i];
        int32_t y = item->design + shift;
        if (y < floor) { shift += floor - y; y = floor; }
        int32_t style = item->style - item->delta + shift;
        kit_flow_t *flow = flow_find(item->obj, shift != 0);
        if (style != item->style) lv_obj_set_y(item->obj, style);
        if (flow) *flow = (kit_flow_t){.delta = shift, .placed_style = style};
        int32_t height = lv_obj_get_height(item->obj);
        bool grown = event_state(item->obj, row_event) && height > ORB_KIT_ROW_H && !lv_obj_is_hidden(item->obj);
        floor = grown ? y + height + ROW_LIST_GAP : INT32_MIN;
        if (grown) bottom = LV_MAX(bottom, y + height);
    }
    orb_ui_free(items);
    row_stack_grow(list, shift, bottom);
}

/* ---- Rows ----------------------------------------------------------------------------- */

static void row_live_sync(kit_row_t *state)
{
    lv_obj_t *labels[] = {state->title, state->subtitle, state->value};
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *live = state->live[i], *anchor = labels[i];
        if (!live || !anchor) continue;
        lv_obj_set_style_text_font(live, lv_obj_get_style_text_font(anchor, 0), 0);
        lv_obj_set_style_text_letter_space(live, lv_obj_get_style_text_letter_space(anchor, 0), 0);
        lv_obj_set_style_text_align(live, lv_obj_get_style_text_align(anchor, 0), 0);
        lv_obj_set_style_max_width(live, lv_obj_get_width(anchor), 0);
        orb_kit_value_set(live, lv_label_get_text(live), NULL);
    }
    row_overlay_update(state);
}

static void row_fonts(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)subject;
    lv_obj_t *row = lv_observer_get_target_obj(observer);
    kit_row_t *state = event_state(row, row_event);
    if (state) {
        orb_kit_row_layout(row, state);
        row_restack(state->list);
        row_live_sync(state);
    }
}

static void row_switch_changed(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_user_data(e);
    lv_obj_send_event(row, LV_EVENT_VALUE_CHANGED, NULL);
}

static void row_disc_overlay(kit_row_t *state, const lv_area_t *parent)
{
    if (!state->live_disc) return;
    lv_area_t bounds, row_bounds;
    bool visible = orb_round_list_child_bounds(state->row, state->disc, &bounds) &&
        orb_round_list_row_bounds(state->row, &row_bounds) &&
        bounds.x1 >= row_bounds.x1 && bounds.x2 <= row_bounds.x2 &&
        bounds.y1 >= row_bounds.y1 && bounds.y2 <= row_bounds.y2;
    if (lv_obj_is_hidden(state->live_disc) == visible) lv_obj_set_hidden(state->live_disc, !visible);
    if (!visible) return;
    lv_opa_t opacity = (unsigned)orb_round_list_row_opacity(state->row) * lv_obj_get_style_opa(state->disc, 0) / 255;
    if (lv_obj_get_style_opa(state->live_disc, 0) != opacity)
        lv_obj_set_style_opa(state->live_disc, opacity, 0);
    int32_t width = lv_area_get_width(&bounds), height = lv_area_get_height(&bounds);
    if (lv_obj_get_width(state->live_disc) != width || lv_obj_get_height(state->live_disc) != height)
        lv_obj_set_size(state->live_disc, width, height);
    int32_t x = bounds.x1 - parent->x1, y = bounds.y1 - parent->y1;
    if (lv_obj_get_x(state->live_disc) != x || lv_obj_get_y(state->live_disc) != y)
        lv_obj_set_pos(state->live_disc, x, y);
    int32_t scale = LV_MIN(width, height) * 256 / ORB_KIT_DISC_ROW;
    if (lv_obj_get_style_transform_scale_x(state->live_glyph, 0) != scale)
        lv_obj_set_style_transform_scale(state->live_glyph, scale, 0);
    lv_obj_update_layout(state->live_glyph);
    int32_t pivot_x = lv_obj_get_style_transform_pivot_x(state->live_glyph, 0);
    int32_t pivot_y = lv_obj_get_style_transform_pivot_y(state->live_glyph, 0);
    x = (width - lv_obj_get_width(state->live_glyph) * scale / 256) / 2 - pivot_x * (256 - scale) / 256;
    y = (height - lv_obj_get_height(state->live_glyph) * scale / 256) / 2 - pivot_y * (256 - scale) / 256;
    if (lv_obj_get_x(state->live_glyph) != x || lv_obj_get_y(state->live_glyph) != y)
        lv_obj_set_pos(state->live_glyph, x, y);
    orb_kit_disc_light_up_pose(state->live_disc, scale);
}

static void row_overlay_update(kit_row_t *state)
{
    if (!state->list) return;
    lv_area_t parent;
    lv_obj_get_coords(state->list, &parent);
    /* Floating overlays do not scroll, but LVGL still offsets them by the
     * list's padding. Lists that rest their first row in the focus lane use
     * pad_top; without this, live values and switches landed a row low. */
    int32_t border = lv_obj_get_style_border_width(state->list, LV_PART_MAIN);
    parent.x1 += lv_obj_get_style_pad_left(state->list, LV_PART_MAIN) + border;
    parent.y1 += lv_obj_get_style_pad_top(state->list, LV_PART_MAIN) + border;
    lv_obj_t *labels[] = {state->title, state->subtitle, state->value};
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *live = state->live[i], *anchor = labels[i], *clip = state->live_clip[i];
        if (!live || !anchor || !clip) continue;
        lv_area_t bounds, row_bounds;
        bool visible = orb_round_list_child_bounds(state->row, anchor, &bounds) &&
            orb_round_list_row_bounds(state->row, &row_bounds);
        lv_area_t clipped;
        if (!visible || !lv_area_intersect(&clipped, &bounds, &row_bounds)) {
            state->live_resume[i] = false;
            if (!lv_obj_is_hidden(clip)) orb_kit_motion_finish(clip);
            lv_obj_set_hidden(clip, true);
            continue;
        }
        /* Flexible text rails shrink independently of the row's scale. Using
         * their width ratio would shrink readable numbers into invisibility.
         * Match the row height instead and clip native ink to its public lane;
         * AX3 leading may extend past the capsule while glyph ink still fits. */
        int32_t scale = LV_MIN(256, lv_area_get_height(&row_bounds) * 256 / lv_obj_get_height(state->row));
        const lv_font_t *font = lv_obj_get_style_text_font(live, 0);
        if (font->line_height * scale < 20 * 256) {
            state->live_resume[i] = false;
            if (!lv_obj_is_hidden(clip)) orb_kit_motion_finish(clip);
            lv_obj_set_hidden(clip, true); continue;
        }
        /* The immutable chord contains only a transparent lane. Scale native
         * live ink with that lane; no label text/font, raster or row source is
         * rebuilt while the list travels or a value changes. */
        bool resumed = lv_obj_is_hidden(clip);
        lv_obj_set_hidden(clip, false);
        if (lv_obj_get_style_transform_scale_x(live, 0) != scale)
            lv_obj_set_style_transform_scale(live, scale, 0);
        /* The lane can be narrower than the label at the shoulders: end the
         * text there with an ellipsis rather than hard-clipping a glyph. */
        orb_kit_value_fit(live, LV_MIN(lv_obj_get_width(live), lv_area_get_width(&bounds) * 256 / LV_MAX(1, scale)));
        lv_obj_set_pos(clip, clipped.x1 - parent.x1, clipped.y1 - parent.y1);
        lv_obj_set_size(clip, lv_area_get_width(&clipped), lv_area_get_height(&clipped));
        int32_t x = bounds.x1 - clipped.x1, y = bounds.y1 - clipped.y1;
        if (lv_obj_get_style_text_align(live, 0) == LV_TEXT_ALIGN_RIGHT)
            x = bounds.x2 + 1 - lv_obj_get_width(live) * scale / 256 - clipped.x1;
        if (lv_obj_get_x(live) != x || lv_obj_get_y(live) != y) lv_obj_set_pos(live, x, y);
        state->live_resume[i] |= resumed;
        if (state->live_resume[i] && lv_obj_is_visible(live)) {
            state->live_resume[i] = false; orb_kit_breathe_resume(live);
        }
    }
    row_disc_overlay(state, &parent);
    if (!state->toggle) return;
    lv_area_t bounds, row_bounds;
    if (!orb_round_list_child_bounds(state->row, state->anchor, &bounds) ||
        !orb_round_list_row_bounds(state->row, &row_bounds) ||
        bounds.x1 < row_bounds.x1 || bounds.x2 > row_bounds.x2 ||
        bounds.y1 < row_bounds.y1 || bounds.y2 > row_bounds.y2) {
        lv_obj_set_hidden(state->toggle, true);
        return;
    }
    int32_t width = lv_area_get_width(&bounds), height = lv_area_get_height(&bounds);
    lv_obj_set_hidden(state->toggle, false);
    lv_obj_set_size(state->toggle, width, height);
    lv_obj_set_pos(state->toggle, bounds.x1 - parent.x1, bounds.y1 - parent.y1);
    orb_kit_switch_row_present(state->toggle, width,
        lv_obj_has_state(state->toggle, LV_STATE_DISABLED) ? ORB_KIT_DISABLED_OPA :
        width < 72 ? LV_OPA_50 : LV_OPA_COVER);
}

/* Deferred viewport sources can become ready after the final scroll event.
 * Read their published pose at refresh start, before layout/draw, without
 * preparing a source or allocating anything in the refresh callback. */
static void row_overlay_frame(lv_event_t *e)
{
    kit_row_t *state = lv_event_get_user_data(e);
    if (state->list && lv_obj_is_visible(state->list)) row_overlay_update(state);
}

static void row_overlay_observe(kit_row_t *state)
{
    if (state->overlay_display) return;
    lv_display_t *display = lv_obj_get_display(state->row);
    if (lv_display_add_event_cb(display, row_overlay_frame, LV_EVENT_REFR_START, state))
        state->overlay_display = display;
}

static void row_list_event(lv_event_t *e)
{
    kit_row_t *state = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        state->list = NULL;
    } else if (code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_BEGIN ||
               code == LV_EVENT_LAYOUT_CHANGED || code == LV_EVENT_SCROLL_END) {
        if (code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_BEGIN) {
            orb_kit_disc_light_up_cancel(state->live_disc);
            for (unsigned i = 0; i < 3; i++) orb_kit_value_roll_finish(state->live[i]);
        }
        if (state->toggle && (code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_BEGIN) &&
            !orb_kit_press_is_cancelled(state->toggle)) orb_kit_press_cancel(state->toggle);
        row_overlay_update(state);
    }
}

static void row_toggle_deleted(lv_event_t *e)
{
    kit_row_t *state = lv_event_get_user_data(e);
    state->toggle = NULL;
}

static void row_event(lv_event_t *e)
{
    kit_row_t *state = lv_event_get_user_data(e);
    lv_obj_t *row = lv_event_get_current_target(e);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        if (state->list) lv_obj_remove_event_cb_with_user_data(state->list, row_list_event, state);
        if (state->overlay_display)
            lv_display_remove_event_cb_with_user_data(state->overlay_display, row_overlay_frame, state);
        if (state->toggle) lv_obj_delete(state->toggle);
        if (state->live_disc) lv_obj_delete(state->live_disc);
        for (unsigned i = 0; i < 3; i++) if (state->live_clip[i]) lv_obj_delete(state->live_clip[i]);
        for (unsigned i = 0; i < 3; i++) lv_free(state->live_template[i]);
        orb_ui_free(state);
    } else if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        orb_kit_disc_light_up_cancel(state->live_disc);
        for (unsigned i = 0; i < 3; i++) orb_kit_value_roll_finish(state->live[i]);
    } else if (lv_event_get_code(e) == LV_EVENT_CLICKED && state->toggle &&
               !lv_obj_has_state(row, LV_STATE_DISABLED)) {
        orb_kit_switch_set_checked(state->toggle, !orb_kit_switch_get_checked(state->toggle), true);
        lv_obj_send_event(row, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

lv_obj_t *orb_kit_row_create(lv_obj_t *list, const orb_kit_row_config_t *config)
{
    if (!list || !config) return NULL;
    lv_obj_update_layout(list);
    kit_row_t *state = orb_ui_alloc(sizeof(*state));
    if (!state) return NULL;
    memset(state, 0, sizeof(*state));
    int32_t y = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        if (lv_obj_is_floating(child)) continue;
        kit_flow_t *flow = flow_find(child, false);
        int32_t delta = flow && flow->placed_style == lv_obj_get_style_y(child, 0) ? flow->delta : 0;
        int32_t height = event_state(child, row_event) ? ORB_KIT_ROW_H : lv_obj_get_height(child);
        y = LV_MAX(y, lv_obj_get_y(child) - delta + height + ROW_LIST_GAP);
    }
    lv_obj_t *row = plain(list);
    state->row = row;
    state->list = list;
    lv_obj_set_size(row, 418, ORB_KIT_ROW_H);
    lv_obj_set_pos(row, (lv_obj_get_width(list) - 418) / 2, y);
    lv_obj_set_clickable(row, true);
    /* The visible row owns taps, while the shell still sees edge contacts to
     * arbitrate rim/list gestures. Cancellation prevents a scroll from acting. */
    lv_obj_set_event_bubble(row, true);
    lv_obj_set_style_radius(row, ORB_KIT_ROW_H / 2, 0);
    lv_obj_set_style_bg_color(row, ORB_GRAY6, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_t *disc = orb_kit_disc_create(row, ORB_KIT_DISC_KIND_ROW, ORB_KIT_ACCENT_FILL,
                                        config->accent, config->glyph ? config->glyph : "");
    lv_obj_set_clickable(disc, false);
    state->disc = disc;
    lv_obj_set_pos(disc, 16, (ORB_KIT_ROW_H - ORB_KIT_DISC_ROW) / 2);
    state->title = lv_label_create(row);
    orb_text_apply(state->title, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(state->title, lv_color_white(), 0);
    lv_label_set_text(state->title, config->title ? config->title : "");
    lv_label_set_long_mode(state->title, LV_LABEL_LONG_MODE_DOTS);
    if (config->subtitle && config->subtitle[0]) {
        state->subtitle = lv_label_create(row);
        orb_text_apply(state->subtitle, ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR, 0);
        lv_obj_set_style_text_color(state->subtitle, ORB_KIT_SECONDARY, 0);
        lv_obj_set_style_text_opa(state->subtitle, ORB_KIT_SECONDARY_OPA, 0);
        lv_label_set_text(state->subtitle, config->subtitle);
        lv_label_set_long_mode(state->subtitle, LV_LABEL_LONG_MODE_DOTS);
    }
    orb_kit_press_apply(row);
    lv_obj_add_event_cb(row, row_event, LV_EVENT_ALL, state);
    if (config->show_switch) {
        /* HIG §§1/7: keep immutable row ink in the chord cache; the touched
         * glass/travelling knob lives above it. A transparent aligned anchor
         * participates in the existing trailing-rail geometry and hit map. */
        state->anchor = plain(row);
        lv_obj_set_size(state->anchor, 80, 46);
        lv_obj_align(state->anchor, LV_ALIGN_RIGHT_MID, -28, 0);
        /* List switches use system green; identity stays on the leading disc. */
        state->toggle = orb_kit_switch_create(list, config->checked, ORB_KIT_PLAY);
        if (state->toggle) {
            /* Reserve floating pose properties before any refresh hook. */
            lv_obj_set_pos(state->toggle, 0, 0);
            /* Lists have two prepared presentation sizes (100% and82%).
             * Standalone switches need only the native glass geometry. */
            orb_kit_switch_row_glass(state->toggle);
            state->trailing = 108;
            lv_obj_set_floating(state->toggle, true);
            lv_obj_set_ignore_layout(state->toggle, true);
            lv_obj_set_hidden(state->toggle, true);
            lv_obj_add_event_cb(state->toggle, row_switch_changed, LV_EVENT_VALUE_CHANGED, row);
            lv_obj_add_event_cb(state->toggle, row_toggle_deleted, LV_EVENT_DELETE, state);
            lv_obj_add_event_cb(list, row_list_event, LV_EVENT_ALL, state);
            row_overlay_observe(state);
        }
    } else if (config->value) {
        state->value = lv_label_create(row);
        orb_text_apply(state->value, ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR, 0);
        lv_obj_set_style_text_color(state->value, ORB_KIT_SECONDARY, 0);
        lv_obj_set_style_text_opa(state->value, ORB_KIT_SECONDARY_OPA, 0);
        lv_obj_set_style_text_align(state->value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(state->value, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(state->value, config->value);
        lv_obj_align(state->value, LV_ALIGN_RIGHT_MID, -28, 0);
    }
    orb_kit_row_layout(row, state);
    /* Record growth from the ordinary 72 px cadence even when the list is
     * first created at AX3, so reducing text size restores the same rhythm. */
    row_restack(list);
    lv_subject_add_observer_obj(orb_fonts_subject(), row_fonts, row, NULL);
    return row;
}

void orb_kit_row_set_value(lv_obj_t *row, const char *value)
{
    orb_kit_row_set_text(row, ORB_KIT_ROW_TEXT_VALUE, value);
}

static void row_live_deleted(lv_event_t *e)
{
    kit_row_t *state = lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    for (unsigned i = 0; i < 3; i++) if (state->live[i] == obj) state->live[i] = NULL;
}

static void row_live_clip_deleted(lv_event_t *e)
{
    kit_row_t *state = lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    for (unsigned i = 0; i < 3; i++) if (state->live_clip[i] == obj) state->live_clip[i] = NULL;
}

static void row_live_text_set(lv_obj_t *row, orb_kit_row_text_role_t role,
                              const char *text, const char *reserve_template, bool rolling)
{
    kit_row_t *state = event_state(row, row_event);
    if (!state || !state->list || role > ORB_KIT_ROW_TEXT_VALUE) return;
    lv_obj_t **labels[] = {&state->title, &state->subtitle, &state->value};
    lv_obj_t *anchor = *labels[role];
    if (!anchor) {
        anchor = lv_label_create(row);
        *labels[role] = anchor;
        orb_text_apply(anchor, role == ORB_KIT_ROW_TEXT_TITLE ? ORB_TEXT_HEADLINE : ORB_TEXT_CAPTION2,
                       ORB_TEXT_TABULAR, 0);
        lv_obj_set_style_text_color(anchor, ORB_KIT_SECONDARY, 0);
        lv_obj_set_style_text_align(anchor, role == ORB_KIT_ROW_TEXT_VALUE ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    }
    bool native_text = text == lv_label_get_text(anchor);
    bool created = !state->live[role];
    if (created) {
        bool observed = state->toggle || state->live_disc || state->live[0] || state->live[1] || state->live[2];
        state->live_clip[role] = plain(state->list);
        lv_obj_set_pos(state->live_clip[role], 0, 0);
        lv_obj_set_size(state->live_clip[role], 1, 1);
        lv_obj_set_floating(state->live_clip[role], true);
        lv_obj_set_ignore_layout(state->live_clip[role], true);
        lv_obj_set_hidden(state->live_clip[role], true);
        lv_obj_add_event_cb(state->live_clip[role], row_live_clip_deleted, LV_EVENT_DELETE, state);
        state->live[role] = lv_label_create(state->live_clip[role]);
        orb_kit_breathe_prepare(state->live[role]);
        orb_kit_value_roll_prepare(state->live[role]);
        lv_obj_set_clickable(state->live[role], false);
        lv_obj_set_pos(state->live[role], 0, 0);
        lv_obj_set_style_transform_scale(state->live[role], 256, 0);
        /* A native live label can cross a render band while scaled at the
         * shoulder. Keep its glyph sampling phase independent of that crop. */
        lv_label_set_stable_transform(state->live[role], true);
        lv_obj_set_style_transform_pivot_x(state->live[role], 0, 0);
        lv_obj_set_style_transform_pivot_y(state->live[role], 0, 0);
        lv_obj_add_event_cb(state->live[role], row_live_deleted, LV_EVENT_DELETE, state);
        if (!observed) lv_obj_add_event_cb(state->list, row_list_event, LV_EVENT_ALL, state);
        row_overlay_observe(state);
        if (role == ORB_KIT_ROW_TEXT_TITLE) state->title_variant |= ORB_TEXT_TABULAR;
        else if (role == ORB_KIT_ROW_TEXT_SUBTITLE) state->subtitle_variant |= ORB_TEXT_TABULAR;
        else state->value_variant |= ORB_TEXT_TABULAR;
    }
    lv_obj_t *live = state->live[role];
    const char *reservation = reserve_template ? reserve_template : state->live_template[role];
    if (!reservation) reservation = text ? text : "";
    bool configured = created || !state->live_template[role] || strcmp(state->live_template[role], reservation);
    if (configured) {
        char *copy = lv_malloc(strlen(reservation) + 1);
        if (!copy) return;
        strcpy(copy, reservation);
        lv_free(state->live_template[role]);
        state->live_template[role] = copy;
        reservation = copy;
        if (native_text) text = copy;
        lv_label_set_text(anchor, reservation);
        orb_kit_row_layout(row, state);
        lv_obj_set_style_text_font(live, lv_obj_get_style_text_font(anchor, 0), 0);
        lv_obj_set_style_text_letter_space(live, lv_obj_get_style_text_letter_space(anchor, 0), 0);
        lv_obj_set_style_text_color(live, lv_obj_get_style_text_color(anchor, 0), 0);
        lv_obj_set_style_text_opa(live, role == ORB_KIT_ROW_TEXT_TITLE ? LV_OPA_COVER : ORB_KIT_SECONDARY_OPA, 0);
        lv_obj_set_style_text_align(live, lv_obj_get_style_text_align(anchor, 0), 0);
        lv_obj_set_width(live, lv_obj_get_width(anchor));
        lv_obj_set_style_max_width(live, lv_obj_get_width(anchor), 0);
        /* Only this one-time lane change touches the prepared row source. */
        lv_obj_set_style_text_opa(anchor, LV_OPA_TRANSP, 0);
        /* A new reservation can move a trailing value under its title at AX3.
         * Grow the shared flow before publishing this changed row source. */
        row_restack(state->list);
        orb_round_list_update(state->list);
    }
    if (rolling) orb_kit_value_set_rolling(live, text, reserve_template);
    else orb_kit_value_set(live, text, reserve_template);
    if (configured) row_live_sync(state);
    else row_overlay_update(state);
}

void orb_kit_row_set_live_text(lv_obj_t *row, orb_kit_row_text_role_t role,
                              const char *text, const char *reserve_template)
{
    row_live_text_set(row, role, text, reserve_template, false);
}

void orb_kit_row_set_live_text_rolling(lv_obj_t *row, orb_kit_row_text_role_t role,
                                      const char *text, const char *reserve_template)
{
    row_live_text_set(row, role, text, reserve_template, true);
}

void orb_kit_row_set_live_value(lv_obj_t *row, const char *value, const char *reserve_template)
{
    orb_kit_row_set_live_text(row, ORB_KIT_ROW_TEXT_VALUE, value, reserve_template);
}

void orb_kit_row_set_text(lv_obj_t *row, orb_kit_row_text_role_t role, const char *text)
{
    kit_row_t *state = event_state(row, row_event);
    if (!state || role > ORB_KIT_ROW_TEXT_VALUE) return;
    if (state->live[role]) { orb_kit_row_set_live_text(row, role, text, NULL); return; }
    lv_obj_t *labels[] = {state->title, state->subtitle, state->value};
    if (!labels[role]) return;
    text = text ? text : "";
    if (!strcmp(lv_label_get_text(labels[role]), text)) return;
    lv_label_set_text(labels[role], text);
    orb_kit_row_layout(row, state);
    row_restack(state->list);
    row_live_sync(state);
    orb_round_list_update(state->list);
}

static void row_live_disc_deleted(lv_event_t *e)
{
    kit_row_t *state = lv_event_get_user_data(e);
    state->live_disc = state->live_glyph = NULL;
}

void orb_kit_row_set_live_accent(lv_obj_t *row, lv_color_t fill, lv_color_t ink)
{
    kit_row_t *state = event_state(row, row_event);
    if (!state || !state->list) return;
    if (!state->live_disc) {
        bool observed = state->toggle || state->live[0] || state->live[1] || state->live[2];
        lv_obj_t *glyph = lv_obj_get_child(state->disc, 0);
        state->live_disc = plain(state->list);
        lv_obj_set_pos(state->live_disc, 0, 0);
        lv_obj_set_size(state->live_disc, ORB_KIT_DISC_ROW, ORB_KIT_DISC_ROW);
        lv_obj_set_style_radius(state->live_disc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(state->live_disc, fill, 0);
        lv_obj_set_style_bg_opa(state->live_disc, LV_OPA_COVER, 0);
        lv_obj_set_floating(state->live_disc, true);
        lv_obj_set_ignore_layout(state->live_disc, true);
        lv_obj_set_hidden(state->live_disc, true);
        lv_obj_add_event_cb(state->live_disc, row_live_disc_deleted, LV_EVENT_DELETE, state);
        state->live_glyph = lv_label_create(state->live_disc);
        lv_obj_set_clickable(state->live_glyph, false);
        orb_icon_apply(state->live_glyph, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED, 0);
        lv_label_set_text(state->live_glyph, glyph ? lv_label_get_text(glyph) : "");
        lv_obj_set_style_text_color(state->live_glyph, ink, 0);
        /* Keep glyph replacements in one owned, reserved lane too. All Home
         * state variants are three-byte symbols; a change never allocates. */
        orb_kit_value_set(state->live_glyph, glyph ? lv_label_get_text(glyph) : "", NULL);
        lv_obj_set_style_transform_scale(state->live_glyph, 256, 0);
        lv_obj_set_style_transform_pivot_x(state->live_glyph, 0, 0);
        lv_obj_set_style_transform_pivot_y(state->live_glyph, 0, 0);
        lv_label_set_stable_transform(state->live_glyph, true);
        /* HIG §§1/5: native state ink sits over a transparent immutable
         * anchor, so a light turning on never recaches or restacks its row. */
        lv_obj_set_style_bg_opa(state->disc, LV_OPA_TRANSP, 0);
        orb_kit_disc_set_ink_opa(state->disc, LV_OPA_TRANSP);
        if (!observed) lv_obj_add_event_cb(state->list, row_list_event, LV_EVENT_ALL, state);
        row_overlay_observe(state);
        orb_round_list_update(state->list);
    }
    orb_kit_disc_light_up_prepare(state->live_disc);
    orb_kit_disc_light_up_cancel(state->live_disc);
    if (!lv_color_eq(lv_obj_get_style_bg_color(state->live_disc, 0), fill))
        lv_obj_set_style_bg_color(state->live_disc, fill, 0);
    if (!lv_color_eq(lv_obj_get_style_text_color(state->live_glyph, 0), ink))
        lv_obj_set_style_text_color(state->live_glyph, ink, 0);
    row_overlay_update(state);
}

bool orb_kit_row_set_text_role(lv_obj_t *row, orb_kit_row_text_role_t role,
                                orb_text_variant_t variant)
{
    kit_row_t *state = event_state(row, row_event);
    if (!state || role > ORB_KIT_ROW_TEXT_VALUE) return false;
    if (state->live[role]) variant |= ORB_TEXT_TABULAR;
    if (role == ORB_KIT_ROW_TEXT_TITLE) state->title_variant = variant;
    else if (role == ORB_KIT_ROW_TEXT_SUBTITLE) state->subtitle_variant = variant;
    else state->value_variant = variant;
    orb_kit_row_layout(row, state);
    row_restack(state->list);
    row_live_sync(state);
    orb_round_list_update(lv_obj_get_parent(row));
    return true;
}

void orb_kit_row_wrap_title(lv_obj_t *row, orb_text_variant_t variant)
{
    kit_row_t *state = event_state(row, row_event);
    if (!state) return;
    /* Time's existing opt-in uses the same layout and restacking as automatic
     * AX growth. No second wrapper mutates title text or pushes rows twice. */
    state->wrap_title = true;
    orb_kit_row_set_text_role(row, ORB_KIT_ROW_TEXT_TITLE, variant);
}

void orb_kit_row_show_full_title(lv_obj_t *row, orb_text_variant_t variant)
{
    kit_row_t *state = event_state(row, row_event);
    if (!state) return;
    state->full_title = true;
    orb_kit_row_wrap_title(row, variant);
}

lv_obj_t *orb_kit_row_switch(lv_obj_t *row)
{
    kit_row_t *state = event_state(row, row_event);
    return state ? state->toggle : NULL;
}

void orb_kit_row_set_enabled(lv_obj_t *row, bool enabled)
{
    kit_row_t *state = event_state(row, row_event);
    if (!state) return;
    bool changed = enabled == lv_obj_has_state(row, LV_STATE_DISABLED);
    lv_obj_t *objects[] = {row, state->toggle};
    for (unsigned i = 0; i < sizeof(objects) / sizeof(*objects); i++) {
        lv_obj_t *object = objects[i];
        if (!object) continue;
        if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
        else {
            orb_kit_press_cancel(object);
            lv_obj_add_state(object, LV_STATE_DISABLED);
        }
    }
    if (!changed) return;
    /* HIG §8 / polish review #12: an unavailable row must look unavailable.
     * Dim its ink, identity disc and switch; the row keeps its place. */
    lv_opa_t primary = enabled ? LV_OPA_COVER : ORB_KIT_DISABLED_OPA;
    lv_opa_t secondary = enabled ? ORB_KIT_SECONDARY_OPA : ORB_KIT_DISABLED_OPA;
    lv_obj_set_style_text_opa(state->title, state->live[0] ? LV_OPA_TRANSP : primary, 0);
    if (state->subtitle) lv_obj_set_style_text_opa(state->subtitle, state->live[1] ? LV_OPA_TRANSP : secondary, 0);
    if (state->value) lv_obj_set_style_text_opa(state->value, state->live[2] ? LV_OPA_TRANSP : secondary, 0);
    for (unsigned i = 0; i < 3; i++)
        if (state->live[i]) orb_kit_live_ink_opa(state->live[i], i ? secondary : primary);
    lv_obj_set_style_opa(state->disc, primary, 0);
    if (state->toggle || state->live_disc) row_overlay_update(state);
    /* The chord cache holds the row's ink; rebuild it once while idle. */
    orb_round_list_update(state->list);
}

void orb_kit_list_refresh(lv_obj_t *list)
{
    if (!list) return;
    lv_obj_update_layout(list);
    orb_round_list_update(list);
    for (unsigned i = 0; i < lv_obj_get_child_count(list); i++) {
        kit_row_t *state = event_state(lv_obj_get_child(list, i), row_event);
        if (state && (state->toggle || state->live_disc || state->live[0] || state->live[1] || state->live[2])) row_overlay_update(state);
    }
    lv_obj_send_event(list, LV_EVENT_REFRESH, NULL);
}

bool orb_kit_list_apply(lv_obj_t *list)
{
    if (!list) return false;
    row_restack(list);
    bool result = orb_round_list_apply(list, &(orb_round_list_config_t){
        .margin = 24, .fade = 38, .stepped = true, .chord_fit = true, .visible_cache = true,
    });
    for (unsigned i = 0; i < lv_obj_get_child_count(list); i++) {
        kit_row_t *state = event_state(lv_obj_get_child(list, i), row_event);
        if (state && (state->toggle || state->live_disc || state->live[0] || state->live[1] || state->live[2])) row_overlay_update(state);
    }
    /* Explicit preparation publishes coordinates immediately for hit testing
     * and iris anchors; scroll callbacks only update the prepared styles. */
    lv_obj_update_layout(list);
    return result;
}

void orb_kit_row_set_live_glyph(lv_obj_t *row, const char *glyph)
{
    kit_row_t *state = event_state(row, row_event);
    if (!state || !state->live_glyph) return;
    orb_kit_value_set(state->live_glyph, glyph, NULL);
    row_overlay_update(state);
}
