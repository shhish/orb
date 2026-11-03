/**
 * @file lv_draw_sw_blur.c
 * ORB PATCH: route task-local scratch through the bounded draw allocator.
 * ORB PATCH: averaged downsampling, bilinear reconstruction, antialiased
 * rounded compositing and screen-anchored RGB565 dithering. RGB565 uses
 * exact cell sums, separable interpolation and tint/quantization lookups.
 */

#include "../../misc/lv_area_private.h"
#include "lv_draw_sw_mask_private.h"
#include "../lv_draw_private.h"
#include "lv_draw_sw.h"
#include "lv_draw_sw_blur_cache_private.h"
#include "../../core/lv_obj_material_private.h"
#include <stdatomic.h>
#include <string.h>

#if LV_USE_DRAW_SW

#define BLUR_INTENSITY_BITS 12
#define BLUR_INTENSITY_MAX (1 << BLUR_INTENSITY_BITS)
#define BLUR_INTENSITY_HALF (BLUR_INTENSITY_MAX / 2)

#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
static lv_draw_sw_blur_fused_cb_t s_fused_callback;
void lv_draw_sw_blur_set_fused_cb(lv_draw_sw_blur_fused_cb_t callback)
{
    s_fused_callback = callback;
}
#endif

typedef struct {
    int32_t offset;
    uint16_t weight;
} blur_sample_t;

static void blur_line(uint8_t * buf, int32_t count, int32_t step, uint32_t channels,
                      uint32_t sample_len, uint32_t intensity);
static inline void read_pixel(const uint8_t * src, uint8_t * rgb, uint32_t px_size, bool swapped);
static inline void write_pixel(uint8_t * dst, const uint8_t * rgb, uint32_t px_size, bool swapped,
                       uint32_t coverage, int32_t x, int32_t y);
static void downsample_565_3(uint8_t * small, const uint8_t * source, int32_t width, int32_t height,
                            uint32_t stride, bool swapped);
static void downsample_565_4(uint8_t * small, const uint8_t * source, int32_t width, int32_t height,
                            uint32_t stride, bool swapped);
static void reconstruct_565(uint8_t * dst, const uint16_t * vertical, const int32_t * offsets, const uint8_t * weights,
                            const lv_opa_t * mask, const uint16_t * quant, int32_t width, uint32_t tint,
                            const uint8_t * tint_rgb, bool swapped, int32_t abs_x, int32_t abs_y);
static blur_sample_t sample_at(int32_t pos, int32_t factor, int32_t count, uint32_t channels);
#if !LV_DRAW_SW_COMPLEX
static uint32_t rounded_coverage(const lv_area_t * coords, int32_t radius, int32_t x, int32_t y);
#endif

/* Exact division for the 0..65152 range of tint, coverage and 565 packing.
 * Avoid a multiply-high or integer division in the ESP32's pixel loop. */
static inline uint32_t divide_255(uint32_t value)
{
    return (value + 1 + (value >> 8)) >> 8;
}

#if defined(CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST) && CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST
static atomic_bool s_solid_enabled = true;

void lv_draw_sw_blur_set_solid_enabled(bool enabled)
{
    atomic_store(&s_solid_enabled, enabled);
}

static bool solid_565(lv_draw_task_t * task, const lv_draw_blur_dsc_t * dsc, const lv_area_t * coords,
                      const lv_area_t * clipped, uint8_t * source, uint32_t stride, bool swapped,
                      int32_t radius, size_t map_bytes)
{
    if(!atomic_load(&s_solid_enabled)) return false;
    int32_t width = lv_area_get_width(clipped), height = lv_area_get_height(clipped);
    /* A sparse sample can miss a thin glyph or a moving edge. Prove the entire
     * clipped source is uniform before replacing its blur with the same solid
     * value (HIG §2.2). Row padding belongs to the layer, never the proof. */
    uint16_t pixel;
    memcpy(&pixel, source, sizeof(pixel));
    uint32_t pair = (uint32_t)pixel | ((uint32_t)pixel << 16);
    int32_t x = 0;
    for(; x + 1 < width; x += 2) {
        uint32_t value;
        /* A band region can begin at an odd X and its stride need not be
         * word-aligned. memcpy keeps the proof valid on both byte orders. */
        memcpy(&value, source + x * 2, sizeof(value));
        if(value != pair) return false;
    }
    if(x < width && (source[x * 2] != source[0] || source[x * 2 + 1] != source[1])) return false;
    for(int32_t y = 1; y < height; y++) {
        /* Compare only this clipped band region, not its padding or the whole
         * screen. libc's bulk compare makes black toolbar bands inexpensive
         * without a lossy sparse sample or an extra retained framebuffer. */
        if(memcmp(source + y * stride, source, (size_t)width * 2)) return false;
    }
    lv_blur_cache_entry_t * lease = NULL;
    /* A list can move while the fixed toolbar still samples true black. Its
     * complete uniformity proof permits exact reuse even during that motion:
     * the cache additionally compares every input byte, geometry, tint, format
     * and absolute dither phase. Reuse the existing bounded leased cache; a
     * failed reservation simply keeps the ordinary solid reconstruction. */
    if(!lv_obj_material_managed(dsc) &&
       lv_draw_sw_blur_cache_begin(task, dsc, coords, clipped, source, stride, 2, &lease) ==
       LV_BLUR_CACHE_HIT) {
        lv_obj_material_count_solid();
        return true;
    }
    lv_opa_t * mask = radius > 0 ? lv_malloc_draw(width, LV_DRAW_SCRATCH_BLUR) : NULL;
    if(radius > 0 && !mask) {
        lv_draw_sw_blur_cache_finish(lease, source, stride, false);
        return false;
    }
#if LV_DRAW_SW_COMPLEX
    lv_draw_sw_mask_radius_param_t round_mask;
    if(radius > 0 && lv_draw_sw_mask_radius_init(&round_mask, coords, radius, false) != LV_RESULT_OK) {
        lv_free_draw(mask, LV_DRAW_SCRATCH_BLUR);
        lv_draw_sw_blur_cache_finish(lease, source, stride, false);
        return false;
    }
    void * masks[] = {&round_mask, NULL};
#endif
    uint8_t rgb[3];
    read_pixel(source, rgb, 2, swapped);
    /* Publish the identical constant map once so settled controls can still
     * restore their complete material bases on later light-only updates. */
    lv_obj_material_store_solid(dsc, coords, clipped, source, stride, rgb, map_bytes);
    uint32_t tint = dsc->tint_opa > LV_OPA_MAX ? LV_OPA_COVER : dsc->tint_opa;
    const uint8_t tint_rgb[3] = {dsc->tint.blue, dsc->tint.green, dsc->tint.red};
    for(unsigned c = 0; c < 3; c++) {
        if(tint) rgb[c] = divide_255(rgb[c] * (255 - tint) + tint_rgb[c] * tint + 127);
    }
    for(int32_t y = 0; y < height; y++) {
        if(radius > 0) {
#if LV_DRAW_SW_COMPLEX
            lv_memset(mask, 255, width);
            if(lv_draw_sw_mask_apply(masks, mask, clipped->x1, clipped->y1 + y, width) ==
               LV_DRAW_SW_MASK_RES_TRANSP) continue;
#else
            for(int32_t x = 0; x < width; x++) {
                mask[x] = rounded_coverage(coords, radius, clipped->x1 + x, clipped->y1 + y);
            }
#endif
        }
        for(int32_t x = 0; x < width; x++) {
            uint32_t coverage = radius > 0 ? mask[x] : 255;
            if(coverage) write_pixel(source + y * stride + x * 2, rgb, 2, swapped, coverage,
                                    clipped->x1 + x, clipped->y1 + y);
        }
    }
#if LV_DRAW_SW_COMPLEX
    if(radius > 0) lv_draw_sw_mask_free_param(&round_mask);
#endif
    lv_free_draw(mask, LV_DRAW_SCRATCH_BLUR);
    lv_draw_sw_blur_cache_finish(lease, source, stride, true);
    lv_obj_material_count_solid();
    return true;
}
#endif

void lv_draw_sw_blur(lv_draw_task_t * t, const lv_draw_blur_dsc_t * dsc, const lv_area_t * coords)
{
    if(dsc->blur_radius <= 0) return;
#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
    if(s_fused_callback && s_fused_callback(t, dsc, coords)) return;
#endif

    lv_layer_t * layer = t->target_layer;
    lv_area_t clipped;
    if(!lv_area_intersect(&clipped, coords, &t->clip_area) ||
       !lv_area_intersect(&clipped, &clipped, &layer->buf_area)) return;

    uint32_t px_size = lv_color_format_get_size(layer->draw_buf->header.cf);
    if(px_size < 1 || px_size > 4) return;
    bool swapped = layer->draw_buf->header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED;
    uint32_t channels = px_size == 1 ? 1 : 3;
    int32_t width = lv_area_get_width(&clipped);
    int32_t height = lv_area_get_height(&clipped);
    int32_t factor = 1;
    uint32_t blur_radius = dsc->blur_radius;
    if(dsc->quality == LV_BLUR_QUALITY_AUTO) {
        if(blur_radius >= 32 && dsc->corner_radius == 0 && width * height > 160 * 160) factor = 3;
        else if(blur_radius >= 8) factor = 2;
    }
    else if(dsc->quality == LV_BLUR_QUALITY_SPEED) {
        factor = blur_radius >= 24 ? 3 : 2;
    }

    /* Motion changes the interior sampling resolution only. Reconstruction,
     * full-resolution rounded coverage and screen-anchored dither stay intact. */
    if(dsc->downsample_factor >= 2 && dsc->downsample_factor <= 4) factor = dsc->downsample_factor;

    int32_t small_w = (width + factor - 1) / factor;
    int32_t small_h = (height + factor - 1) / factor;
    int32_t small_stride = small_w * channels;
    int32_t radius = LV_MIN(dsc->corner_radius,
                           LV_MIN(lv_area_get_width(coords), lv_area_get_height(coords)) / 2);

    uint32_t stride = layer->draw_buf->header.stride;
    uint8_t * source = lv_draw_buf_goto_xy(layer->draw_buf, clipped.x1 - layer->buf_area.x1,
                                          clipped.y1 - layer->buf_area.y1);
#if defined(CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST) && CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST
    if(px_size == 2 && solid_565(t, dsc, coords, &clipped, source, stride, swapped, radius,
                                (size_t)small_stride * small_h)) return;
#endif
    lv_blur_cache_entry_t * lease = NULL;
    if(!dsc->backdrop_moving && !lv_obj_material_managed(dsc) && lv_draw_sw_blur_cache_begin(t, dsc, coords, &clipped, source, stride, px_size, &lease) ==
       LV_BLUR_CACHE_HIT) return;

    /* One scratch allocation holds the reduced image, compact X map, fractional
     * row and optional RGB565 lookup. The original remains intact until AA
     * compositing. Per-task storage keeps both draw workers independent. */
    size_t offsets_size = (size_t)width * sizeof(int32_t);
    size_t vertical_size = (size_t)small_stride * sizeof(uint16_t);
    /* A tiny invalidated strip costs less to blend directly than to build a
     * lookup table; ordinary controls amortize it over thousands of pixels. */
    size_t quant_size = px_size == 2 && (int64_t)width * height >= 1024 ? 3 * 256 * sizeof(uint16_t) : 0;
    size_t small_size = (size_t)small_stride * small_h;
    size_t scratch_size = offsets_size + vertical_size + quant_size + small_size + width + (radius > 0 ? width : 0);
    uint8_t * scratch = lv_malloc_draw(scratch_size, LV_DRAW_SCRATCH_BLUR);
    if(!scratch) {
        lv_draw_sw_blur_cache_finish(lease, source, stride, false);
        return;
    }
    int32_t * offsets = (int32_t *)scratch;
    uint16_t * vertical = (uint16_t *)(scratch + offsets_size);
    uint16_t * quant = quant_size ? (uint16_t *)(scratch + offsets_size + vertical_size) : NULL;
    uint8_t * small = scratch + offsets_size + vertical_size + quant_size;
    uint8_t * weights = small + small_size;
    lv_opa_t * mask = radius > 0 ? weights + width : NULL;

#if LV_DRAW_SW_COMPLEX
    lv_draw_sw_mask_radius_param_t round_mask;
    if(radius > 0 && lv_draw_sw_mask_radius_init(&round_mask, coords, radius, false) != LV_RESULT_OK) {
        lv_free_draw(scratch, LV_DRAW_SCRATCH_BLUR);
        lv_draw_sw_blur_cache_finish(lease, source, stride, false);
        return;
    }
    void * masks[] = {&round_mask, NULL};
#endif

    LV_PROFILER_DRAW_BEGIN;

    uint64_t preparation_started = lv_obj_material_preparation_begin();
    bool retained_filter = lv_obj_material_filter_get(t, dsc, coords, &clipped, source, stride,
                                                       px_size, small, small_size);
    if(!retained_filter) {
        lv_obj_material_count_kernel();
    /* Average every source pixel before reducing resolution. Skipping samples
     * aliases fine content into solid streaks; repeating the filtered pixels
     * later also made the curved glass edge visibly step by 2-3 pixels.
     * Replicate the last pixel at partial cells to keep a uniform sample grid. */
    if(px_size == 2 && factor == 3) downsample_565_3(small, source, width, height, stride, swapped);
    else if(px_size == 2 && factor == 4) downsample_565_4(small, source, width, height, stride, swapped);
    else {
        for(int32_t y = 0; y < small_h; y++) {
            for(int32_t x = 0; x < small_w; x++) {
                uint32_t sum[3] = {0, 0, 0};
                for(int32_t dy = 0; dy < factor; dy++) {
                    const uint8_t * row = source + LV_MIN(y * factor + dy, height - 1) * stride;
                    for(int32_t dx = 0; dx < factor; dx++) {
                        uint8_t rgb[3];
                        read_pixel(row + LV_MIN(x * factor + dx, width - 1) * px_size, rgb, px_size, swapped);
                        for(uint32_t c = 0; c < channels; c++) sum[c] += rgb[c];
                    }
                }
                uint32_t n = factor * factor;
                uint8_t * dst = small + y * small_stride + x * channels;
                for(uint32_t c = 0; c < channels; c++) dst[c] = (sum[c] + n / 2) / n;
            }
        }
    }

    /* The same four-direction IIR as the full-resolution path, now at >= 8 bits
     * per channel. Keep the fractional radius when reducing small blurs. */
    uint32_t intensity = (uint64_t)BLUR_INTENSITY_MAX * blur_radius / (blur_radius + 4 * factor);
    uint32_t sample_len = LV_MAX(blur_radius / (2 * factor), 1);
    for(int32_t x = 0; x < small_w; x++) {
        blur_line(small + x * channels, small_h, small_stride, channels, sample_len, intensity);
        blur_line(small + (small_h - 1) * small_stride + x * channels, small_h, -small_stride,
                  channels, sample_len, intensity);
    }
    for(int32_t y = 0; y < small_h; y++) {
        blur_line(small + y * small_stride, small_w, channels, channels, sample_len, intensity);
        blur_line(small + y * small_stride + (small_w - 1) * channels, small_w, -(int32_t)channels,
                  channels, sample_len, intensity);
    }

        lv_obj_material_filter_store(dsc, small, small_size);
    }

    lv_obj_material_preparation_end(preparation_started);

    /* Map pixel centres to the centres of the averaged cells. The X map avoids
     * divisions in the full-resolution inner loop. Clamp only at the bounds;
     * never round the clip inward or copy entire rows outside its mask. */
    for(int32_t x = 0; x < width; x++) {
        blur_sample_t sx = sample_at(x, factor, small_w, channels);
        offsets[x] = sx.offset;
        weights[x] = sx.weight;
    }
    uint32_t tint = dsc->tint_opa > LV_OPA_MAX ? LV_OPA_COVER : dsc->tint_opa;
    const uint8_t tint_rgb[3] = {dsc->tint.blue, dsc->tint.green, dsc->tint.red};
    if(quant) {
        /* Each entry holds the floor quantization and the Bayer threshold at
         * which it rounds up. Fold tint and quantization out of the opaque
         * interior loop; the thin AA edge still mixes with original pixels. */
        for(uint32_t c = 0; c < 3; c++) {
            for(uint32_t value = 0; value < 256; value++) {
                uint32_t tinted = tint ? divide_255(value * (255 - tint) + tint_rgb[c] * tint + 127) : value;
                uint32_t scaled = tinted * (c == 1 ? 63 : 31);
                uint32_t base = divide_255(scaled);
                quant[c * 256 + value] = (base << 8) | (255 - (scaled - base * 255));
            }
        }
    }

    for(int32_t y = 0; y < height; y++) {
        if(radius > 0) {
#if LV_DRAW_SW_COMPLEX
            lv_memset(mask, 255, width);
            if(lv_draw_sw_mask_apply(masks, mask, clipped.x1, clipped.y1 + y, width) ==
               LV_DRAW_SW_MASK_RES_TRANSP) continue;
#else
            for(int32_t x = 0; x < width; x++) {
                mask[x] = rounded_coverage(coords, radius, clipped.x1 + x, clipped.y1 + y);
            }
#endif
        }
        blur_sample_t sy = sample_at(y, factor, small_h, small_stride);
        const uint8_t * row0 = small + sy.offset;
        const uint8_t * row1 = row0 + (sy.weight ? small_stride : 0);
        /* Keep 8 fractional bits so separable interpolation is byte-exact
         * with four-tap bilinear reconstruction, without repeating its
         * vertical work for every output pixel in a downsampled cell. */
        for(int32_t i = 0; i < small_stride; i++) {
            vertical[i] = (int32_t)row0[i] * 256 + ((int32_t)row1[i] - row0[i]) * sy.weight;
        }
        uint8_t * dst = source + y * stride;
        if(px_size == 2) {
            reconstruct_565(dst, vertical, offsets, weights, mask, quant, width, tint, tint_rgb, swapped,
                            clipped.x1, clipped.y1 + y);
            continue;
        }
        for(int32_t x = 0; x < width; x++, dst += px_size) {
            uint32_t coverage = radius > 0 ? mask[x] : 255;
            if(coverage == 0) continue;
            blur_sample_t sx = {offsets[x], weights[x]};
            int32_t next = sx.offset + (sx.weight ? channels : 0);
            uint8_t rgb[3];
            for(uint32_t c = 0; c < channels; c++) {
                int32_t first = vertical[sx.offset + c];
                uint32_t value = (first * 256 + ((int32_t)vertical[next + c] - first) * sx.weight + 32768) >> 16;
                if(tint && channels == 3) value = divide_255(value * (255 - tint) + tint_rgb[c] * tint + 127);
                rgb[c] = value;
            }
            write_pixel(dst, rgb, px_size, swapped, coverage, clipped.x1 + x, clipped.y1 + y);
        }
    }

#if LV_DRAW_SW_COMPLEX
    if(radius > 0) lv_draw_sw_mask_free_param(&round_mask);
#endif
    lv_free_draw(scratch, LV_DRAW_SCRATCH_BLUR);
    lv_draw_sw_blur_cache_finish(lease, source, stride, true);
    LV_PROFILER_DRAW_END;
}

/* The device's SPEED glass uses 3x3 cells. Keep the cell count constant:
 * averaging has three fixed divisors and no per-pixel format/channel dispatch.
 * Red and blue share independent 16-bit sums; neither can carry into the other. */
static void downsample_565_3(uint8_t * small, const uint8_t * source, int32_t width, int32_t height,
                            uint32_t stride, bool swapped)
{
    for(int32_t y = 0; y < height; y += 3) {
        const uint8_t * rows[3] = {source + y * stride,
            source + LV_MIN(y + 1, height - 1) * stride, source + LV_MIN(y + 2, height - 1) * stride};
        for(int32_t x = 0; x < width; x += 3) {
            uint32_t rb_sum = 0, g_sum = 0;
            for(int32_t dy = 0; dy < 3; dy++) {
                for(int32_t dx = 0; dx < 3; dx++) {
                    const uint8_t * src = rows[dy] + LV_MIN(x + dx, width - 1) * 2;
                    union { uint16_t value; uint8_t bytes[2]; } packed;
                    packed.bytes[0] = src[0];
                    packed.bytes[1] = src[1];
                    uint32_t pixel = swapped ? lv_color_swap_16(packed.value) : packed.value;
                    uint32_t rb = (pixel & 31) | ((pixel & 0xF800) << 5);
                    rb_sum += (rb << 3) | ((rb >> 2) & 0x00070007);
                    uint32_t g = (pixel >> 5) & 63;
                    g_sum += (g << 2) | (g >> 4);
                }
            }
            *small++ = ((rb_sum & 0xFFFF) + 4) / 9;
            *small++ = (g_sum + 4) / 9;
            *small++ = ((rb_sum >> 16) + 4) / 9;
        }
    }
}

/* Quarter-resolution cells retain every backdrop sample. Fixed 4x4 sums use
 * shifts instead of per-channel divisions; edge cells replicate their bounds. */
static void downsample_565_4(uint8_t * small, const uint8_t * source, int32_t width, int32_t height,
                            uint32_t stride, bool swapped)
{
    for(int32_t y = 0; y < height; y += 4) {
        const uint8_t * rows[4] = {source + y * stride,
            source + LV_MIN(y + 1, height - 1) * stride, source + LV_MIN(y + 2, height - 1) * stride,
            source + LV_MIN(y + 3, height - 1) * stride};
        for(int32_t x = 0; x < width; x += 4) {
            uint32_t rb_sum = 0, g_sum = 0;
            for(int32_t dy = 0; dy < 4; dy++) {
                for(int32_t dx = 0; dx < 4; dx++) {
                    const uint8_t * src = rows[dy] + LV_MIN(x + dx, width - 1) * 2;
                    union { uint16_t value; uint8_t bytes[2]; } packed;
                    packed.bytes[0] = src[0];
                    packed.bytes[1] = src[1];
                    uint32_t pixel = swapped ? lv_color_swap_16(packed.value) : packed.value;
                    uint32_t rb = (pixel & 31) | ((pixel & 0xF800) << 5);
                    rb_sum += (rb << 3) | ((rb >> 2) & 0x00070007);
                    uint32_t g = (pixel >> 5) & 63;
                    g_sum += (g << 2) | (g >> 4);
                }
            }
            *small++ = ((rb_sum & 0xFFFF) + 8) >> 4;
            *small++ = (g_sum + 8) >> 4;
            *small++ = ((rb_sum >> 16) + 8) >> 4;
        }
    }
}

/* RGB565 is the device's output. Keep its three channels and pixel size
 * constant so the hot loop does not dispatch on format for every pixel. */
static void reconstruct_565(uint8_t * dst, const uint16_t * vertical, const int32_t * offsets, const uint8_t * weights,
                            const lv_opa_t * mask, const uint16_t * quant, int32_t width, uint32_t tint,
                            const uint8_t * tint_rgb, bool swapped, int32_t abs_x, int32_t abs_y)
{
    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };
    uint32_t tint_b = tint_rgb[0] * tint + 127;
    uint32_t tint_g = tint_rgb[1] * tint + 127;
    uint32_t tint_r = tint_rgb[2] * tint + 127;
    uint32_t inv_tint = 255 - tint;
    for(int32_t x = 0; x < width; x++, dst += 2) {
        uint32_t coverage = mask ? mask[x] : 255;
        if(coverage == 0) continue;
        blur_sample_t sx = {offsets[x], weights[x]};
        const uint16_t * left = vertical + sx.offset;
        const uint16_t * right = left + (sx.weight ? 3 : 0);
        int32_t b = (left[0] * 256 + ((int32_t)right[0] - left[0]) * sx.weight + 32768) >> 16;
        int32_t g = (left[1] * 256 + ((int32_t)right[1] - left[1]) * sx.weight + 32768) >> 16;
        int32_t r = (left[2] * 256 + ((int32_t)right[2] - left[2]) * sx.weight + 32768) >> 16;
        uint32_t threshold = bayer[abs_y & 3][(abs_x + x) & 3] * 16 + 8;
        if(coverage == 255 && quant) {
            uint32_t qb = quant[b], qg = quant[256 + g], qr = quant[512 + r];
            b = (qb >> 8) + (threshold >= (qb & 255));
            g = (qg >> 8) + (threshold >= (qg & 255));
            r = (qr >> 8) + (threshold >= (qr & 255));
            union { uint16_t value; uint8_t bytes[2]; } packed;
            packed.value = (r << 11) | (g << 5) | b;
            if(swapped) packed.value = lv_color_swap_16(packed.value);
            dst[0] = packed.bytes[0];
            dst[1] = packed.bytes[1];
            continue;
        }
        if(tint) {
            b = divide_255(b * inv_tint + tint_b);
            g = divide_255(g * inv_tint + tint_g);
            r = divide_255(r * inv_tint + tint_r);
        }
        if(coverage < 255) {
            union { uint16_t value; uint8_t bytes[2]; } packed;
            packed.bytes[0] = dst[0];
            packed.bytes[1] = dst[1];
            uint32_t old = swapped ? lv_color_swap_16(packed.value) : packed.value;
            uint32_t old_b = old & 31, old_g = (old >> 5) & 63, old_r = old >> 11;
            old_b = (old_b << 3) | (old_b >> 2);
            old_g = (old_g << 2) | (old_g >> 4);
            old_r = (old_r << 3) | (old_r >> 2);
            b = divide_255(b * coverage + old_b * (255 - coverage) + 127);
            g = divide_255(g * coverage + old_g * (255 - coverage) + 127);
            r = divide_255(r * coverage + old_r * (255 - coverage) + 127);
        }
        union { uint16_t value; uint8_t bytes[2]; } packed;
        packed.value = (divide_255(r * 31 + threshold) << 11) |
                       (divide_255(g * 63 + threshold) << 5) | divide_255(b * 31 + threshold);
        if(swapped) packed.value = lv_color_swap_16(packed.value);
        dst[0] = packed.bytes[0];
        dst[1] = packed.bytes[1];
    }
}

static void blur_line(uint8_t * buf, int32_t count, int32_t step, uint32_t channels,
                      uint32_t sample_len, uint32_t intensity)
{
    uint32_t sum[3] = {0, 0, 0};
    sample_len = LV_MIN(sample_len, (uint32_t)count);
    const uint8_t * sample = buf;
    for(uint32_t i = 0; i < sample_len; i++) {
        for(uint32_t c = 0; c < channels; c++) sum[c] += sample[c];
        if(i + 1 < sample_len) sample += step;
    }
    for(uint32_t c = 0; c < channels; c++) sum[c] = ((uint64_t)sum[c] << BLUR_INTENSITY_BITS) / sample_len;
    for(int32_t i = 0; i < count; i++) {
        for(uint32_t c = 0; c < channels; c++) {
            sum[c] = ((sum[c] * intensity) >> BLUR_INTENSITY_BITS) + buf[c] * (BLUR_INTENSITY_MAX - intensity);
            buf[c] = (sum[c] + BLUR_INTENSITY_HALF) >> BLUR_INTENSITY_BITS;
        }
        if(i + 1 < count) buf += step;
    }
}

static blur_sample_t sample_at(int32_t pos, int32_t factor, int32_t count, uint32_t channels)
{
    int32_t fixed = ((2 * pos + 1 - factor) * 128) / factor;
    fixed = LV_CLAMP(0, fixed, (count - 1) * 256);
    blur_sample_t sample = {(fixed >> 8) * channels, fixed & 255};
    return sample;
}

static inline void read_pixel(const uint8_t * src, uint8_t * rgb, uint32_t px_size, bool swapped)
{
    if(px_size == 2) {
        /* Byte access also supports unaligned strides without a memcpy call
         * for every pixel in the firmware's CLIB string implementation. */
        union { uint16_t value; uint8_t bytes[2]; } packed;
        packed.bytes[0] = src[0];
        packed.bytes[1] = src[1];
        uint16_t pixel = packed.value;
        if(swapped) pixel = lv_color_swap_16(pixel);
        uint32_t b = pixel & 31;
        uint32_t g = (pixel >> 5) & 63;
        uint32_t r = pixel >> 11;
        rgb[0] = (b << 3) | (b >> 2);
        rgb[1] = (g << 2) | (g >> 4);
        rgb[2] = (r << 3) | (r >> 2);
    }
    else {
        rgb[0] = src[0];
        if(px_size >= 3) {
            rgb[1] = src[1];
            rgb[2] = src[2];
        }
    }
}

static inline void write_pixel(uint8_t * dst, const uint8_t * rgb, uint32_t px_size, bool swapped,
                       uint32_t coverage, int32_t x, int32_t y)
{
    uint8_t result[3];
    uint32_t channels = px_size == 1 ? 1 : 3;
    if(coverage < 255) {
        uint8_t original[3];
        read_pixel(dst, original, px_size, swapped);
        for(uint32_t c = 0; c < channels; c++) {
            result[c] = divide_255(rgb[c] * coverage + original[c] * (255 - coverage) + 127);
        }
        rgb = result;
    }
    if(px_size == 2) {
        /* Screen-anchored Bayer noise avoids dark RGB565 bands and does not
         * shimmer when a partial buffer starts on a different scanline. */
        static const uint8_t bayer[4][4] = {
            {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
        };
        uint32_t threshold = bayer[y & 3][x & 3] * 16 + 8;
        uint32_t b = divide_255(rgb[0] * 31 + threshold);
        uint32_t g = divide_255(rgb[1] * 63 + threshold);
        uint32_t r = divide_255(rgb[2] * 31 + threshold);
        union { uint16_t value; uint8_t bytes[2]; } packed;
        packed.value = (r << 11) | (g << 5) | b;
        if(swapped) packed.value = lv_color_swap_16(packed.value);
        dst[0] = packed.bytes[0];
        dst[1] = packed.bytes[1];
    }
    else {
        for(uint32_t c = 0; c < channels; c++) dst[c] = rgb[c];
        /* XRGB/ARGB's fourth byte belongs to the layer, not the backdrop blur. */
    }
}

#if !LV_DRAW_SW_COMPLEX
static uint32_t rounded_coverage(const lv_area_t * coords, int32_t radius, int32_t x, int32_t y)
{
    int32_t dx = LV_MAX(2 * (coords->x1 + radius - x) - 1, 2 * (x - coords->x2 + radius) - 1);
    int32_t dy = LV_MAX(2 * (coords->y1 + radius - y) - 1, 2 * (y - coords->y2 + radius) - 1);
    if(dx <= 0 || dy <= 0) return 255;
    /* One-pixel coverage ramp, for builds without LVGL's complex mask cache. */
    int64_t distance = (int64_t)dx * dx + (int64_t)dy * dy;
    int64_t inner = (int64_t)(2 * radius - 1) * (2 * radius - 1);
    int64_t outer = (int64_t)(2 * radius + 1) * (2 * radius + 1);
    if(distance <= inner) return 255;
    if(distance >= outer) return 0;
    return (outer - distance) * 255 / (outer - inner);
}
#endif

#endif /*LV_USE_DRAW_SW*/
