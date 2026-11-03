#include "orb_kit.h"
#include "orb_ui_private.h"

#include <math.h>
#include <string.h>

typedef struct {
    lv_obj_t *obj;
    orb_delight_spring_t width[27];
    unsigned count, active;
    bool moving;
} kit_page_dots_t;

static const char s_dots_motion_key;

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    return obj;
}

static void dots_present(kit_page_dots_t *dots)
{
    float total = (dots->count - 1) * 6.f;
    for (unsigned i = 0; i < dots->count; i++) total += dots->width[i].value;
    float cursor = -total * .5f;
    for (unsigned i = 0; i < dots->count; i++) {
        lv_obj_t *dot = lv_obj_get_child(dots->obj, i);
        float width = dots->width[i].value;
        float x = cursor + width * .5f;
        /* Radius 186 leaves room inside a surrounding level/ring. Capsules
         * borrow the same bounded lane; all dots remain content, not targets. */
        int32_t y = ORB_SCREEN_R + (int32_t)lroundf(sqrtf(LV_MAX(0.f, 186.f * 186.f - x * x)));
        int32_t w = (int32_t)lroundf(width);
        lv_obj_set_size(dot, w, 8);
        lv_obj_set_pos(dot, ORB_SCREEN_R + (int32_t)lroundf(x) - w / 2, y - 4);
        cursor += width + 6;
    }
}

static bool dots_tick(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    kit_page_dots_t *dots = context;
    dots->moving = false;
    for (unsigned i = 0; i < dots->count; i++) {
        if (finish || elapsed >= 420)
            orb_delight_spring_reset(&dots->width[i], dots->width[i].target);
        else orb_delight_spring_step(&dots->width[i], dt / 1000.f, .35f, .15f);
        dots->moving |= !orb_delight_spring_settled(&dots->width[i]);
    }
    dots_present(dots);
    return dots->moving;
}

static void dots_event(lv_event_t *event)
{
    kit_page_dots_t *dots = lv_event_get_user_data(event);
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    orb_kit_motion_cancel(dots->obj, &s_dots_motion_key, false);
    orb_ui_free(dots);
}

lv_obj_t *orb_kit_page_dots_create(lv_obj_t *parent, unsigned count)
{
    /* 14 px minimum pitch; 27 dots fit the approved radius 186 rim lane. */
    if (!count || count > 27) return NULL;
    kit_page_dots_t *dots = orb_ui_alloc(sizeof(*dots));
    if (!dots) return NULL;
    memset(dots, 0, sizeof(*dots));
    dots->count = count;
    dots->obj = plain(parent);
    lv_obj_set_size(dots->obj, ORB_SCREEN_D, ORB_SCREEN_D);
    lv_obj_set_floating(dots->obj, true);
    lv_obj_set_ignore_layout(dots->obj, true);
    lv_obj_center(dots->obj);
    lv_obj_add_event_cb(dots->obj, dots_event, LV_EVENT_DELETE, dots);
    for (unsigned i = 0; i < count; i++) {
        /* Rebuilding or restoring a screen presents its existing page. Only
         * a subsequent change of active page earns the stretch response. */
        orb_delight_spring_reset(&dots->width[i], i ? 8 : 20);
        lv_obj_t *dot = plain(dots->obj);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, i ? ORB_KIT_SECONDARY : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(dot, i ? 89 : LV_OPA_COVER, 0);
    }
    dots_present(dots);
    return dots->obj;
}

void orb_kit_page_dots_set_active(lv_obj_t *obj, unsigned index, lv_color_t accent)
{
    kit_page_dots_t *dots = kit_event_state(obj, dots_event);
    if (!dots || index >= dots->count) return;
    bool changed = dots->active != index;
    dots->active = index;
    for (unsigned i = 0; i < dots->count; i++) {
        lv_obj_t *dot = lv_obj_get_child(obj, i);
        lv_color_t fill = i == index ? accent : ORB_KIT_SECONDARY;
        lv_opa_t opa = i == index ? LV_OPA_COVER : 89;
        if (!lv_color_eq(lv_obj_get_style_bg_color(dot, 0), fill)) lv_obj_set_style_bg_color(dot, fill, 0);
        if (lv_obj_get_style_bg_opa(dot, 0) != opa) lv_obj_set_style_bg_opa(dot, opa, 0);
        if (changed) orb_delight_spring_target(&dots->width[i], i == index ? 20 : 8);
    }
    if (!changed) return;
    if (orb_ui_reduce_motion()) dots_tick(dots, 0, 0, true);
    else {
        dots->moving = true;
        orb_kit_motion_start(obj, &s_dots_motion_key, dots_tick, dots);
    }
}
