#include "orb_game_native.h"
#include <string.h>
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include "src/draw/sw/lv_draw_sw_mask_private.h"

#define BODY_D ORB_GAME_NATIVE_BODY_D
typedef orb_game_native_row_t native_row_t;
typedef orb_game_native_mask_t native_mask_t;

typedef struct {
    lv_layer_t *layer;
    lv_area_t clip;
    size_t stride;
    bool swapped, native;
} native_target_t;

typedef struct {
    lv_color_t colour;
    uint16_t stored;
    uint32_t prepared;
} native_ink_t;

void orb_game_native_rows(orb_game_native_row_t *rows, const lv_opa_t *pixels, unsigned diameter)
{
    for (unsigned y = 0; y < diameter; ++y) {
        const lv_opa_t *row = pixels + y * diameter;
        unsigned first = 0, last = diameter;
        while (first < diameter && !row[first]) ++first;
        while (last > first && !row[last - 1]) --last;
        unsigned opaque_first = first, opaque_last = last;
        while (opaque_first < last && row[opaque_first] < LV_OPA_MAX) ++opaque_first;
        while (opaque_last > opaque_first && row[opaque_last - 1] < LV_OPA_MAX) --opaque_last;
        rows[y] = (native_row_t){(uint8_t)first, (uint8_t)last,
            (uint8_t)opaque_first, (uint8_t)opaque_last};
    }
}

static void mask_prepare(native_mask_t *mask, const lv_opa_t *pixels, unsigned diameter)
{
    mask->pixels = pixels;
    mask->diameter = diameter;
    orb_game_native_rows(mask->rows, pixels, diameter);
}

bool orb_game_native_circle(lv_opa_t *pixels, unsigned radius)
{
    /* LVGL's own radius mask over the same (2r+1)-square a circular fill
     * uses; stamping it at full opacity reproduces that fill exactly. */
    unsigned diameter = radius * 2u + 1u;
    lv_area_t area = {0, 0, (int32_t)diameter - 1, (int32_t)diameter - 1};
    lv_draw_sw_mask_radius_param_t mask;
    if (lv_draw_sw_mask_radius_init(&mask, &area, (int32_t)radius, false) != LV_RESULT_OK) return false;
    void *masks[] = {&mask, NULL};
    memset(pixels, 255, diameter * diameter);
    for (unsigned y = 0; y < diameter; ++y)
        (void)lv_draw_sw_mask_apply(masks, pixels + y * diameter, 0, (int32_t)y, (int32_t)diameter);
    lv_draw_sw_mask_free_param(&mask);
    return true;
}

void orb_game_native_beads_prepare(orb_game_native_beads_t *masks,
                                    const lv_opa_t *body)
{
    mask_prepare(&masks->body, body, BODY_D);
}

static native_ink_t ink_prepare(uint32_t colour, bool swapped)
{
    /* This is pixel-format conversion of artwork supplied by the presenter,
     * not a renderer-owned design colour. */
    lv_color_t ink = lv_color_make((uint8_t)(colour >> 16), (uint8_t)(colour >> 8), (uint8_t)colour);
    uint16_t value = lv_color_to_u16(ink);
    return (native_ink_t){ink, swapped ? lv_color_swap_16(value) : value,
        LV_COLOR_MIX_16_PREPARE(value)};
}

static void mix_row565(uint16_t *pixels, const lv_opa_t *alpha, int first, int last,
                        const native_ink_t *ink)
{
    for (int x = first; x < last; ++x) {
        uint32_t coverage = alpha[x];
        if (!coverage) continue;
        if (coverage >= LV_OPA_MAX) pixels[x] = ink->stored;
        else {
            uint16_t value = pixels[x];
            LV_COLOR_MIX_16_TO_16_PREPARED(value, ink->prepared, value, coverage);
            pixels[x] = value;
        }
    }
}

static void mix_row_swapped(uint16_t *pixels, const lv_opa_t *alpha, int first, int last,
                             const native_ink_t *ink)
{
    for (int x = first; x < last; ++x) {
        uint32_t coverage = alpha[x];
        if (!coverage) continue;
        if (coverage >= LV_OPA_MAX) pixels[x] = ink->stored;
        else {
            uint16_t value = lv_color_swap_16(pixels[x]);
            LV_COLOR_MIX_16_TO_16_PREPARED(value, ink->prepared, value, coverage);
            pixels[x] = lv_color_swap_16(value);
        }
    }
}

static void paint_row(uint16_t *pixels, const lv_opa_t *alpha, const native_row_t *row,
                       int first, int last, const native_ink_t *ink, bool swapped)
{
    int opaque_first = LV_MAX(first, row->opaque_first);
    int opaque_last = LV_MIN(last, row->opaque_last);
    if (opaque_first >= opaque_last) {
        if (swapped) mix_row_swapped(pixels, alpha, first, last, ink);
        else mix_row565(pixels, alpha, first, last, ink);
        return;
    }
    if (swapped) mix_row_swapped(pixels, alpha, first, opaque_first, ink);
    else mix_row565(pixels, alpha, first, opaque_first, ink);
    for (int x = opaque_first; x < opaque_last; ++x) pixels[x] = ink->stored;
    if (swapped) mix_row_swapped(pixels, alpha, opaque_last, last, ink);
    else mix_row565(pixels, alpha, opaque_last, last, ink);
}

static void stamp_rows(const native_target_t *target, int x, int y, const lv_opa_t *pixels,
                        const native_row_t *rows, unsigned diameter, const native_ink_t *ink)
{
    int radius = (int)diameter / 2;
    int left = x - radius, top = y - radius;
    int first_y = LV_MAX(top, target->clip.y1), last_y = LV_MIN(y + radius, target->clip.y2);
    int first_x = LV_MAX(0, target->clip.x1 - left);
    int last_x = LV_MIN((int)diameter, target->clip.x2 - left + 1);
    if (first_y > last_y || first_x >= last_x) return;
    lv_layer_t *layer = target->layer;
    for (int py = first_y; py <= last_y; ++py) {
        const native_row_t *row = &rows[py - top];
        int first = LV_MAX(first_x, row->first), last = LV_MIN(last_x, row->last);
        if (first >= last) continue;
        /* Anchor at the clipped first pixel; never form a pointer before the
         * layer allocation for a stamp crossing a display/band boundary. */
        uint16_t *out = (uint16_t *)(layer->draw_buf->data +
            (py - layer->buf_area.y1) * target->stride +
            (left + first - layer->buf_area.x1) * 2);
        const lv_opa_t *alpha = pixels + (py - top) * diameter;
        native_row_t clipped = {(uint8_t)first, (uint8_t)last,
            (uint8_t)LV_MAX(first, row->opaque_first), (uint8_t)LV_MAX(first, row->opaque_last)};
        clipped.opaque_first -= (uint8_t)first;
        clipped.opaque_last -= (uint8_t)first;
        paint_row(out, alpha + first, &clipped, 0, last - first, ink, target->swapped);
    }
}

static void stamp_native(const native_target_t *target, int x, int y,
                          const native_mask_t *mask, const native_ink_t *ink)
{
    stamp_rows(target, x, y, mask->pixels, mask->rows, mask->diameter, ink);
}

bool orb_game_native_stamp(lv_layer_t *layer, const lv_area_t *clip, int x, int y,
                           const lv_opa_t *pixels, const orb_game_native_row_t *rows,
                           unsigned diameter, uint32_t colour)
{
    native_target_t target = {.layer = layer, .clip = *clip,
        .stride = layer->draw_buf->header.stride,
        .swapped = layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED};
    target.native = target.swapped || layer->color_format == LV_COLOR_FORMAT_RGB565;
    if (!target.native) return false;
    native_ink_t ink = ink_prepare(colour, target.swapped);
    stamp_rows(&target, x, y, pixels, rows, diameter, &ink);
    return true;
}

static void stamp_fallback(const native_target_t *target, int x, int y,
                            const native_mask_t *mask, const native_ink_t *ink)
{
    int radius = (int)mask->diameter / 2;
    lv_area_t area = {x - radius, y - radius, x + radius, y + radius};
    if (area.x2 < target->clip.x1 || area.x1 > target->clip.x2 ||
        area.y2 < target->clip.y1 || area.y1 > target->clip.y2) return;
    lv_draw_task_t task = {0};
    task.target_layer = target->layer;
    task.clip_area = target->clip;
    lv_draw_sw_blend_dsc_t blend = {0};
    blend.color = ink->colour;
    blend.opa = LV_OPA_COVER;
    blend.mask_res = LV_DRAW_SW_MASK_RES_CHANGED;
    blend.mask_stride = mask->diameter;
    blend.mask_buf = mask->pixels;
    blend.blend_area = &area;
    blend.mask_area = &area;
    lv_draw_sw_blend(&task, &blend);
}

static void fade_mask(native_mask_t *mask, lv_opa_t *pixels,
                        const native_mask_t *source, unsigned opacity)
{
    for (unsigned p = 0; p < source->diameter * source->diameter; ++p)
        pixels[p] = (lv_opa_t)LV_UDIV255(source->pixels[p] * opacity);
    mask_prepare(mask, pixels, source->diameter);
}

void orb_game_native_beads_colours(lv_layer_t *layer, const orb_game_native_beads_t *masks,
                                    size_t count, const int16_t *x, const int16_t *y,
                                    const uint8_t *opacity, const uint32_t *colours,
                                    size_t colour_stride)
{
    native_target_t target = {.layer = layer, .clip = layer->_clip_area,
        .stride = layer->draw_buf->header.stride,
        .swapped = layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED};
    target.native = target.swapped || layer->color_format == LV_COLOR_FORMAT_RGB565;
    lv_opa_t body_pixels[BODY_D * BODY_D];
    native_mask_t body_faded;
    uint8_t last_opacity = 0;
    for (size_t i = 0; i < count; ++i) {
        int px = x[i], py = y[i], radius = ORB_GAME_BEAD_DRAW_RADIUS;
        if (py + radius < target.clip.y1 || py - radius > target.clip.y2 ||
            px + radius < target.clip.x1 || px - radius > target.clip.x2) continue;
        if (opacity[i] <= LV_OPA_MIN) continue;
        const native_mask_t *body = &masks->body;
        if (opacity[i] < LV_OPA_MAX) {
            if (opacity[i] != last_opacity) {
                fade_mask(&body_faded, body_pixels, &masks->body, opacity[i]);
                last_opacity = opacity[i];
            }
            body = &body_faded;
        }
        const uint32_t *colour = (const uint32_t *)((const uint8_t *)colours + i * colour_stride);
        native_ink_t body_ink = ink_prepare(*colour, target.swapped);
        /* HIG §2.2: flat seeded bodies retain identity without a static
         * printed-on highlight or a second stamp for every particle. */
        if (target.native) {
            stamp_native(&target, px, py, body, &body_ink);
        } else {
            stamp_fallback(&target, px, py, body, &body_ink);
        }
    }
}

void orb_game_native_beads(lv_layer_t *layer, const orb_game_native_beads_t *masks,
                            size_t count, const int16_t *x, const int16_t *y,
                            const uint8_t *opacity, const orb_game_sprite_t *sprites)
{
    orb_game_native_beads_colours(layer, masks, count, x, y, opacity,
        count ? &sprites[0].colour : NULL, sizeof(*sprites));
}

bool orb_game_native_phase_stamp(lv_layer_t *layer, const lv_area_t *clip, int32_t qx, int32_t qy,
                                 const lv_opa_t *mask, const orb_game_native_row_t *rows, unsigned diameter,
                                 uint32_t colour, lv_opa_t opa, bool fill, int full_row)
{
    bool swapped = layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED;
    if (!swapped && layer->color_format != LV_COLOR_FORMAT_RGB565) return false;
    if (opa <= LV_OPA_MIN) return true;
    /* Floor division keeps each phase in 0..P-1 on either side of the origin. */
    const int32_t phases = ORB_GAME_NATIVE_PHASES;
    int32_t xi = qx >= 0 ? qx / phases : -((phases - 1 - qx) / phases);
    int32_t yi = qy >= 0 ? qy / phases : -((phases - 1 - qy) / phases);
    int fx = (int)(qx - xi * phases), fy = (int)(qy - yi * phases);
    int radius = (int)diameter / 2, size = (int)diameter;
    lv_area_t area = {xi - radius, yi - radius, xi + radius + (fx ? 1 : 0), yi + radius + (fy ? 1 : 0)};
    if (!lv_area_intersect(&area, &area, clip) || !lv_area_intersect(&area, &area, &layer->buf_area)) return true;
    native_ink_t ink = ink_prepare(colour, swapped);
    size_t stride = layer->draw_buf->header.stride;
    int w00 = (phases - fx) * (phases - fy), w10 = fx * (phases - fy), w01 = (phases - fx) * fy, w11 = fx * fy;
    int shift = 2 * ORB_GAME_NATIVE_PHASE_BITS, round = 1 << (shift - 1);
    for (int py = area.y1; py <= area.y2; ++py) {
        int v = py - (yi - radius);
        const lv_opa_t *row = v < size ? mask + v * size : NULL;
        const lv_opa_t *above = v >= 1 && v - 1 < size ? mask + (v - 1) * size : NULL;
        /* Only columns covered by this row or the one above can receive ink. */
        int first = size, last = -1;
        if (row) { first = rows[v].first; last = rows[v].last; }
        if (above && rows[v - 1].first < rows[v - 1].last) {
            first = LV_MIN(first, rows[v - 1].first);
            last = LV_MAX(last, rows[v - 1].last);
        }
        if (first >= last) continue;
        int x1 = LV_MAX(area.x1, xi - radius + first), x2 = LV_MIN(area.x2, xi - radius + last - 1 + (fx ? 1 : 0));
        uint16_t *out = (uint16_t *)(layer->draw_buf->data + (py - layer->buf_area.y1) * stride +
            (x1 - layer->buf_area.x1) * 2);
        for (int px = x1; px <= x2; ++px, ++out) {
            int u = px - (xi - radius);
            unsigned m00 = row && u < size ? row[u] : 0, m10 = row && u >= 1 && u - 1 < size ? row[u - 1] : 0;
            unsigned m01 = above && u < size ? above[u] : 0;
            unsigned m11 = above && u >= 1 && u - 1 < size ? above[u - 1] : 0;
            if (!(m00 | m10 | m01 | m11)) continue;
            /* Phase zero is (P*P*m + P*P/2) >> 2B == m: LVGL's own coverage. */
            unsigned c = (unsigned)(w00 * m00 + w10 * m10 + w01 * m01 + w11 * m11 + round) >> shift;
            unsigned a;
            if (opa >= LV_OPA_MAX) a = c;
            /* A border's straight middle-row segments are unmasked opa fills. */
            else if (!fill && !fx && !fy && v == full_row && c == LV_OPA_COVER) a = opa;
            else a = fill ? LV_UDIV255(c * opa) : LV_OPA_MIX2(c, opa);
            if (!a) continue;
            if (a >= LV_OPA_MAX) { *out = ink.stored; continue; }
            uint16_t value = swapped ? lv_color_swap_16(*out) : *out;
            LV_COLOR_MIX_16_TO_16_PREPARED(value, ink.prepared, value, a);
            *out = swapped ? lv_color_swap_16(value) : value;
        }
    }
    return true;
}
