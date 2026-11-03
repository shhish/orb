/* HIG §§2/5/6: a flat content fill, confined to the existing disc. The dormant
 * Home helper had no callers because details replaced their discs on update.
 * One kit implementation now serves both stable details and floating rows. */
#include "orb_kit_effects.h"
#include "orb_kit_motion.h"

#include <math.h>

typedef struct {
    lv_obj_t *disc, *glyph;
    lv_color_t after, glyph_ink;
    lv_opa_t after_opacity;
    orb_delight_spring_t fill, glyph_pop;
    int32_t glyph_scale;
    int32_t pivot_x, pivot_y;
    float pop;
    bool active;
} kit_fill_t;

static void fill_event(lv_event_t *event);

static kit_fill_t *fill_find(lv_obj_t *disc)
{
    if (!disc) return NULL;
    for (unsigned i = 0; i < lv_obj_get_event_count(disc); i++) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(disc, i);
        if (lv_event_dsc_get_cb(event) == fill_event) return lv_event_dsc_get_user_data(event);
    }
    return NULL;
}

void orb_kit_disc_light_up_prepare(lv_obj_t *disc)
{
    if (!disc) return;
    lv_obj_set_stable_transform(disc, true);
    if (fill_find(disc)) return;
    kit_fill_t *fill = lv_malloc_zeroed(sizeof(*fill));
    if (!fill) return;
    fill->disc = disc;
    lv_obj_add_event_cb(disc, fill_event, LV_EVENT_ALL, fill);
    orb_kit_motion_prepare(disc);
}

static void fill_glyph(kit_fill_t *fill, float pop)
{
    if (!fill->glyph) return;
    int32_t scale = (int32_t)lroundf(fill->glyph_scale * pop);
    if (lv_obj_get_raster(fill->glyph)) lv_obj_set_raster_scale(fill->glyph, scale);
    else lv_obj_set_style_transform_scale(fill->glyph, scale, 0);
}

static bool fill_frame(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    kit_fill_t *fill = context;
    if (finish || elapsed >= 250) {
        fill->active = false;
        lv_obj_set_style_bg_color(fill->disc, fill->after, 0);
        lv_obj_set_style_bg_opa(fill->disc, fill->after_opacity, 0);
        if (fill->glyph) lv_obj_set_style_text_color(fill->glyph, fill->glyph_ink, 0);
        fill_glyph(fill, 1.f);
        if (fill->glyph) {
            lv_obj_set_style_transform_pivot_x(fill->glyph, fill->pivot_x, 0);
            lv_obj_set_style_transform_pivot_y(fill->glyph, fill->pivot_y, 0);
        }
        lv_obj_invalidate(fill->disc);
        return false;
    }
    orb_delight_spring_step(&fill->fill, dt / 1000.f, .25f, .15f);
    /* Retarget the same velocity-preserving spring after its initial lift.
     * The final tick restores the exact cached shoulder scale. */
    if (elapsed >= 80) orb_delight_spring_target(&fill->glyph_pop, 1);
    orb_delight_spring_step(&fill->glyph_pop, dt / 1000.f, .15f, .15f);
    fill->pop = LV_MAX(1.f, fill->glyph_pop.value);
    fill_glyph(fill, fill->pop);
    if (fill->glyph && fill->fill.value >= .72f)
        lv_obj_set_style_text_color(fill->glyph, fill->glyph_ink, 0);
    lv_obj_invalidate(fill->disc);
    return true;
}

static void fill_draw(kit_fill_t *fill, lv_layer_t *layer)
{
    if (!fill->active) return;
    lv_area_t bounds;
    lv_obj_get_coords(fill->disc, &bounds);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    int32_t diameter = LV_MIN(lv_area_get_width(&bounds), lv_area_get_height(&bounds));
    int32_t growing = (int32_t)lroundf(diameter * LV_CLAMP(0.f, fill->fill.value, 1.f));
    if (growing <= 0) return;
    int32_t x = bounds.x1 + (lv_area_get_width(&bounds) - growing) / 2;
    int32_t y = bounds.y1 + (lv_area_get_height(&bounds) - growing) / 2;
    lv_area_t patch = {x, y, x + growing - 1, y + growing - 1};
    /* The disc sits on Orb's true-black canvas. Resolve a translucent track
     * against that canvas so darkening replaces the previous flat fill. */
    dsc.bg_color = lv_color_mix(fill->after, lv_color_black(), fill->after_opacity);
    lv_draw_rect(layer, &dsc, &patch);
}

static void fill_event(lv_event_t *event)
{
    kit_fill_t *fill = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        orb_kit_motion_cancel(fill->disc, fill, false);
        lv_free(fill);
    } else if (code == LV_EVENT_DRAW_MAIN) fill_draw(fill, lv_event_get_layer(event));
    else if (code == LV_EVENT_PRESSED || code == LV_EVENT_SCROLL_BEGIN)
        orb_kit_motion_cancel(fill->disc, fill, true);
}

void orb_kit_disc_light_up_cancel(lv_obj_t *disc)
{
    kit_fill_t *fill = fill_find(disc);
    if (fill && fill->active) orb_kit_motion_cancel(disc, fill, true);
}

bool orb_kit_disc_light_up_matches(lv_obj_t *disc, lv_color_t colour, lv_color_t ink)
{
    kit_fill_t *fill = fill_find(disc);
    return fill && fill->active && lv_color_eq(fill->after, colour) && lv_color_eq(fill->glyph_ink, ink);
}

static void disc_light_up(lv_obj_t *disc, lv_color_t before, lv_color_t after, bool committed)
{
    if (!disc) return;
    orb_kit_disc_light_up_cancel(disc);
    lv_obj_set_style_bg_color(disc, after, 0);
    if (lv_color_eq(before, after) || (!committed && lv_obj_has_state(disc, LV_STATE_PRESSED)) ||
        !orb_kit_motion_allowed(disc)) return;
    if (lv_obj_get_width(disc) != lv_obj_get_height(disc)) return;
    orb_kit_disc_light_up_prepare(disc);
    kit_fill_t *fill = fill_find(disc);
    if (!fill) return;
    fill->glyph = NULL;
    for (unsigned i = 0; i < lv_obj_get_child_count(disc); i++) {
        lv_obj_t *child = lv_obj_get_child(disc, i);
        if (lv_obj_check_type(child, &lv_label_class)) { fill->glyph = child; break; }
    }
    fill->after = after;
    fill->after_opacity = lv_obj_get_style_bg_opa(disc, 0);
    /* Painting two antialiased full discs exposes the target rim through
     * the old colour. Keep the native base old and draw only the new centre. */
    lv_obj_set_style_bg_color(disc, before, 0);
    fill->active = true;
    fill->pop = 1;
    orb_delight_spring_reset(&fill->fill, 0);
    orb_delight_spring_target(&fill->fill, 1);
    orb_delight_spring_reset(&fill->glyph_pop, 1);
    orb_delight_spring_target(&fill->glyph_pop, 1.1f);
    if (fill->glyph) {
        fill->glyph_ink = lv_obj_get_style_text_color(fill->glyph, 0);
        lv_obj_set_style_text_color(fill->glyph, orb_kit_accent_ink(before), 0);
        fill->glyph_scale = lv_obj_get_style_transform_scale_x(fill->glyph, 0);
        fill->pivot_x = lv_obj_get_style_transform_pivot_x(fill->glyph, 0);
        fill->pivot_y = lv_obj_get_style_transform_pivot_y(fill->glyph, 0);
        lv_obj_set_style_transform_pivot_x(fill->glyph, lv_obj_get_width(fill->glyph) / 2, 0);
        lv_obj_set_style_transform_pivot_y(fill->glyph, lv_obj_get_height(fill->glyph) / 2, 0);
    }
    orb_kit_motion_start(disc, fill, fill_frame, fill);
    lv_obj_invalidate(disc);
}

void orb_kit_disc_light_up(lv_obj_t *disc, lv_color_t before, lv_color_t after)
{
    disc_light_up(disc, before, after, false);
}

void orb_kit_disc_light_up_to(lv_obj_t *disc, lv_color_t after, lv_opa_t opacity)
{
    if (!disc) return;
    lv_color_t before = lv_obj_get_style_bg_color(disc, 0);
    /* Local toggles commit in CLICKED, before LVGL retires PRESSED. This is
     * a real state change after release, not feedback during a held contact. */
    disc_light_up(disc, before, after, true);
    kit_fill_t *fill = fill_find(disc);
    if (fill && fill->active) fill->after_opacity = opacity;
    else lv_obj_set_style_bg_opa(disc, opacity, 0);
}

/* Floating rows refresh their cached shoulder pose before drawing. Compose
 * the pop with that exact base scale instead of replacing the row's scale. */
void orb_kit_disc_light_up_pose(lv_obj_t *disc, int32_t scale)
{
    kit_fill_t *fill = fill_find(disc);
    if (!fill || !fill->active) return;
    fill->glyph_scale = scale;
    fill_glyph(fill, fill->pop);
}
