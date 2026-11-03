#include "orb_kit.h"
#include "orb_ui_private.h"

#include <math.h>
#include <string.h>

/* The right-rim scroll position indicator (HIG §7): a 4px secondary track at
 * radius 224 with a proportional white thumb. Split from orb_kit_controls.c
 * so that file's size ratchet can absorb the level state API. */
typedef struct kit_scroll {
    struct kit_scroll *next;
    lv_obj_t *list, *arc;
    orb_delight_spring_t width;
    uint32_t changed;
    int32_t top;
    bool busy, arrival_flash, pending, input;
} kit_scroll_t;

static void scroll_event(lv_event_t *e);
static kit_scroll_t *s_scrolls;

static void *event_state(lv_obj_t *obj, lv_event_cb_t callback)
{
    if (!obj) return NULL;
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(descriptor) == callback) return lv_event_dsc_get_user_data(descriptor);
    }
    return NULL;
}

static void scroll_angles(lv_obj_t *arc, float start, float end)
{
    /* Match LVGL's precise-value conversion and its inclusive 360 endpoint.
     * Its arc setter invalidates the knob area even when neither angle moves,
     * so repeated list events must stop here before reaching that setter. */
    lv_value_precise_t first = start, last = end;
    while (first > 360) first -= 360;
    while (last > 360) last -= 360;
    while (first < 0) first += 360;
    while (last < 0) last += 360;
    if (lv_arc_get_angle_start(arc) != first || lv_arc_get_angle_end(arc) != last)
        lv_arc_set_angles(arc, first, last);
}

static const char s_scroll_motion_key;

static void scroll_breath_present(kit_scroll_t *scroll, unsigned opacity, int32_t width)
{
    lv_obj_t *arc = scroll->arc;
    if (lv_obj_get_style_opa(arc, 0) == opacity &&
        lv_obj_get_style_arc_width(arc, LV_PART_INDICATOR) == width) return;
    /* Only the right-rim segment contains ink. Arc style setters otherwise
     * dirty its 452px square for a four-pixel opacity/width change. */
    lv_display_t *display = lv_obj_get_display(arc);
    bool invalidating = lv_display_is_invalidation_enabled(display);
    lv_display_enable_invalidation(display, false);
    lv_obj_set_style_opa(arc, (lv_opa_t)opacity, 0);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_display_enable_invalidation(display, invalidating);
    lv_area_t area;
    lv_obj_get_coords(arc, &area);
    int32_t cx = (area.x1 + area.x2 + 1) / 2, cy = (area.y1 + area.y2 + 1) / 2;
    area = (lv_area_t){cx + ORB_KIT_SCROLL_RADIUS - 24, cy - 66,
                       cx + ORB_KIT_SCROLL_RADIUS + 6, cy + 66};
    lv_obj_invalidate_area(arc, &area);
}

static bool scroll_tick(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    LV_UNUSED(elapsed);
    kit_scroll_t *scroll = context;
    uint32_t quiet = lv_tick_elaps(scroll->changed);
    if (finish || quiet >= (scroll->arrival_flash ? 100u : 600u)) {
        orb_delight_spring_reset(&scroll->width, 4);
        scroll->busy = scroll->input = false;
        scroll_breath_present(scroll, 0, 4);
        return false;
    }
    if (scroll->arrival_flash) {
        unsigned opacity = quiet <= 50 ? 255 : 255 * (100 - quiet) / 50;
        scroll_breath_present(scroll, opacity, 4);
        return true;
    }
    orb_delight_spring_target(&scroll->width, quiet < 80 ? 6 : 4);
    orb_delight_spring_step(&scroll->width, dt / 1000.f, .25f, .15f);
    /* The indicator is gone 600 ms after the last scroll sample; the 150 ms
     * fade stays within that bound instead of adding a late repaint. */
    unsigned opacity = quiet <= 450 ? 255 : 255 * (600 - quiet) / 150;
    scroll_breath_present(scroll, opacity, (int32_t)lroundf(scroll->width.value));
    return true;
}

static void scroll_activate(kit_scroll_t *scroll, bool arrival)
{
    if (orb_ui_reduce_motion()) {
        orb_kit_motion_cancel(scroll->arc, &s_scroll_motion_key, false);
        orb_delight_spring_reset(&scroll->width, 4);
        scroll->busy = scroll->input = false;
        scroll_breath_present(scroll, 0, 4);
        return;
    }
    bool changing = scroll->busy && scroll->arrival_flash != arrival;
    scroll->changed = lv_tick_get();
    scroll->arrival_flash = arrival;
    scroll_breath_present(scroll, 255, (int32_t)lroundf(scroll->width.value));
    if (!scroll->busy || changing) {
        scroll->busy = true;
        if (arrival) orb_kit_motion_start(scroll->arc, &s_scroll_motion_key, scroll_tick, scroll);
        else orb_kit_motion_start_feedback(scroll->arc, &s_scroll_motion_key, scroll_tick, scroll);
    }
}

static void scroll_arrived(void *context, bool fresh)
{
    kit_scroll_t *scroll = context;
    scroll->pending = false;
    if (fresh) scroll_activate(scroll, true);
    else scroll_tick(scroll, 0, 0, true);
}

void orb_kit_scroll_arc_arrival_prepare(lv_obj_t *list)
{
    kit_scroll_t *scroll = event_state(list, scroll_event);
    if (!scroll || !scroll->arc) return;
    orb_kit_arrival_complete(scroll->arc, false);
    orb_kit_motion_cancel(scroll->arc, &s_scroll_motion_key, true);
    orb_delight_spring_reset(&scroll->width, 4);
    scroll->busy = scroll->input = false;
    scroll->pending = !lv_obj_is_hidden(scroll->arc);
    scroll_breath_present(scroll, 0, 4);
    /* The same fence as other arrivals owns timeout, cover and touch
     * fallback. A pending indicator cannot survive and replay on Back. */
    if (scroll->pending) orb_kit_arrival_prepare(scroll->arc, scroll_arrived, scroll);
}

void orb_kit_scroll_arc_input(lv_obj_t *list)
{
    kit_scroll_t *scroll = event_state(list, scroll_event);
    if (!scroll || !scroll->arc) return;
    orb_kit_arrival_complete(scroll->arc, false);
    scroll->input = true;
    scroll_activate(scroll, false);
}

static void scroll_update(kit_scroll_t *scroll, bool activity)
{
    if (!scroll || !scroll->arc) return;
    lv_obj_t *list = scroll->list;
    int32_t top = LV_MAX(0, lv_obj_get_scroll_top(list));
    int32_t bottom = LV_MAX(0, lv_obj_get_scroll_bottom(list));
    int32_t visible = lv_obj_get_content_height(list);
    int32_t extent = top + bottom;
    bool moved = top != scroll->top;
    scroll->top = top;
    /* The sibling shares ancestor visibility and travel with its list.
     * An offscreen navigation preview must retain its destination indicator. */
    if (lv_obj_is_hidden(list) || extent <= 0 || visible <= 0) {
        lv_obj_set_hidden(scroll->arc, true);
        return;
    }
    lv_obj_set_hidden(scroll->arc, false);
    float size = LV_MAX(4.f, 30.f * visible / (visible + extent));
    float start = 345.f + (30.f - size) * top / extent;
    scroll_angles(scroll->arc, start, start + size);
    if (activity && moved && scroll->input) scroll_activate(scroll, false);
    lv_area_t parent;
    lv_obj_get_coords(lv_obj_get_parent(scroll->arc), &parent);
    int32_t outer = ORB_KIT_SCROLL_RADIUS + 2;
    lv_obj_set_pos(scroll->arc, ORB_SCREEN_R - outer - parent.x1,
                   ORB_SCREEN_R - outer - parent.y1);
}

static void scroll_arc_event(lv_event_t *e)
{
    kit_scroll_t *scroll = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        orb_kit_motion_cancel(scroll->arc, &s_scroll_motion_key, false);
        scroll->arc = NULL;
        scroll->busy = false;
    }
}

static void scroll_event(lv_event_t *e)
{
    kit_scroll_t *scroll = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        kit_scroll_t **slot = &s_scrolls;
        while (*slot && *slot != scroll) slot = &(*slot)->next;
        if (*slot) *slot = scroll->next;
        if (scroll->arc) lv_obj_delete(scroll->arc);
        orb_ui_free(scroll);
    } else if (code == LV_EVENT_SCROLL_BEGIN) {
        /* LVGL also emits BEGIN for raw programmatic restoration. Only a
         * native pointer owner or the explicit rim API admits feedback. */
        lv_indev_t *input = lv_indev_active();
        if (input && lv_indev_get_scroll_obj(input) == scroll->list)
            orb_kit_scroll_arc_input(scroll->list);
        scroll_update(scroll, false);
    } else if (code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_END || code == LV_EVENT_REFRESH ||
               code == LV_EVENT_LAYOUT_CHANGED || code == LV_EVENT_SIZE_CHANGED) {
        scroll_update(scroll, code == LV_EVENT_SCROLL);
    }
}

static lv_obj_t *scroll_arc_create(lv_obj_t *parent)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_scrollable(arc, false);
    lv_obj_set_floating(arc, true);
    lv_obj_set_ignore_layout(arc, true);
    lv_obj_set_size(arc, 2 * (ORB_KIT_SCROLL_RADIUS + 2), 2 * (ORB_KIT_SCROLL_RADIUS + 2));
    lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, ORB_KIT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, 64, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, 230, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(arc, 345, 15);
    lv_obj_center(arc);
    return arc;
}

void orb_kit_scroll_arc_set_position(lv_obj_t *obj, unsigned position, unsigned count)
{
    if (!obj) return;
    lv_obj_set_hidden(obj, count <= 1);
    if (count <= 1) return;
    position = LV_MIN(position, count - 1);
    float size = LV_MAX(4.f, 30.f / count);
    float start = 345.f + (30.f - size) * position / (count - 1);
    scroll_angles(obj, start, start + size);
}

lv_obj_t *orb_kit_scroll_arc_create(lv_obj_t *parent, unsigned position, unsigned count)
{
    lv_obj_t *arc = scroll_arc_create(parent);
    orb_kit_scroll_arc_set_position(arc, position, count);
    return arc;
}

lv_obj_t *orb_kit_scroll_arc_attach(lv_obj_t *list)
{
    if (!list || !lv_obj_get_parent(list)) return NULL;
    kit_scroll_t *existing = event_state(list, scroll_event);
    if (existing && existing->arc) return existing->arc;
    kit_scroll_t *scroll = existing ? existing : orb_ui_alloc(sizeof(*scroll));
    if (!scroll) return NULL;
    if (!existing) memset(scroll, 0, sizeof(*scroll));
    orb_delight_spring_reset(&scroll->width, 4);
    scroll->pending = true;
    scroll->input = scroll->busy = false;
    scroll->list = list;
    scroll->arc = scroll_arc_create(lv_obj_get_parent(list));
    /* Attached indicators use absolute display-centred offsets, including
     * lists inside an inset card. Keep those offsets independent of the
     * standalone paged indicator's parent-centred alignment. */
    lv_obj_set_align(scroll->arc, LV_ALIGN_TOP_LEFT);
    if (!existing) {
        scroll->next = s_scrolls;
        s_scrolls = scroll;
        lv_obj_add_event_cb(list, scroll_event, LV_EVENT_ALL, scroll);
    }
    lv_obj_add_event_cb(scroll->arc, scroll_arc_event, LV_EVENT_DELETE, scroll);
    lv_obj_set_style_opa(scroll->arc, LV_OPA_TRANSP, 0);
    orb_kit_motion_prepare(scroll->arc);
    lv_obj_update_layout(list);
    scroll_update(scroll, false);
    orb_kit_scroll_arc_arrival_prepare(list);
    return scroll->arc;
}
