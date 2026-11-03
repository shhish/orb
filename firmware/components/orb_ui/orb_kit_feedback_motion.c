#include "orb_kit_effects.h"
#include "orb_kit_motion.h"
#include "orb_kit_value.h"

#include <math.h>
#include "src/core/lv_obj_event_private.h"

typedef struct {
    lv_obj_t *obj;
    orb_delight_spring_t spring;
    lv_opa_t opacity;
    int32_t scale, click_pad, pivot_x, pivot_y;
    bool glass, active, advanced, hit_testing, stable;
} kit_pop_t;

typedef struct {
    lv_obj_t *obj;
    lv_color_t colour;
    float progress;
    bool deny, label;
    lv_opa_t opacity;
} kit_check_t;

typedef struct {
    lv_obj_t *label;
    uint32_t started, sampled;
    lv_opa_t opacity;
    bool waiting, active;
} kit_breathe_t;

static void pop_hit_test(kit_pop_t *pop, lv_event_t *event)
{
    if (!pop->active || pop->hit_testing) return;
    lv_hit_test_info_t *hit = lv_event_get_hit_test_info(event);
    lv_point_t point = *hit->point;
    /* indev inversely transformed the point before its early rectangle test.
     * Undo only the visual transform so the original hit rectangle survives. */
    lv_obj_transform_point(pop->obj, &point, LV_OBJ_POINT_TRANSFORM_FLAG_NONE);
    lv_area_t bounds;
    lv_obj_get_coords(pop->obj, &bounds);
    bounds.x1 -= pop->click_pad; bounds.y1 -= pop->click_pad;
    bounds.x2 += pop->click_pad; bounds.y2 += pop->click_pad;
    hit->res = point.x >= bounds.x1 && point.x <= bounds.x2 &&
        point.y >= bounds.y1 && point.y <= bounds.y2;
    if (hit->res && pop->advanced) {
        /* A caller's shape test receives its original coordinate space too.
         * The recursive event is guarded; no pointer action is dispatched. */
        lv_hit_test_info_t shape = {.point = &point, .res = true};
        pop->hit_testing = true;
        lv_obj_send_event(pop->obj, LV_EVENT_HIT_TEST, &shape);
        pop->hit_testing = false;
        hit->res = shape.res;
    }
}

static void pop_delete(lv_event_t *event)
{
    kit_pop_t *pop = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_HIT_TEST) pop_hit_test(pop, event);
    else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        orb_kit_motion_cancel(pop->obj, pop, false);
        lv_free(pop);
    } else if (lv_event_get_code(event) == LV_EVENT_PRESSED ||
               lv_event_get_code(event) == LV_EVENT_SCROLL_BEGIN)
        orb_kit_motion_cancel(pop->obj, pop, true);
}

static bool pop_frame(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    kit_pop_t *pop = context;
    if (!finish && elapsed < 350) orb_delight_spring_step(&pop->spring, dt / 1000.f, .35f, .15f);
    bool done = finish || elapsed >= 350;
    float p = done ? 1.f : pop->spring.value;
    lv_obj_set_style_transform_scale(pop->obj, (int32_t)lroundf(pop->scale * (.8f + .2f * p)), 0);
    /* HIG §9: glass develops its material and never alpha-fades. Ordinary
     * content may fade within the unchanged control/hit/layout rectangle. */
    if (pop->glass) orb_glass_set_amount(pop->obj, LV_CLAMP(.001f, p, 1.f));
    else lv_obj_set_style_opa(pop->obj, (lv_opa_t)lroundf(pop->opacity * LV_CLAMP(0.f, p, 1.f)), 0);
    /* The spring may pass its target before its perceptual duration. Keep
     * its velocity until that boundary instead of clipping away the bounce. */
    pop->active = !done;
    if (!pop->active) {
        lv_obj_set_ext_click_area(pop->obj, pop->click_pad);
        lv_obj_set_adv_hittest(pop->obj, pop->advanced);
        lv_obj_set_stable_transform(pop->obj, pop->stable);
        lv_obj_set_style_transform_pivot_x(pop->obj, pop->pivot_x, 0);
        lv_obj_set_style_transform_pivot_y(pop->obj, pop->pivot_y, 0);
    }
    return pop->active;
}

static kit_pop_t *pop_prepare(lv_obj_t *obj)
{
    if (!obj || lv_obj_has_state(obj, LV_STATE_PRESSED)) return NULL;
    kit_pop_t *pop = NULL;
    for (unsigned i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(event) == pop_delete) { pop = lv_event_dsc_get_user_data(event); break; }
    }
    if (pop && pop->active) return NULL;
    if (!pop) {
        pop = lv_malloc_zeroed(sizeof(*pop));
        if (!pop) return NULL;
        pop->obj = obj;
        lv_obj_add_event_cb(obj, pop_delete, LV_EVENT_ALL, pop);
    } else orb_kit_motion_cancel(obj, pop, true);
    lv_obj_update_layout(obj);
    pop->opacity = lv_obj_get_style_opa(obj, 0);
    pop->scale = lv_obj_get_style_transform_scale_x(obj, 0);
    pop->pivot_x = lv_obj_get_style_transform_pivot_x(obj, 0);
    pop->pivot_y = lv_obj_get_style_transform_pivot_y(obj, 0);
    pop->glass = orb_glass_set_amount(obj, 1);
    lv_area_t click, bounds;
    lv_obj_get_click_area(obj, &click);
    lv_obj_get_coords(obj, &bounds);
    pop->click_pad = bounds.x1 - click.x1;
    pop->advanced = lv_obj_is_adv_hittest(obj);
    pop->stable = lv_obj_get_stable_transform(obj);
    lv_obj_set_stable_transform(obj, true);
    /* Permit the inverse-scaled outer edge through LVGL's early area reject;
     * the advanced test then restores exactly the original target. */
    lv_obj_set_ext_click_area(obj, pop->click_pad + LV_MAX(lv_obj_get_width(obj), lv_obj_get_height(obj)) / 8 + 2);
    lv_obj_set_adv_hittest(obj, true);
    lv_obj_set_style_transform_pivot_x(obj, lv_obj_get_width(obj) / 2, 0);
    lv_obj_set_style_transform_pivot_y(obj, lv_obj_get_height(obj) / 2, 0);
    pop->active = true;
    orb_delight_spring_reset(&pop->spring, 0);
    orb_delight_spring_target(&pop->spring, 1);
    pop_frame(pop, 0, 0, false);
    return pop;
}

void orb_kit_pop_in(lv_obj_t *obj)
{
    if (!orb_kit_motion_allowed(obj)) return;
    kit_pop_t *pop = pop_prepare(obj);
    if (pop) orb_kit_motion_start(obj, pop, pop_frame, pop);
}

static void pop_origin(kit_pop_t *pop, int32_t origin_x, int32_t origin_y)
{
    lv_area_t box;
    lv_obj_get_coords(pop->obj, &box);
    /* A remote visual pivot expands out from the finger. Layout coordinates
     * and the advanced hit test remain the original full-size target. */
    lv_obj_set_style_transform_pivot_x(pop->obj, origin_x - box.x1, 0);
    lv_obj_set_style_transform_pivot_y(pop->obj, origin_y - box.y1, 0);
    int32_t reach = LV_MAX(LV_MAX(LV_ABS(box.x1 - origin_x), LV_ABS(box.x2 - origin_x)),
                          LV_MAX(LV_ABS(box.y1 - origin_y), LV_ABS(box.y2 - origin_y)));
    /* The 80% visual scale inversely moves an edge by at most one quarter
     * of its distance from the pivot. The advanced test still rejects every
     * point outside the original target, including this temporary margin. */
    lv_obj_set_ext_click_area(pop->obj, pop->click_pad + (reach + pop->click_pad + 3) / 4 + 2);
}

void orb_kit_pop_in_from(lv_obj_t *obj, int32_t origin_x, int32_t origin_y)
{
    if (!orb_kit_motion_allowed(obj)) return;
    kit_pop_t *pop = pop_prepare(obj);
    if (!pop) return;
    pop_origin(pop, origin_x, origin_y);
    orb_kit_motion_start(obj, pop, pop_frame, pop);
}

static void pop_arrival(void *context, bool animate)
{
    kit_pop_t *pop = context;
    if (animate) orb_kit_motion_start(pop->obj, pop, pop_frame, pop);
    else pop_frame(pop, 0, 0, true);
}

void orb_kit_pop_in_arrival(lv_obj_t *obj)
{
    if (!obj || orb_ui_reduce_motion()) return;
    kit_pop_t *pop = pop_prepare(obj);
    if (pop) orb_kit_arrival_prepare(obj, pop_arrival, pop);
}

void orb_kit_pop_in_arrival_from(lv_obj_t *obj, int32_t origin_x, int32_t origin_y)
{
    if (!obj || orb_ui_reduce_motion()) return;
    kit_pop_t *pop = pop_prepare(obj);
    if (!pop) return;
    pop_origin(pop, origin_x, origin_y);
    orb_kit_arrival_prepare(obj, pop_arrival, pop);
}

static bool check_frame(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    LV_UNUSED(dt);
    kit_check_t *check = context;
    check->progress = finish ? 1.f : LV_MIN(1.f, elapsed / 280.f);
    if (check->label)
        lv_obj_set_style_text_opa(check->obj, check->progress >= 1 ? check->opacity : LV_OPA_TRANSP, 0);
    lv_obj_invalidate(check->obj);
    return check->progress < 1.f;
}

static void check_event(lv_event_t *event)
{
    kit_check_t *check = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        orb_kit_motion_cancel(check->obj, check, false);
        lv_free(check);
        return;
    }
    if (check->label && check->progress >= 1) return;
    lv_area_t box;
    lv_obj_get_coords(check->obj, &box);
    if (check->label) {
        int32_t size = LV_MIN(lv_area_get_width(&box), lv_area_get_height(&box));
        int32_t centre = (box.x1 + box.x2) / 2;
        box.x1 = centre - size / 2;
        box.x2 = box.x1 + size - 1;
    }
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = check->colour;
    dsc.width = 4;
    dsc.round_start = dsc.round_end = true;
    int32_t x = box.x1, y = box.y1;
    lv_point_precise_t points[3] = {{x + 4, y + 17}, {x + 13, y + 26}, {x + 33, y + 5}};
    if (check->label) {
        float sx = lv_area_get_width(&box) / 38.f, sy = lv_area_get_height(&box) / 34.f;
        for (unsigned i = 0; i < 3; i++) {
            points[i].x = x + (points[i].x - x) * sx;
            points[i].y = y + (points[i].y - y) * sy;
        }
        dsc.width = LV_MAX(2, (int32_t)lroundf(4 * LV_MIN(sx, sy)));
    }
    if (check->deny) {
        points[0] = (lv_point_precise_t){x + 6, y + 6};
        points[1] = (lv_point_precise_t){x + 29, y + 29};
        points[2] = (lv_point_precise_t){x + 6, y + 29};
    }
    for (unsigned i = 0; i < 2; i++) {
        float part = LV_CLAMP(0.f, check->progress * 2 - i, 1.f);
        if (part <= 0) continue;
        dsc.p1 = check->deny && i == 1 ? (lv_point_precise_t){x + 29, y + 6} : points[i];
        lv_point_precise_t end = points[i + 1];
        dsc.p2.x = dsc.p1.x + (end.x - dsc.p1.x) * part;
        dsc.p2.y = dsc.p1.y + (end.y - dsc.p1.y) * part;
        lv_draw_line(lv_event_get_layer(event), &dsc);
    }
}

lv_obj_t *orb_kit_check_create(lv_obj_t *parent, lv_color_t colour, bool deny)
{
    kit_check_t *check = lv_malloc_zeroed(sizeof(*check));
    if (!check) return NULL;
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, 38, 34);
    lv_obj_set_pos(obj, 4, 4);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    *check = (kit_check_t){.obj = obj, .colour = colour, .deny = deny, .progress = 1};
    lv_obj_add_event_cb(obj, check_event, LV_EVENT_DELETE, check);
    lv_obj_add_event_cb(obj, check_event, LV_EVENT_DRAW_MAIN, check);
    if (orb_kit_motion_allowed(obj)) {
        check->progress = 0;
        orb_kit_motion_start(obj, check, check_frame, check);
    }
    return obj;
}

static void check_arrival(void *context, bool animate)
{
    kit_check_t *check = context;
    check->progress = animate ? 0 : 1;
    if (animate) orb_kit_motion_start(check->obj, check, check_frame, check);
    else check_frame(check, 0, 0, true);
}

void orb_kit_check_arrival(lv_obj_t *obj)
{
    if (!obj) return;
    kit_check_t *check = NULL;
    for (unsigned i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(event) == check_event) { check = lv_event_dsc_get_user_data(event); break; }
    }
    if (!check) return;
    orb_kit_motion_cancel(obj, check, false);
    check->progress = 0;
    orb_kit_arrival_prepare(obj, check_arrival, check);
}

static kit_check_t *check_label_prepare(lv_obj_t *label)
{
    if (!label) return NULL;
    kit_check_t *check = NULL;
    for (unsigned i = 0; i < lv_obj_get_event_count(label); i++) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(label, i);
        if (lv_event_dsc_get_cb(event) == check_event) { check = lv_event_dsc_get_user_data(event); break; }
    }
    if (!check) {
        check = lv_malloc_zeroed(sizeof(*check));
        if (!check) return NULL;
        *check = (kit_check_t){.obj = label, .label = true,
            .colour = lv_obj_get_style_text_color(label, 0),
            .opacity = lv_obj_get_style_text_opa(label, 0)};
        lv_obj_add_event_cb(label, check_event, LV_EVENT_DELETE, check);
        lv_obj_add_event_cb(label, check_event, LV_EVENT_DRAW_MAIN, check);
    }
    return check;
}

void orb_kit_check_label_draw_on(lv_obj_t *label)
{
    if (!orb_kit_motion_allowed(label)) return;
    kit_check_t *check = check_label_prepare(label);
    if (!check) return;
    check->progress = 0;
    lv_obj_set_style_text_opa(label, LV_OPA_TRANSP, 0);
    orb_kit_motion_start(label, check, check_frame, check);
}

void orb_kit_check_label_arrival(lv_obj_t *label)
{
    kit_check_t *check = check_label_prepare(label);
    if (!check) return;
    lv_obj_set_style_text_opa(label, LV_OPA_TRANSP, 0);
    orb_kit_check_arrival(label);
}

static void breathe_delete(lv_event_t *event)
{
    kit_breathe_t *breath = lv_event_get_user_data(event);
    orb_kit_motion_cancel(breath->label, breath, false);
    lv_free(breath);
}

static kit_breathe_t *breathe_find(lv_obj_t *label)
{
    if (!label || !lv_obj_check_type(label, &lv_label_class)) return NULL;
    for (unsigned i = 0; i < lv_obj_get_event_count(label); i++) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(label, i);
        if (lv_event_dsc_get_cb(event) == breathe_delete) return lv_event_dsc_get_user_data(event);
    }
    return NULL;
}

void orb_kit_live_ink_opa(lv_obj_t *label, lv_opa_t opacity)
{
    if (!label) return;
    bool drawing_check = false;
    for (unsigned i = 0; i < lv_obj_get_event_count(label); i++) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(label, i);
        if (lv_event_dsc_get_cb(event) != check_event) continue;
        kit_check_t *check = lv_event_dsc_get_user_data(event);
        if (check->label) { check->opacity = opacity; drawing_check = check->progress < 1; }
        break;
    }
    kit_breathe_t *breath = breathe_find(label);
    if (breath && breath->waiting) breath->opacity = opacity;
    bool rolling = orb_kit_value_roll_set_ink_opa(label, opacity);
    /* Model refreshes may restate a row's colour while its ink is animated.
     * Update the final opacity without exposing the native label mid-roll or
     * resetting the currently sampled waiting breath. */
    if (!drawing_check && !rolling && !(breath && breath->active))
        lv_obj_set_style_text_opa(label, opacity, 0);
}

void orb_kit_breathe_prepare(lv_obj_t *label)
{
    if (!label || !lv_obj_check_type(label, &lv_label_class)) return;
    for (unsigned i = 0; i < lv_obj_get_event_count(label); i++)
        if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(label, i)) == breathe_delete) return;
    kit_breathe_t *breath = lv_malloc_zeroed(sizeof(*breath));
    if (!breath) return;
    breath->label = label;
    lv_obj_add_event_cb(label, breathe_delete, LV_EVENT_DELETE, breath);
    orb_kit_motion_prepare(label);
}

static bool breathe_frame(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    LV_UNUSED(elapsed);
    LV_UNUSED(dt);
    kit_breathe_t *breath = context;
    uint32_t age = lv_tick_get() - breath->started;
    if (finish || !breath->waiting || age >= 30000 || !lv_obj_is_visible(breath->label)) {
        breath->active = false;
        lv_obj_set_style_text_opa(breath->label, breath->opacity, 0);
        return false;
    }
    /* Literal 100%-50%-100% avoids double-dimming secondary wait ink (HIG §5).
     * One full cycle takes 1.6 s; only this named wait may
     * request repeat frames, capped to ten style changes per virtual second. */
    if (age - breath->sampled >= 100) {
        breath->sampled = age;
        float opacity = .75f + .25f * cosf(2.f * (float)M_PI * (age % 1600) / 1600.f);
        lv_obj_set_style_text_opa(breath->label, (lv_opa_t)lroundf(LV_OPA_COVER * opacity), 0);
    }
    return true;
}

static void breathe_begin(kit_breathe_t *breath)
{
    if (breath->active || lv_tick_get() - breath->started >= 30000 ||
        !lv_obj_is_visible(breath->label) || !orb_kit_motion_allowed(breath->label)) return;
    breath->active = true;
    lv_obj_set_style_text_opa(breath->label, LV_OPA_COVER, 0);
    orb_kit_motion_start_wait_until(breath->label, breath, breathe_frame, breath, breath->started + 30000);
}

void orb_kit_breathe_since(lv_obj_t *label, bool waiting, uint32_t started_ms)
{
    if (!label || !lv_obj_check_type(label, &lv_label_class)) return;
    kit_breathe_t *breath = breathe_find(label);
    if (!breath && !waiting) return;
    if (!breath) {
        orb_kit_breathe_prepare(label);
        breath = breathe_find(label);
        if (!breath) return;
    }
    if (!waiting) {
        breath->waiting = false;
        orb_kit_motion_cancel(label, breath, true);
        return;
    }
    /* Restating the same model wait keeps its clock and 30 s deadline.
     * Visibility admission resumes a quieted wait explicitly. */
    if (breath->waiting && breath->started == started_ms) return;
    orb_kit_motion_cancel(label, breath, true);
    breath->waiting = true;
    breath->started = started_ms;
    breath->sampled = 0;
    breath->opacity = lv_obj_get_style_text_opa(label, 0);
    breathe_begin(breath);
}

void orb_kit_breathe(lv_obj_t *label, bool waiting)
{
    kit_breathe_t *breath = breathe_find(label);
    uint32_t started = breath && breath->waiting ? breath->started : lv_tick_get();
    orb_kit_breathe_since(label, waiting, started);
}

void orb_kit_breathe_resume(lv_obj_t *parent)
{
    if (!parent) return;
    kit_breathe_t *breath = breathe_find(parent);
    if (breath && breath->waiting && !breath->active)
        breathe_begin(breath);
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); i++)
        orb_kit_breathe_resume(lv_obj_get_child(parent, i));
}

void orb_kit_material_finish(lv_obj_t *parent)
{
    if (!parent) return;
    if (orb_glass_get_amount(parent) != 1.f) orb_glass_set_amount(parent, 1.f);
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); i++)
        orb_kit_material_finish(lv_obj_get_child(parent, i));
}
