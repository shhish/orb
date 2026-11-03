/* Frozen serial native row kernel for the immutable-batch pixel oracle. */
#include "orb_game_native.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"

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

static void mask_prepare(native_mask_t *mask, const lv_opa_t *pixels, unsigned diameter)
{
    mask->pixels = pixels;
    mask->diameter = diameter;
    for (unsigned y = 0; y < diameter; ++y) {
        const lv_opa_t *row = pixels + y * diameter;
        unsigned first = 0, last = diameter;
        while (first < diameter && !row[first]) ++first;
        while (last > first && !row[last - 1]) --last;
        unsigned opaque_first = first, opaque_last = last;
        while (opaque_first < last && row[opaque_first] < LV_OPA_MAX) ++opaque_first;
        while (opaque_last > opaque_first && row[opaque_last - 1] < LV_OPA_MAX) --opaque_last;
        mask->rows[y] = (native_row_t){(uint8_t)first, (uint8_t)last,
            (uint8_t)opaque_first, (uint8_t)opaque_last};
    }
}

void orb_game_native_beads_prepare_reference(orb_game_native_beads_t *masks,
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

static void stamp_native(const native_target_t *target, int x, int y,
                          const native_mask_t *mask, const native_ink_t *ink)
{
    int radius = (int)mask->diameter / 2;
    int left = x - radius, top = y - radius;
    int first_y = LV_MAX(top, target->clip.y1), last_y = LV_MIN(y + radius, target->clip.y2);
    int first_x = LV_MAX(0, target->clip.x1 - left);
    int last_x = LV_MIN((int)mask->diameter, target->clip.x2 - left + 1);
    if (first_y > last_y || first_x >= last_x) return;
    lv_layer_t *layer = target->layer;
    for (int py = first_y; py <= last_y; ++py) {
        const native_row_t *row = &mask->rows[py - top];
        int first = LV_MAX(first_x, row->first), last = LV_MIN(last_x, row->last);
        if (first >= last) continue;
        /* Anchor at the clipped first pixel; never form a pointer before the
         * layer allocation for a stamp crossing a display/band boundary. */
        uint16_t *out = (uint16_t *)(layer->draw_buf->data +
            (py - layer->buf_area.y1) * target->stride +
            (left + first - layer->buf_area.x1) * 2);
        const lv_opa_t *alpha = mask->pixels + (py - top) * mask->diameter;
        native_row_t clipped = {(uint8_t)first, (uint8_t)last,
            (uint8_t)LV_MAX(first, row->opaque_first), (uint8_t)LV_MAX(first, row->opaque_last)};
        clipped.opaque_first -= (uint8_t)first;
        clipped.opaque_last -= (uint8_t)first;
        paint_row(out, alpha + first, &clipped, 0, last - first, ink, target->swapped);
    }
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

void orb_game_native_beads_reference(lv_layer_t *layer, const orb_game_native_beads_t *masks,
                            size_t count, const int16_t *x, const int16_t *y,
                            const uint8_t *opacity, const orb_game_sprite_t *sprites)
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
        native_ink_t body_ink = ink_prepare(sprites[i].colour, target.swapped);
        /* The intended HIG artwork is now one flat seeded body per bead. */
        if (target.native) {
            stamp_native(&target, px, py, body, &body_ink);
        } else {
            stamp_fallback(&target, px, py, body, &body_ink);
        }
    }
}
