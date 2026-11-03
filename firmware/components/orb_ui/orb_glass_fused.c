/* Bounded reduced glass, HIG §2.2. Source presentation is sampled once, with a
 * halo, before this control is drawn. Every band consumes that immutable map.
 * There is no full-resolution capture or reconstructed material in PSRAM. */
#include "orb_glass_fused.h"

#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#include "orb_ui_private.h"
#include "orb_ui.h"
#include <stdatomic.h>
#include "src/core/lv_obj_private.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw_mask_private.h"
#include "src/misc/lv_area_private.h"
#include "include/lvgl/draw/lv_draw_sw_profile.h"

#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
#define HALO 16
#define SOURCES 48
#define MAP_BUDGET (12u * 1024u)
#define MAX_SMALL_SIDE 128

typedef struct {
    lv_obj_t *object;
    lv_area_t bounds, clip;
    lv_color_t fill;
    lv_opa_t opacity;
    bool raster;
} source_t;

static size_t s_map_bytes;
static size_t s_map_peak;
static unsigned s_maps;
static atomic_uint s_prepares, s_draws, s_fallbacks;

void orb_glass_fused_note_fallback(void)
{
    atomic_fetch_add(&s_fallbacks, 1);
}

void orb_glass_fused_get_stats(orb_glass_fused_stats_t *out)
{
    if (!out) return;
    *out = (orb_glass_fused_stats_t){.prepares = atomic_load(&s_prepares), .draws = atomic_load(&s_draws),
        .fallbacks = atomic_load(&s_fallbacks), .bytes = s_map_bytes, .peak_bytes = s_map_peak,
        .budget_bytes = MAP_BUDGET, .maps = s_maps};
}

static void *internal_alloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    /* This candidate must earn the same 12KiB internal headroom gate as the
     * reference build. Low memory keeps the reference path available. */
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < bytes + 12u * 1024u) return NULL;
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return lv_malloc(bytes);
#endif
}

static void internal_free(void *pointer)
{
#ifdef ESP_PLATFORM
    heap_caps_free(pointer);
#else
    lv_free(pointer);
#endif
}

static uint32_t div255(uint32_t value)
{
    return (value + 1 + (value >> 8)) >> 8;
}

/* Reject unknown painting rather than guessing its contribution to the halo.
 * Raster roots already include custom painting and are atomic content sources. */
static bool collect(lv_obj_t *object, lv_obj_t *stop, const lv_area_t *sample,
                    lv_area_t clip, source_t *sources, unsigned *count, bool *found)
{
    if (object == stop) { *found = true; return true; }
    if (lv_obj_is_hidden(object)) return true;
    lv_area_t bounds;
    lv_obj_get_coords(object, &bounds);
    bool intersects = lv_area_is_on(&bounds, sample) && lv_area_is_on(&clip, sample);
    bool raster = lv_obj_get_raster(object);
    if (intersects) {
        if (lv_obj_get_layer_type(object) != LV_LAYER_TYPE_NONE || object->has_blur ||
            lv_obj_get_style_clip_corner(object, 0) || lv_obj_get_style_bitmap_mask_src(object, 0)) return false;
        if (!raster) {
            for (uint32_t i = 0; i < lv_obj_get_event_count(object); i++) {
                lv_event_code_t code = lv_obj_get_event_dsc(object, i)->filter & ~LV_EVENT_PREPROCESS;
                if (code == LV_EVENT_ALL || (code >= LV_EVENT_DRAW_MAIN_BEGIN && code <= LV_EVENT_DRAW_POST_END)) return false;
            }
            /* Transparent containers and plain square fills are supported;
             * labels, images, borders and effects use LVGL's reference path. */
            if (!lv_obj_check_type(object, &lv_obj_class) ||
                lv_obj_get_style_bg_grad_dir(object, 0) != LV_GRAD_DIR_NONE ||
                lv_obj_get_style_bg_image_src(object, 0) ||
                lv_obj_get_style_border_width(object, 0) ||
                lv_obj_get_style_shadow_width(object, 0) ||
                lv_obj_get_style_outline_width(object, 0) ||
                lv_obj_get_style_blend_mode(object, 0) != LV_BLEND_MODE_NORMAL) return false;
        }
        lv_opa_t opacity = lv_obj_get_style_opa_recursive(object, 0);
        /* A raster's compositor owns its opacity independently of the object's
         * ordinary style. Layered/style alpha needs the reference compositor. */
        if (raster && (opacity != LV_OPA_COVER || lv_obj_get_ext_draw_size(object) ||
                       !lv_obj_raster_sample_ready(object))) return false;
        lv_opa_t fill_opacity = lv_obj_get_style_bg_opa(object, 0);
        if (!raster && opacity > LV_OPA_MIN && fill_opacity > LV_OPA_MIN &&
            lv_color_to_u16(lv_obj_get_style_bg_color(object, 0)) != 0) return false;
        if (!raster && opacity > LV_OPA_MIN && fill_opacity > LV_OPA_MIN) {
            /* A later black overlay can erase earlier content. Only the
             * initial/disjoint black canvas can be omitted from an additive
             * source integral; other painter order uses the reference path. */
            lv_area_t visible;
            if (lv_area_intersect(&visible, &bounds, &clip) && lv_area_intersect(&visible, &visible, sample)) {
                for (unsigned i = 0; i < *count; i++) {
                    lv_area_t previous;
                    if (lv_area_intersect(&previous, &sources[i].bounds, &sources[i].clip) &&
                        lv_area_is_on(&previous, &visible)) return false;
                }
            }
        }
        if (raster) {
            if (*count == SOURCES) return false;
            /* The fast scene proof is disjoint retained sources over true
             * black. Their premultiplied area integrals add exactly. Averaging
             * overlapping alpha layers independently would darken cell seams. */
            lv_area_t visible;
            if (!lv_area_intersect(&visible, &bounds, &clip) || !lv_area_intersect(&visible, &visible, sample)) return true;
            for (unsigned i = 0; i < *count; i++) {
                lv_area_t previous;
                if (lv_area_intersect(&previous, &sources[i].bounds, &sources[i].clip) &&
                    lv_area_is_on(&previous, &visible)) return false;
            }
            sources[(*count)++] = (source_t){object, bounds, clip,
                lv_obj_get_style_bg_color(object, 0),
                raster ? opacity : div255(opacity * fill_opacity + 127), raster};
        }
    }
    if (raster) return true;
    if (!lv_obj_is_overflow_visible(object) && !lv_area_intersect(&clip, &clip, &bounds)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        if (!collect(lv_obj_get_child(object, i), stop, sample, clip, sources, count, found)) return false;
        if (*found) return true;
    }
    return true;
}

static lv_color32_t scene_cell(const source_t *sources, unsigned count, const lv_area_t *cell)
{
    lv_color32_t result = {.alpha = 255};
    unsigned cell_area = lv_area_get_width(cell) * lv_area_get_height(cell);
    for (unsigned i = 0; i < count; i++) {
        const source_t *s = &sources[i];
        lv_area_t clipped;
        if (!lv_area_intersect(&clipped, cell, &s->clip) || !lv_area_intersect(&clipped, &clipped, &s->bounds)) continue;
        lv_color32_t color;
        if (s->raster) {
            if (!lv_obj_raster_sample_area(s->object, &clipped, &color)) continue;
        } else {
            color = lv_color_to_32(s->fill, s->opacity);
        }
        unsigned covered = lv_area_get_width(&clipped) * lv_area_get_height(&clipped);
        color.alpha = (color.alpha * covered + cell_area / 2) / cell_area;
        result.blue = LV_MIN(255u, result.blue + div255(color.blue * color.alpha + 127));
        result.green = LV_MIN(255u, result.green + div255(color.green * color.alpha + 127));
        result.red = LV_MIN(255u, result.red + div255(color.red * color.alpha + 127));
    }
    return result;
}

/* Four directional fixed-point IIR preserves the reference filter's radius
 * while reducing its cell count. Filter storage is internal and >=8b/channel. */
static void filter_line(uint8_t *p, int32_t count, int32_t step, unsigned intensity)
{
    unsigned sum[3] = {p[0] << 12, p[1] << 12, p[2] << 12};
    for (int32_t i = 0; i < count; i++) {
        for (unsigned c = 0; c < 3; c++) {
            sum[c] = ((sum[c] * intensity) >> 12) + p[c] * (4096 - intensity);
            p[c] = (sum[c] + 2048) >> 12;
        }
        if (i + 1 < count) p += step;
    }
}

static bool prepare_internal(orb_glass_fused_t *map, lv_obj_t *obj, const lv_area_t *shape, int32_t radius)
{
    uint64_t started = lv_draw_sw_profile_stage_begin();
    source_t *sources = internal_alloc(sizeof(*sources) * SOURCES);
    if (!sources) return false;
    unsigned count = 0;
    bool found = false;
    lv_area_t screen = {0, 0, lv_display_get_horizontal_resolution(lv_obj_get_display(obj)) - 1,
        lv_display_get_vertical_resolution(lv_obj_get_display(obj)) - 1};
    lv_area_t sample = *shape;
    lv_area_increase(&sample, HALO, HALO);
    lv_obj_t *root = obj;
    while (lv_obj_get_parent(root)) root = lv_obj_get_parent(root);
    if (!collect(root, obj, &sample, screen, sources, &count, &found) || !found) {
        internal_free(sources); return false;
    }
    unsigned factor = lv_area_get_width(shape) <= 96 ? 4 : 8;
    unsigned width = (lv_area_get_width(&sample) + factor - 1) / factor;
    unsigned height = (lv_area_get_height(&sample) + factor - 1) / factor;
    if (width > MAX_SMALL_SIDE || height > MAX_SMALL_SIDE) { internal_free(sources); return false; }
    size_t bytes = width * height * 3;
    if (map->capacity < bytes) {
        if (s_map_bytes - map->capacity + bytes > MAP_BUDGET) { internal_free(sources); return false; }
        /* No worker can still use the old map at this UI-thread frame seam.
         * Release it first so replacement never transiently doubles charge. */
        s_map_bytes -= map->capacity;
        if (map->pixels) s_maps--;
        internal_free(map->pixels);
        map->pixels = NULL;
        map->capacity = 0;
        map->pixels = internal_alloc(bytes);
        if (!map->pixels) { internal_free(sources); return false; }
        map->capacity = bytes;
        s_map_bytes += bytes;
        s_maps++;
        if (s_map_bytes > s_map_peak) s_map_peak = s_map_bytes;
    }
    map->sample = sample;
    map->width = width;
    map->height = height;
    map->factor = factor;
    uint64_t downsample_started = lv_draw_sw_profile_stage_begin();
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            lv_area_t cell = {sample.x1 + (int32_t)(x * factor), sample.y1 + (int32_t)(y * factor),
                sample.x1 + (int32_t)((x + 1) * factor) - 1, sample.y1 + (int32_t)((y + 1) * factor) - 1};
            lv_color32_t color = scene_cell(sources, count, &cell);
            uint8_t *p = map->pixels + (y * width + x) * 3;
            p[0] = color.blue; p[1] = color.green; p[2] = color.red;
        }
    }
    internal_free(sources);
    lv_draw_sw_profile_stage_end(LV_DRAW_SW_PROFILE_GLASS_DOWNSAMPLE, downsample_started, width * height);
    uint64_t filter_started = lv_draw_sw_profile_stage_begin();
    unsigned intensity = (uint64_t)4096 * radius / (radius + 4 * factor);
    for (unsigned x = 0; x < width; x++) {
        filter_line(map->pixels + x * 3, height, width * 3, intensity);
        filter_line(map->pixels + ((height - 1) * width + x) * 3, height, -(int32_t)(width * 3), intensity);
    }
    for (unsigned y = 0; y < height; y++) {
        filter_line(map->pixels + y * width * 3, width, 3, intensity);
        filter_line(map->pixels + (y * width + width - 1) * 3, width, -3, intensity);
    }
    lv_draw_sw_profile_stage_end(LV_DRAW_SW_PROFILE_GLASS_FILTER, filter_started, width * height);
    lv_draw_sw_profile_stage_end(LV_DRAW_SW_PROFILE_GLASS_PREPARE, started, width * height);
    atomic_fetch_add(&s_prepares, 1);
    return true;
}

bool orb_glass_fused_prepare(orb_glass_fused_t *map, lv_obj_t *obj, const lv_area_t *shape, int32_t radius)
{
    bool prepared = prepare_internal(map, obj, shape, radius);
    if (!prepared) atomic_fetch_add(&s_fallbacks, 1);
    return prepared;
}

void orb_glass_fused_release(orb_glass_fused_t *map)
{
    s_map_bytes -= map->capacity;
    if (map->pixels) s_maps--;
    internal_free(map->pixels);
    memset(map, 0, sizeof(*map));
}

static unsigned quantize(unsigned value, unsigned levels, unsigned threshold)
{
    return div255(value * levels + threshold);
}

#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
extern void orb_glass_fused_interpolate8_s3(int16_t *out, const int16_t *left,
                                           const int16_t *right, const int16_t *weights);
#else
static void orb_glass_fused_interpolate8_s3(int16_t *out, const int16_t *left,
                                           const int16_t *right, const int16_t *weights)
{
    /* Host oracle for PIE's signed multiply and arithmetic shift. Two-stage
     * bilinear interpolation differs by <=1 level from the reference's Q16
     * rounding; output quality is checked by PSNR and magnified crops. */
    for (unsigned i = 0; i < 8; i++) out[i] = left[i] + (((right[i] - left[i]) * weights[i]) >> 8);
}
#endif

static inline void interpolate_block(const orb_glass_fused_t *map, const uint16_t *vertical,
    int32_t first_fx, int32_t step_fx, int32_t block, int16_t interpolated[3][8],
    int16_t left_values[8], int16_t right_values[8], int16_t weights[8])
{
    for (unsigned c = 0; c < 3; c++) {
        for (unsigned lane = 0; lane < 8; lane++) {
            int32_t fx = LV_CLAMP(0, first_fx + (block + (int32_t)lane) * step_fx, (map->width - 1) * 256);
            const uint16_t *left = vertical + (fx >> 8) * 3;
            const uint16_t *right = left + ((fx & 255) ? 3 : 0);
            left_values[lane] = (left[c] + 128) >> 8;
            right_values[lane] = (right[c] + 128) >> 8;
            weights[lane] = fx & 255;
        }
        orb_glass_fused_interpolate8_s3(interpolated[c], left_values, right_values, weights);
    }
}

static void quantize_table(uint16_t *quant, unsigned tint, const unsigned tint_rgb[3])
{
    for (unsigned c = 0; c < 3; c++) for (unsigned value = 0; value < 256; value++) {
        unsigned tinted = div255(value * (255 - tint) + tint_rgb[c] * tint + 127);
        unsigned scaled = tinted * (c == 1 ? 63 : 31), base = div255(scaled);
        quant[c * 256 + value] = (base << 8) | (255 - (scaled - base * 255));
    }
}

static bool draw_internal(const orb_glass_fused_t *map, lv_draw_task_t *task,
                          const lv_draw_blur_dsc_t *dsc, const lv_area_t *shape,
                          const lv_draw_buf_t *dark, const lv_draw_buf_t *rim,
                          int32_t shadow_pad, lv_opa_t amount)
{
    lv_layer_t *layer = task->target_layer;
    lv_color_format_t format = layer->draw_buf->header.cf;
    if (format != LV_COLOR_FORMAT_RGB565 && format != LV_COLOR_FORMAT_RGB565_SWAPPED) return false;
    lv_area_t clip;
    if (!lv_area_intersect(&clip, shape, &task->clip_area) || !lv_area_intersect(&clip, &clip, &layer->buf_area)) return true;
    int32_t width = lv_area_get_width(&clip), shape_w = lv_area_get_width(shape);
    if (width > 466 || shape_w != (int32_t)rim->header.w || lv_area_get_height(shape) != (int32_t)rim->header.h) return false;
    /* One scanline of the shared PSRAM masks is staged internally. Full-output
     * reconstruction, tint, SDF edge/rim, coverage and Bayer write are fused. */
    size_t scratch_bytes = ((width * 3 + 1) & ~1) + map->width * 3 * sizeof(uint16_t) + 3 * 256 * sizeof(uint16_t);
    uint8_t *scratch = internal_alloc(scratch_bytes);
    if (!scratch) return false;
    lv_opa_t *coverage = scratch, *dark_row = scratch + width, *rim_row = scratch + width * 2;
    uint16_t *vertical = (uint16_t *)(scratch + ((width * 3 + 1) & ~1));
    uint16_t *quant = vertical + map->width * 3;
    lv_draw_sw_mask_radius_param_t mask;
    if (lv_draw_sw_mask_radius_init(&mask, shape, dsc->corner_radius, false) != LV_RESULT_OK) {
        internal_free(scratch); return false;
    }
    void *masks[] = {&mask, NULL};
    static const uint8_t bayer[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    unsigned tint = dsc->tint_opa > LV_OPA_MAX ? 255 : dsc->tint_opa;
    const unsigned tint_rgb[3] = {dsc->tint.blue, dsc->tint.green, dsc->tint.red};
    bool swapped = format == LV_COLOR_FORMAT_RGB565_SWAPPED;
    uint64_t started = lv_draw_sw_profile_stage_begin();
    /* Most material pixels are opaque, unlit interiors. Fold tint and RGB565
     * quantization out of that loop, as the reference blur does. The small LUT
     * shares this band's internal scratch and is never retained in PSRAM. */
    quantize_table(quant, tint, tint_rgb);
    int32_t first_fx = ((2 * (clip.x1 - map->sample.x1) + 1 - map->factor) * 128) / map->factor;
    int32_t step_fx = 256 / map->factor;
    for (int32_t y = clip.y1; y <= clip.y2; y++) {
        memset(coverage, 255, width);
        if (lv_draw_sw_mask_apply(masks, coverage, clip.x1, y, width) == LV_DRAW_SW_MASK_RES_TRANSP)
            memset(coverage, 0, width);
        int32_t my = y - shape->y1, mx = clip.x1 - shape->x1;
        memcpy(dark_row, dark->data + (my + shadow_pad) * dark->header.stride + mx + shadow_pad, width);
        memcpy(rim_row, rim->data + my * rim->header.stride + mx, width);
        int32_t fy = LV_CLAMP(0, ((2 * (y - map->sample.y1) + 1 - map->factor) * 128) / map->factor,
                              (map->height - 1) * 256);
        const uint8_t *top = map->pixels + (fy >> 8) * map->width * 3;
        const uint8_t *bottom = top + ((fy & 255) ? map->width * 3 : 0);
        for (unsigned i = 0; i < map->width * 3; i++) vertical[i] = top[i] * 256 + ((int)bottom[i] - top[i]) * (fy & 255);
        uint8_t *destination = lv_draw_buf_goto_xy(layer->draw_buf, clip.x1 - layer->buf_area.x1, y - layer->buf_area.y1);
        for (int32_t block = 0; block < width; block += 8) {
            int16_t interpolated[3][8] __attribute__((aligned(16)));
            int16_t left_values[8] __attribute__((aligned(16)));
            int16_t right_values[8] __attribute__((aligned(16)));
            int16_t weights[8] __attribute__((aligned(16)));
            interpolate_block(map, vertical, first_fx, step_fx, block,
                              interpolated, left_values, right_values, weights);
            for (int32_t x = block; x < LV_MIN(block + 8, width); x++) {
                unsigned alpha = coverage[x];
                if (!alpha && !dark_row[x] && !rim_row[x]) continue;
                unsigned threshold = bayer[y & 3][(clip.x1 + x) & 3] * 16 + 8;
                if (alpha == 255 && !dark_row[x] && !rim_row[x]) {
                    unsigned qb = quant[interpolated[0][x - block]],
                        qg = quant[256 + interpolated[1][x - block]], qr = quant[512 + interpolated[2][x - block]];
                    union { uint16_t value; uint8_t bytes[2]; } packed;
                    packed.value = (((qr >> 8) + (threshold >= (qr & 255))) << 11) |
                        (((qg >> 8) + (threshold >= (qg & 255))) << 5) |
                        ((qb >> 8) + (threshold >= (qb & 255)));
                    if (swapped) packed.value = lv_color_swap_16(packed.value);
                    destination[x * 2] = packed.bytes[0];
                    destination[x * 2 + 1] = packed.bytes[1];
                    continue;
                }
                unsigned rgb[3];
                unsigned edge = div255(dark_row[x] * amount + 127), light = div255(rim_row[x] * amount + 127);
                union { uint16_t value; uint8_t bytes[2]; } packed;
                packed.bytes[0] = destination[x * 2]; packed.bytes[1] = destination[x * 2 + 1];
                uint16_t old = packed.value;
                if (swapped) old = lv_color_swap_16(old);
                const unsigned old_rgb[3] = {((old & 31) << 3) | ((old & 31) >> 2),
                    (((old >> 5) & 63) << 2) | (((old >> 5) & 63) >> 4), ((old >> 11) << 3) | ((old >> 11) >> 2)};
                for (unsigned c = 0; c < 3; c++) {
                    unsigned value = interpolated[c][x - block];
                    value = div255(value * (255 - tint) + tint_rgb[c] * tint + 127);
                    if (alpha < 255) value = div255(value * alpha + old_rgb[c] * (255 - alpha) + 127);
                    if (edge) value = div255(value * (255 - edge) + 127);
                    rgb[c] = light ? div255(value * (255 - light) + 255 * light + 127) : value;
                }
                packed.value = (quantize(rgb[2], 31, threshold) << 11) |
                    (quantize(rgb[1], 63, threshold) << 5) | quantize(rgb[0], 31, threshold);
                if (swapped) packed.value = lv_color_swap_16(packed.value);
                destination[x * 2] = packed.bytes[0];
                destination[x * 2 + 1] = packed.bytes[1];
            }
        }
    }
    lv_draw_sw_profile_stage_end(LV_DRAW_SW_PROFILE_GLASS_COMPOSE, started, width * lv_area_get_height(&clip));
    lv_draw_sw_mask_free_param(&mask);
    internal_free(scratch);
    atomic_fetch_add(&s_draws, 1);
    return true;
}

bool orb_glass_fused_draw(const orb_glass_fused_t *map, lv_draw_task_t *task,
                          const lv_draw_blur_dsc_t *dsc, const lv_area_t *shape,
                          const lv_draw_buf_t *dark, const lv_draw_buf_t *rim,
                          int32_t shadow_pad, lv_opa_t amount)
{
    bool drawn = draw_internal(map, task, dsc, shape, dark, rim, shadow_pad, amount);
    if (!drawn) atomic_fetch_add(&s_fallbacks, 1);
    return drawn;
}
#else
void orb_glass_fused_get_stats(orb_glass_fused_stats_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}
#endif
