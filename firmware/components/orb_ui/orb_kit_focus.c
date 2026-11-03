#include "orb_kit.h"

#include <math.h>
#include <stdlib.h>

typedef struct kit_focus_group kit_focus_group_t;
typedef struct kit_focus_member {
    lv_obj_t *obj;
    kit_focus_group_t *group;
    struct kit_focus_member *next;
    bool focused;
} kit_focus_member_t;

struct kit_focus_group {
    lv_obj_t *parent;

    kit_focus_member_t *members, *arrived, *departed, *destination;
    lv_area_t from, target;
    orb_delight_spring_t springs[4];
    uint32_t arrived_at, departed_at, previous, started;
    bool active, reduced;
};

static void focus_parent_event(lv_event_t *event);
static void focus_member_event(lv_event_t *event);

static void focus_cache_update(lv_obj_t *obj)
{
    lv_obj_t *row = lv_obj_get_parent(obj);
    lv_obj_t *list = row ? lv_obj_get_parent(row) : NULL;
    orb_round_list_cache_stats_t cache;
    if (list && orb_round_list_cache_stats(list, &cache)) orb_round_list_update(list);
}

static void focus_bounds(lv_obj_t *obj, lv_area_t *area)
{
    lv_obj_t *row = lv_obj_get_parent(obj);
    if (!row || !orb_round_list_child_bounds(row, obj, area)) lv_obj_get_coords(obj, area);
}

static lv_area_t focus_area(kit_focus_group_t *group)
{
    return (lv_area_t){lroundf(group->springs[0].value), lroundf(group->springs[1].value),
                       lroundf(group->springs[2].value), lroundf(group->springs[3].value)};
}

static void focus_finish(kit_focus_group_t *group)
{
    group->active = false;
    orb_kit_motion_cancel(group->parent, group, false);
    if (group->destination && group->destination->focused) {
        lv_obj_t *obj = group->destination->obj;
        lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, 0);
        focus_cache_update(obj);
    }
    group->destination = NULL;
    lv_obj_invalidate(group->parent);
}

static bool focus_tick(void *context, uint32_t elapsed, uint32_t dt_ms, bool finish)
{
    kit_focus_group_t *group = context;
    if (finish) { focus_finish(group); return false; }
    if (!group->destination || !group->destination->focused) { focus_finish(group); return false; }
    uint32_t now = lv_tick_get();
    if (orb_ui_reduce_motion() && !group->reduced) {
        group->from = focus_area(group);
        group->reduced = true;
        group->started = now;
    }
    focus_bounds(group->destination->obj, &group->target);
    if (group->reduced) {
        if (now - group->started >= 150) { focus_finish(group); return false; }
    } else {
        float target[] = {group->target.x1, group->target.y1, group->target.x2, group->target.y2};
        for (unsigned i = 0; i < 4; i++) {
            orb_delight_spring_target(&group->springs[i], target[i]);
            orb_delight_spring_step(&group->springs[i], dt_ms / 1000.f, .5f, 0);
        }
        bool settled = true;
        for (unsigned i = 0; i < 4; i++)
            settled &= fabsf(target[i] - group->springs[i].value) < .2f && fabsf(group->springs[i].velocity) < 2.f;
        if (settled) { focus_finish(group); return false; }
    }
    group->previous = now;
    lv_obj_invalidate(group->parent);
    return true;
}

static void focus_draw(lv_layer_t *layer, lv_area_t area, lv_opa_t opacity)
{
    if (!opacity) return;
    const int32_t extent = ORB_KIT_FOCUS_OFFSET + ORB_KIT_FOCUS_WIDTH;
    area.x1 -= extent; area.y1 -= extent; area.x2 += extent; area.y2 += extent;
    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.radius = LV_RADIUS_CIRCLE;
    descriptor.bg_opa = LV_OPA_TRANSP;
    descriptor.border_color = ORB_LABEL;
    descriptor.border_width = ORB_KIT_FOCUS_WIDTH;
    descriptor.border_opa = opacity;
    lv_draw_rect(layer, &descriptor, &area);
}

static void focus_parent_event(lv_event_t *event)
{
    kit_focus_group_t *group = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        for (kit_focus_member_t *member = group->members; member; member = member->next) member->group = NULL;
        orb_kit_motion_cancel(group->parent, group, false);
        lv_free(group);
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_DRAW_POST || !group->active) return;
    lv_layer_t *layer = lv_event_get_layer(event);
    if (group->reduced) {
        lv_opa_t opacity = LV_MIN(255, (lv_tick_get() - group->started) * 255 / 150);
        focus_draw(layer, group->from, 255 - opacity);
        focus_draw(layer, group->target, opacity);
    } else focus_draw(layer, focus_area(group), LV_OPA_COVER);
}

static lv_obj_t *focus_parent(lv_obj_t *obj)
{
    lv_obj_t *parent = lv_obj_get_parent(obj);
    lv_obj_t *list = parent ? lv_obj_get_parent(parent) : NULL;
    orb_round_list_cache_stats_t cache;
    if (list && (orb_round_list_cache_stats(list, &cache) ||
        (lv_obj_get_style_width(obj, 0) == ORB_KIT_DISC_ROW && lv_obj_get_style_height(parent, 0) >= ORB_KIT_ROW_H))) parent = list;
    return parent;
}

static kit_focus_group_t *focus_group(lv_obj_t *obj)
{
    lv_obj_t *parent = focus_parent(obj);
    if (!parent) return NULL;
    for (unsigned i = 0; i < lv_obj_get_event_count(parent); i++) {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(parent, i);
        if (lv_event_dsc_get_cb(descriptor) == focus_parent_event) return lv_event_dsc_get_user_data(descriptor);
    }
    kit_focus_group_t *group = lv_malloc_zeroed(sizeof(*group));
    if (!group) return NULL;
    group->parent = parent;
    orb_kit_motion_prepare(parent);
    if (!lv_obj_add_event_cb(parent, focus_parent_event, LV_EVENT_ALL, group)) {
        lv_free(group); return NULL;
    }
    return group;
}

static void focus_member_detach(kit_focus_member_t *member)
{
    kit_focus_group_t *group = member->group;
    if (!group) return;
    if (group->destination == member) focus_finish(group);
    if (group->arrived == member) group->arrived = NULL;
    if (group->departed == member) group->departed = NULL;
    kit_focus_member_t **slot = &group->members;
    while (*slot && *slot != member) slot = &(*slot)->next;
    if (*slot) *slot = member->next;
    member->group = NULL;
}

static void focus_member_event(lv_event_t *event)
{
    kit_focus_member_t *member = lv_event_get_user_data(event);
    focus_member_detach(member);
    lv_free(member);
}

static kit_focus_member_t *focus_member(lv_obj_t *obj, bool create)
{
    for (unsigned i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(descriptor) != focus_member_event) continue;
        kit_focus_member_t *member = lv_event_dsc_get_user_data(descriptor);
        if (member->group && member->group->parent == focus_parent(obj)) return member;
        /* Explicit reparenting can move a prepared control into a new group.
         * Keep its focus state and retire any old in-flight outline. */
        focus_member_detach(member);
        member->group = focus_group(obj);
        if (member->group) {
            member->next = member->group->members;
            member->group->members = member;
        }
        return member;
    }
    if (!create) return NULL;
    /* Registration only needs declared role/parent and style slots. Leave
     * geometry admission to the factory's existing press/layout preparation. */
    kit_focus_group_t *group = focus_group(obj);
    if (!group) return NULL;
    kit_focus_member_t *member = lv_malloc_zeroed(sizeof(*member));
    if (!member) return NULL;
    *member = (kit_focus_member_t){.obj = obj, .group = group, .next = group->members};
    if (!lv_obj_add_event_cb(obj, focus_member_event, LV_EVENT_DELETE, member)) { lv_free(member); return NULL; }
    group->members = member;
    /* All mutable style slots are admitted at construction. Focus transfer
     * and its animation can then change them without allocating LVGL payload. */
    lv_obj_set_style_outline_color(obj, ORB_LABEL, 0);
    lv_obj_set_style_outline_pad(obj, ORB_KIT_FOCUS_OFFSET, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, 0);
    return member;
}

static void focus_start(kit_focus_group_t *group)
{
    if (!group->arrived || !group->departed || group->arrived == group->departed ||
        !group->arrived->focused || abs((int32_t)(group->arrived_at - group->departed_at)) > 16) return;
    bool retarget = group->active;
    if (retarget && group->destination && group->destination->focused)
        lv_obj_set_style_outline_opa(group->destination->obj, LV_OPA_COVER, 0);
    group->destination = group->arrived;
    focus_bounds(group->destination->obj, &group->target);
    if (!retarget) {
        group->springs[0].value = group->from.x1; group->springs[1].value = group->from.y1;
        group->springs[2].value = group->from.x2; group->springs[3].value = group->from.y2;
        for (unsigned i = 0; i < 4; i++) group->springs[i].velocity = 0.f;
    } else group->from = focus_area(group);
    group->reduced = orb_ui_reduce_motion();
    group->started = group->previous = lv_tick_get();
    group->active = true;
    lv_obj_set_style_outline_opa(group->destination->obj, LV_OPA_TRANSP, 0);
    focus_cache_update(group->destination->obj);
    group->arrived = group->departed = NULL;
    if (group->reduced) orb_kit_motion_start_fade(group->parent, group, focus_tick, group);
    else orb_kit_motion_start(group->parent, group, focus_tick, group);
    lv_obj_invalidate(group->parent);
}

void orb_kit_focus_finish(lv_obj_t *parent)
{
    if (!parent) return;
    for (unsigned i = 0; i < lv_obj_get_event_count(parent); i++) {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(parent, i);
        if (lv_event_dsc_get_cb(descriptor) != focus_parent_event) continue;
        kit_focus_group_t *group = lv_event_dsc_get_user_data(descriptor);
        if (group->active) focus_finish(group);
        break;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); i++)
        orb_kit_focus_finish(lv_obj_get_child(parent, i));
}

void orb_kit_disc_set_focus(lv_obj_t *obj, bool focused)
{
    if (!obj) return;
    kit_focus_member_t *member = focus_member(obj, true);
    if (member && member->focused == focused) return;
    int32_t width = focused ? ORB_KIT_FOCUS_WIDTH : 0;
    if (!member && lv_obj_get_style_outline_width(obj, 0) == width) return;
    lv_obj_set_style_outline_color(obj, ORB_LABEL, 0);
    lv_obj_set_style_outline_pad(obj, ORB_KIT_FOCUS_OFFSET, 0);
    lv_obj_set_style_outline_width(obj, width, 0);
    lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, 0);
    if (member && member->group) {
        kit_focus_group_t *group = member->group;
        member->focused = focused;
        if (focused) { group->arrived = member; group->arrived_at = lv_tick_get(); }
        else {
            group->departed = member; group->departed_at = lv_tick_get();
            if (!group->active) focus_bounds(obj, &group->from);
        }
        focus_start(group);
    }
    focus_cache_update(obj);
}

void orb_kit_chip_set_selected(lv_obj_t *obj, bool selected)
{
    if (!obj) return;
    /* HIG §§5/7/8: selection is an outline, with semantic checked state,
     * rather than another tinted primary. It never changes the hit area. */
    if (selected) lv_obj_add_state(obj, LV_STATE_CHECKED);
    else lv_obj_remove_state(obj, LV_STATE_CHECKED);
    orb_kit_disc_set_focus(obj, selected);
}
