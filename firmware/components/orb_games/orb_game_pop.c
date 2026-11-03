#include "orb_game_pop.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw_mask_private.h"
#include "src/misc/lv_area_private.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#endif

/* draw_sprite's bubble rim width. */
#define POP_RIM 3u

static void *coverage_alloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    return orb_memory_calloc(ORB_MEMORY_GAMES, 1, bytes);
#else
    return calloc(1, bytes);
#endif
}

static void coverage_free(void *pointer)
{
#ifdef ESP_PLATFORM
    orb_memory_free(pointer);
#else
    free(pointer);
#endif
}

/* LVGL's circular fill: masked rows above and below one unmasked centre row. */
static bool fill_coverage(lv_opa_t *pixels, unsigned radius)
{
    if (!orb_game_native_circle(pixels, radius)) return false;
    memset(pixels + radius * (radius * 2u + 1u), LV_OPA_COVER, radius * 2u + 1u);
    return true;
}

/* LVGL's circular border: inner (inverted) and outer radius masks on every row
 * but the centre, where only the two straight side segments are filled. */
static bool rim_coverage(lv_opa_t *pixels, unsigned radius)
{
    unsigned diameter = radius * 2u + 1u;
    lv_area_t outer = {0, 0, (int32_t)diameter - 1, (int32_t)diameter - 1};
    lv_area_t inner = {POP_RIM, POP_RIM, (int32_t)(diameter - 1 - POP_RIM), (int32_t)(diameter - 1 - POP_RIM)};
    lv_draw_sw_mask_radius_param_t in, out;
    if (lv_draw_sw_mask_radius_init(&in, &inner, (int32_t)(radius - POP_RIM), true) != LV_RESULT_OK) return false;
    if (lv_draw_sw_mask_radius_init(&out, &outer, (int32_t)radius, false) != LV_RESULT_OK) {
        lv_draw_sw_mask_free_param(&in);
        return false;
    }
    void *masks[] = {&in, &out, NULL};
    memset(pixels, LV_OPA_COVER, diameter * diameter);
    for (unsigned y = 0; y < diameter; ++y)
        (void)lv_draw_sw_mask_apply(masks, pixels + y * diameter, 0, (int32_t)y, (int32_t)diameter);
    lv_draw_sw_mask_free_param(&in);
    lv_draw_sw_mask_free_param(&out);
    lv_opa_t *centre = pixels + radius * diameter;
    memset(centre, LV_OPA_TRANSP, diameter);
    memset(centre, LV_OPA_COVER, POP_RIM);
    memset(centre + diameter - POP_RIM, LV_OPA_COVER, POP_RIM);
    return true;
}

static const orb_game_pop_coverage_t *find(const orb_game_pop_coverage_t *coverage, unsigned radius)
{
    for (unsigned i = 0; i < ORB_GAME_POP_COVERAGES; ++i)
        if (coverage[i].radius == radius) return &coverage[i];
    return NULL;
}

void orb_game_pop_coverage_prepare(orb_game_pop_coverage_t coverage[ORB_GAME_POP_COVERAGES],
                                   const orb_game_sprite_t *sprites, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const orb_game_sprite_t *s = &sprites[i];
        if (s->shape != ORB_GAME_BUBBLE || s->radius <= POP_RIM || s->radius > 120 || find(coverage, s->radius))
            continue;
        orb_game_pop_coverage_t *slot = NULL;
        for (unsigned k = 0; k < ORB_GAME_POP_COVERAGES && !slot; ++k)
            if (!coverage[k].radius) slot = &coverage[k];
        if (!slot) return;
        unsigned diameter = s->radius * 2u + 1u;
        size_t mask_bytes = 2u * diameter * diameter;
        lv_opa_t *pixels = coverage_alloc(mask_bytes + 2u * diameter * sizeof(orb_game_native_row_t));
        if (!pixels) return;
        lv_opa_t *rim = pixels + diameter * diameter;
        if (!fill_coverage(pixels, s->radius) || !rim_coverage(rim, s->radius)) {
            coverage_free(pixels);
            return;
        }
        orb_game_native_row_t *rows = (orb_game_native_row_t *)(pixels + mask_bytes);
        *slot = (orb_game_pop_coverage_t){s->radius, pixels, rim, rows, rows + diameter};
        orb_game_native_rows(slot->body_rows, slot->body, diameter);
        orb_game_native_rows(slot->rim_rows, slot->rim, diameter);
    }
}

void orb_game_pop_coverage_release(orb_game_pop_coverage_t coverage[ORB_GAME_POP_COVERAGES])
{
    for (unsigned i = 0; i < ORB_GAME_POP_COVERAGES; ++i) {
        coverage_free(coverage[i].body);
        coverage[i] = (orb_game_pop_coverage_t){0};
    }
}

#ifndef ESP_PLATFORM
static bool s_integer;

void orb_game_pop_test_integer(bool integer)
{
    s_integer = integer;
}
#endif

static int32_t phase(float value)
{
    return (int32_t)lroundf(value * ORB_GAME_NATIVE_PHASES);
}

bool orb_game_pop_draw(const orb_game_pop_coverage_t coverage[ORB_GAME_POP_COVERAGES], lv_layer_t *layer,
                       const orb_game_sprite_t *s)
{
    const orb_game_pop_coverage_t *c = find(coverage, s->radius);
#ifndef ESP_PLATFORM
    if (s_integer) return false;
#endif
    if (!c || (layer->color_format != LV_COLOR_FORMAT_RGB565 &&
        layer->color_format != LV_COLOR_FORMAT_RGB565_SWAPPED)) return false;
    lv_area_t clip;
    if (!lv_area_intersect(&clip, &layer->_clip_area, &layer->buf_area)) return true;
    /* The view's damage bound (centre +-(r + 1)) covers every phase. */
    int x = (int)lroundf(s->x), y = (int)lroundf(s->y), r = (int)s->radius;
    if (x + r + 1 < clip.x1 || x - r - 1 > clip.x2 || y + r + 1 < clip.y1 || y - r - 1 > clip.y2) return true;
    /* Earlier LVGL tasks of this layer (rings, particles) precede the stamp. */
    while (layer->draw_task_head) {
        lv_draw_dispatch();
        lv_draw_wait_for_finish();
    }
    unsigned diameter = s->radius * 2u + 1u;
    int32_t qx = phase(s->x), qy = phase(s->y);
    /* draw_sprite's two flat parts, order and opacities (HIG §§2/5/9). */
    orb_game_native_phase_stamp(layer, &clip, qx, qy, c->body, c->body_rows, diameter, s->colour,
        (uint8_t)(s->opacity * 31 / 255), true, -1);
    orb_game_native_phase_stamp(layer, &clip, qx, qy, c->rim, c->rim_rows, diameter, s->colour,
        s->opacity, false, r);
    return true;
}
