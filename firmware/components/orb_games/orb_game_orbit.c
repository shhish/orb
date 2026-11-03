#include "orb_game_orbit.h"
#include "orb_game_native.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"
#include "src/draw/sw/lv_draw_sw.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"

#include <math.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#endif

#define TAU 6.28318530718f
#define ORBIT_SPRITES (7u + ORB_GAME_OBSTACLES * 3u + ORB_GAME_ORBIT_STARS + ORB_GAME_PARTICLES + 1u)
#define ORBIT_PRIMITIVES (ORBIT_SPRITES + ORB_GAME_ORBIT_STARS * 5u)
#define ORBIT_SPLIT_MIN_PIXELS (466u * 16u)

/* Opaque discs up to this radius (planet, comets, star centres, trails) stamp
 * LVGL's own circle coverage instead of setting up a radius mask per band. */
#define ORBIT_DISC_RADIUS 15u
#define ORBIT_DISC_D (ORBIT_DISC_RADIUS * 2u + 1u)
#define ORBIT_DISCS 8u

typedef enum { ORBIT_FILL, ORBIT_BORDER, ORBIT_TRIANGLE, ORBIT_DISC } orbit_opcode_t;

typedef struct {
    unsigned radius;
    lv_opa_t pixels[ORBIT_DISC_D * ORBIT_DISC_D];
    orb_game_native_row_t rows[ORBIT_DISC_D];
} orbit_disc_t;

typedef struct {
    orbit_opcode_t opcode;
    lv_area_t area;
    lv_color_t colour;
    uint8_t opacity;
    lv_point_precise_t triangle[3];
    const orbit_disc_t *disc;
} orbit_primitive_t;

struct orb_game_orbit {
    orb_game_orbit_ring_t *rings[3];
    unsigned references, readers, count, disc_count;
    uint64_t revision;
    uint32_t ring_colour;
    bool prepared;
    orbit_primitive_t primitives[ORBIT_PRIMITIVES];
    /* Grow-only and written only by the UI thread while no task reads the
     * source, so worker tasks always see immutable coverage. */
    orbit_disc_t discs[ORBIT_DISCS];
};

typedef struct {
    lv_draw_task_t task;
    size_t stride;
    uint16_t ink, stored;
    uint32_t prepared;
    lv_color_t colour;
    bool native, swapped;
} orbit_ring_target_t;

static lv_color_t artwork_colour(uint32_t colour)
{
    return lv_color_make((uint8_t)(colour >> 16), (uint8_t)(colour >> 8), (uint8_t)colour);
}

static void source_free(orb_game_orbit_t *source)
{
    for (unsigned i = 0; i < 3; ++i) {
#ifdef ESP_PLATFORM
        orb_memory_free(source->rings[i]);
#else
        free(source->rings[i]);
#endif
    }
#ifdef ESP_PLATFORM
    orb_memory_free(source);
#else
    free(source);
#endif
}

orb_game_orbit_t *orb_game_orbit_create(orb_game_orbit_ring_t *const rings[3])
{
#ifdef ESP_PLATFORM
    orb_game_orbit_t *source = orb_memory_calloc(ORB_MEMORY_GAMES, 1, sizeof(*source));
#else
    orb_game_orbit_t *source = calloc(1, sizeof(*source));
#endif
    if (!source) return NULL;
    source->references = 1;
    for (unsigned i = 0; i < 3; ++i) source->rings[i] = rings[i];
    return source;
}

void orb_game_orbit_release(orb_game_orbit_t *source)
{
    if (source && !--source->references) source_free(source);
}

static orbit_primitive_t *primitive(orb_game_orbit_t *source, orbit_opcode_t opcode,
                                     const lv_area_t *area, lv_color_t colour, uint8_t opacity)
{
    if (source->count == ORBIT_PRIMITIVES) return NULL;
    orbit_primitive_t *out = &source->primitives[source->count++];
    out->opcode = opcode;
    out->area = *area;
    out->colour = colour;
    out->opacity = opacity;
    return out;
}

static const orbit_disc_t *disc_coverage(orb_game_orbit_t *source, unsigned radius)
{
    if (!radius || radius > ORBIT_DISC_RADIUS) return NULL;
    for (unsigned i = 0; i < source->disc_count; ++i)
        if (source->discs[i].radius == radius) return &source->discs[i];
    if (source->disc_count == ORBIT_DISCS) return NULL;
    orbit_disc_t *disc = &source->discs[source->disc_count];
    unsigned diameter = radius * 2u + 1u;
    if (!orb_game_native_circle(disc->pixels, radius)) return NULL;
    orb_game_native_rows(disc->rows, disc->pixels, diameter);
    disc->radius = radius;
    ++source->disc_count;
    return disc;
}

static bool prepare_circle(orb_game_orbit_t *source, int x, int y, unsigned radius,
                             lv_color_t colour, uint8_t opacity, bool border)
{
    lv_area_t area = {x - (int)radius, y - (int)radius, x + (int)radius, y + (int)radius};
    /* LVGL ignores fill opacity from LV_OPA_MAX upward; fainter discs keep the
     * LVGL fill, whose mask-times-opacity rounding the native mix does not use. */
    const orbit_disc_t *disc = !border && opacity >= LV_OPA_MAX ? disc_coverage(source, radius) : NULL;
    orbit_primitive_t *out = primitive(source, border ? ORBIT_BORDER : disc ? ORBIT_DISC : ORBIT_FILL,
        &area, colour, opacity);
    if (out) out->disc = disc;
    return out != NULL;
}

static bool prepare_star(orb_game_orbit_t *source, int x, int y, unsigned radius,
                           lv_color_t colour, uint8_t opacity)
{
    lv_point_precise_t vertices[10];
    for (unsigned i = 0; i < 10; ++i) {
        float rr = i % 2 == 0 ? (float)radius : (float)radius * .5f;
        float angle = -TAU / 4 + (float)i * TAU / 10;
        vertices[i] = (lv_point_precise_t){x + cosf(angle) * rr, y + sinf(angle) * rr};
    }
    if (!prepare_circle(source, x, y, radius / 2, colour, opacity, false)) return false;
    for (unsigned i = 0; i < 10; i += 2) {
        const lv_point_precise_t *a = &vertices[i], *b = &vertices[(i + 9) % 10], *c = &vertices[(i + 1) % 10];
        lv_area_t area = {(int32_t)LV_MIN3(a->x, b->x, c->x), (int32_t)LV_MIN3(a->y, b->y, c->y),
            (int32_t)LV_MAX3(a->x, b->x, c->x), (int32_t)LV_MAX3(a->y, b->y, c->y)};
        orbit_primitive_t *out = primitive(source, ORBIT_TRIANGLE, &area, colour, opacity);
        if (!out) return false;
        out->triangle[0] = *a;
        out->triangle[1] = *b;
        out->triangle[2] = *c;
    }
    return true;
}

static bool prepare_pose(orb_game_orbit_t *source, const orb_game_sprite_t *sprites,
                           size_t count, uint64_t revision, uint32_t ring_colour)
{
    if (count > ORBIT_SPRITES) return false;
    source->prepared = false;
    source->count = 0;
    for (size_t i = 0; i < count; ++i) {
        const orb_game_sprite_t *sprite = &sprites[i];
        if (!sprite->radius || !sprite->opacity) continue;
        int x = (int)lroundf(sprite->x), y = (int)lroundf(sprite->y);
        lv_color_t colour = artwork_colour(sprite->colour);
        bool ok;
        switch (sprite->shape) {
        case ORB_GAME_STAR:
            ok = prepare_star(source, x, y, sprite->radius, colour, sprite->opacity);
            break;
        case ORB_GAME_DISC:
        case ORB_GAME_PARTICLE:
        case ORB_GAME_RING:
            ok = prepare_circle(source, x, y, sprite->radius, colour, sprite->opacity,
                sprite->shape == ORB_GAME_RING);
            break;
        default:
            return false;
        }
        if (!ok) return false;
    }
    source->revision = revision;
    source->ring_colour = ring_colour;
    source->prepared = true;
    return true;
}

static void draw_circle(lv_draw_task_t *task, const orbit_primitive_t *primitive)
{
    if (primitive->opcode == ORBIT_DISC) {
        const lv_area_t *area = &primitive->area;
        int radius = (int)primitive->disc->radius;
        if (orb_game_native_stamp(task->target_layer, &task->clip_area, area->x1 + radius, area->y1 + radius,
                primitive->disc->pixels, primitive->disc->rows, primitive->disc->radius * 2u + 1u,
                lv_color_to_u32(primitive->colour) & 0xffffffu)) return;
    }
    if (primitive->opcode == ORBIT_BORDER) {
        lv_draw_border_dsc_t descriptor;
        lv_draw_border_dsc_init(&descriptor);
        descriptor.radius = LV_RADIUS_CIRCLE;
        descriptor.color = primitive->colour;
        descriptor.opa = primitive->opacity;
        descriptor.width = 2;
        lv_draw_sw_border(task, &descriptor, &primitive->area);
    } else {
        lv_draw_fill_dsc_t descriptor;
        lv_draw_fill_dsc_init(&descriptor);
        descriptor.radius = LV_RADIUS_CIRCLE;
        descriptor.color = primitive->colour;
        descriptor.opa = primitive->opacity;
        lv_draw_sw_fill(task, &descriptor, &primitive->area);
    }
}

static void draw_primitive(lv_draw_task_t *task, const orbit_primitive_t *primitive)
{
    if (primitive->opacity <= LV_OPA_MIN) return;
    const lv_area_t *area = &primitive->area, *clip = &task->clip_area;
    if (area->x2 < clip->x1 || area->x1 > clip->x2 ||
        area->y2 < clip->y1 || area->y1 > clip->y2) return;
    if (primitive->opcode == ORBIT_TRIANGLE) {
        lv_draw_triangle_dsc_t descriptor;
        lv_draw_triangle_dsc_init(&descriptor);
        descriptor.color = primitive->colour;
        descriptor.opa = primitive->opacity;
        for (unsigned i = 0; i < 3; ++i) descriptor.p[i] = primitive->triangle[i];
        lv_draw_sw_triangle(task, &descriptor);
    } else draw_circle(task, primitive);
}

static orbit_ring_target_t ring_target(lv_layer_t *layer, const lv_area_t *clip, uint32_t colour)
{
    orbit_ring_target_t target = {0};
    target.task.target_layer = layer;
    target.task.clip_area = *clip;
    target.stride = layer->draw_buf->header.stride;
    target.colour = artwork_colour(colour);
    target.ink = lv_color_to_u16(target.colour);
    target.swapped = layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED;
    target.native = target.swapped || layer->color_format == LV_COLOR_FORMAT_RGB565;
    target.stored = target.swapped ? lv_color_swap_16(target.ink) : target.ink;
    target.prepared = LV_COLOR_MIX_16_PREPARE(target.ink);
    return target;
}

static void span_native(const orbit_ring_target_t *target, int y, int left, int right,
                          const uint8_t *alpha, int alpha_step)
{
    const lv_area_t *clip = &target->task.clip_area;
    int first = LV_MAX(left, clip->x1), last = LV_MIN(right, clip->x2);
    if (first > last) return;
    lv_layer_t *layer = target->task.target_layer;
    uint16_t *pixels = (uint16_t *)(layer->draw_buf->data +
        (y - layer->buf_area.y1) * target->stride + (first - layer->buf_area.x1) * 2);
    if (alpha) alpha += (first - left) * alpha_step;
    for (int x = first; x <= last; ++x, ++pixels) {
        uint32_t coverage = alpha ? *alpha : LV_OPA_COVER;
        if (alpha && x < last) alpha += alpha_step;
        if (!coverage) continue;
        if (coverage >= LV_OPA_MAX) *pixels = target->stored;
        else {
            uint16_t value = target->swapped ? lv_color_swap_16(*pixels) : *pixels;
            LV_COLOR_MIX_16_TO_16_PREPARED(value, target->prepared, value, coverage);
            *pixels = target->swapped ? lv_color_swap_16(value) : value;
        }
    }
}

static void ring_span(orbit_ring_target_t *target, int y, int left, int right,
                        const uint8_t *alpha, bool mirror)
{
    if (left > right) return;
    int count = right - left + 1;
    int alpha_step = 1;
    if (mirror) {
        int old_left = left;
        left = 466 - right;
        right = 466 - old_left;
        if (alpha) { alpha += count - 1; alpha_step = -1; }
    }
    const lv_area_t *clip = &target->task.clip_area;
    if (right < clip->x1 || left > clip->x2) return;
    if (target->native) { span_native(target, y, left, right, alpha, alpha_step); return; }
    uint8_t reversed[234];
    if (alpha && alpha_step < 0) {
        for (int i = 0; i < count; ++i) reversed[i] = alpha[-i];
        alpha = reversed;
    }
    lv_area_t area = {left, y, right, y};
    lv_draw_sw_blend_dsc_t blend = {0};
    blend.color = target->colour;
    blend.opa = LV_OPA_COVER;
    blend.blend_area = &area;
    blend.mask_area = &area;
    blend.mask_buf = alpha;
    blend.mask_res = alpha ? LV_DRAW_SW_MASK_RES_CHANGED : LV_DRAW_SW_MASK_RES_FULL_COVER;
    lv_draw_sw_blend(&target->task, &blend);
}

static void paint_ring(orbit_ring_target_t *target, const orb_game_orbit_ring_t *cache)
{
    int radius = cache->radius, origin = 233 - radius;
    const lv_area_t *clip = &target->task.clip_area;
    /* Retain the serial renderer's exact empty-annulus rejection before
     * walking PSRAM quadrant rows for a tiny sprite's damage rectangle. */
    int near_x = clip->x1 > 233 ? clip->x1 - 233 : clip->x2 < 233 ? 233 - clip->x2 : 0;
    int near_y = clip->y1 > 233 ? clip->y1 - 233 : clip->y2 < 233 ? 233 - clip->y2 : 0;
    int far_x = LV_MAX(abs(clip->x1 - 233), abs(clip->x2 - 233));
    int far_y = LV_MAX(abs(clip->y1 - 233), abs(clip->y2 - 233));
    int outer = radius + 2, inner = LV_MAX(0, radius - (int)cache->width - 2);
    if (near_x * near_x + near_y * near_y > outer * outer ||
        far_x * far_x + far_y * far_y < inner * inner) return;
    int first_y = LV_MAX(origin, clip->y1), last_y = LV_MIN(233 + radius, clip->y2);
    const uint8_t *samples = (const uint8_t *)(cache->rows + radius + 1);
    for (int y = first_y; y <= last_y; ++y) {
        int row_y = y - origin;
        if (row_y > radius) row_y = radius * 2 - row_y;
        const orb_game_orbit_ring_row_t *row = &cache->rows[row_y];
        if (row->first > row->last) continue;
        const uint8_t *alpha = samples + row->alpha_offset;
        int before = row->opaque_first - row->first;
        for (unsigned side = 0; side < 2; ++side) {
            int limit = side ? radius - 1 : radius;
            ring_span(target, y, origin + row->first,
                origin + LV_MIN((int)row->opaque_first - 1, limit), alpha, side != 0);
            ring_span(target, y, origin + row->opaque_first,
                origin + LV_MIN(row->opaque_last, limit), NULL, side != 0);
            ring_span(target, y, origin + row->opaque_last + 1,
                origin + LV_MIN(row->last, limit), alpha + before, side != 0);
        }
    }
}

static void pose_draw(lv_layer_t *layer, const lv_area_t *clip, const void *payload)
{
    const orb_game_orbit_t *source = payload;
    orbit_ring_target_t target = ring_target(layer, clip, source->ring_colour);
    static const unsigned radii[] = {(unsigned)ORB_GAME_ORBIT_R0 + 1,
        (unsigned)ORB_GAME_ORBIT_R1 + 1, (unsigned)ORB_GAME_ORBIT_R2 + 1};
    for (unsigned i = 0; i < 3; ++i) {
        if (source->rings[i]) paint_ring(&target, source->rings[i]);
        else {
            int radius = (int)radii[i];
            orbit_primitive_t ring = {.opcode = ORBIT_BORDER,
                .area = {233 - radius, 233 - radius, 233 + radius, 233 + radius},
                .colour = target.colour, .opacity = LV_OPA_COVER};
            draw_circle(&target.task, &ring);
        }
    }
    for (unsigned i = 0; i < source->count; ++i) draw_primitive(&target.task, &source->primitives[i]);
}

static void pose_retire(void *payload)
{
    orb_game_orbit_t *source = payload;
    --source->readers;
    orb_game_orbit_release(source);
}

static void draw_drain(lv_layer_t *layer)
{
    while (layer->draw_task_head) {
        lv_draw_dispatch_layer(layer->display, layer);
        lv_draw_dispatch();
        lv_draw_wait_for_finish();
    }
}

bool orb_game_orbit_dispatch(orb_game_orbit_t *source, lv_layer_t *layer,
                              const orb_game_sprite_t *sprites, size_t count,
                              uint64_t revision, uint32_t ring_colour)
{
    if (!source) return false;
    if (source->readers) {
        lv_draw_wait_for_finish();
        lv_draw_dispatch();
        if (source->readers) return false;
    }
    if ((!source->prepared || source->revision != revision || source->ring_colour != ring_colour) &&
        !prepare_pose(source, sprites, count, revision, ring_colour)) return false;
    lv_area_t clip;
    if (!lv_area_intersect(&clip, &layer->_clip_area, &layer->buf_area)) return true;
    /* Tiny damage clips repeat circle/triangle setup across two workers.
     * Keep one whole-clip task there; large canvas regions retain both. */
    unsigned slice_count = lv_area_get_size(&clip) >= ORBIT_SPLIT_MIN_PIXELS &&
        lv_area_get_height(&clip) > 1 ? 2u : 1u;
    lv_area_t slices[2] = {clip, clip};
    if (slice_count == 2) {
        slices[0].y2 = (clip.y1 + clip.y2) / 2;
        slices[1].y1 = slices[0].y2 + 1;
    }
    ++source->references; /* Creator protects partial enqueue and synchronous fallback. */
    for (unsigned i = 0; i < slice_count; ++i) {
        if (slices[i].y1 > slices[i].y2) continue;
        ++source->references;
        ++source->readers;
        if (!lv_draw_custom(layer, &slices[i], pose_draw, pose_retire, source)) {
            pose_retire(source);
            draw_drain(layer);
            pose_draw(layer, &slices[i], source);
        }
    }
    orb_game_orbit_release(source);
    return true;
}
