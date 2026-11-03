/**
 * @file lv_obj_raster.c
 *
 * ORB PATCH: raster cache (see lv_obj_raster.h and ORB_PATCHES.md).
 *
 * Opaque matte content is rendered directly into retained RGB565. Transparent
 * content is drawn once into a bounded ARGB8888 bitmap, then split into parts by how
 * they are drawn later: rectangles where every pixel is opaque go into a plane
 * in the display's color format (drawing them is a plain copy, which the SW
 * renderer can do with SIMD), and the rest, typically the rounded corners and
 * anti-aliased edges, stays ARGB8888 and is blended. Fully transparent
 * rectangles are dropped. The ARGB8888 bitmap is freed once split.
 */

/*********************
 *      INCLUDES
 *********************/
#include "orb_memory.h"
#include "../misc/lv_area_private.h"
#include "../lvgl_public.h"
#include "../display/lv_display_private.h"
#include "../draw/lv_draw_private.h"
#include "../draw/lv_draw_buf_private.h"
#include "lv_obj_draw_private.h"
#include "lv_obj_private.h"
#include "lv_obj_raster_private.h"
#include "lv_obj_rows_private.h"
#include "lv_obj_material_private.h"
#include "lv_refr_private.h"
#include "../draw/sw/lv_draw_sw.h"
#include "../draw/lv_draw_image_private.h"
#include "../../include/lvgl/draw/lv_draw_sw_profile.h"

#ifndef CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
#define CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE 0
#endif
#ifndef CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE_KIB
#define CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE_KIB 512
#endif

/*********************
 *      DEFINES
 *********************/
#define RASTER_BUDGET   (768u * 1024u)
#define PAGE_BUDGET     (448u * 1024u)
#define BUILD_BUDGET    (256u * 1024u)
#define MAX_PARTS       24
#define MAX_BAND_GROUPS 64      /*Limits the split search on tall, irregular bitmaps*/

/*Relative costs used to choose the split, in units of one copied pixel:
 *every part is a draw task (~30 us), and a blended ARGB8888 pixel reads twice
 *the bytes of a copied one and does the math too.*/
#define COST_PART       1300
#define COST_ALPHA_PX   4

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    lv_area_t area;             /*Relative to the top left of the bitmap*/
    lv_draw_buf_t buf;          /*A view into the cache's pixels; drawn without a decoder*/
    lv_color_t color;           /*For an A8 part: its one color*/
} raster_part_t;

typedef struct raster_scale_entry_t raster_scale_entry_t;

typedef struct _lv_obj_raster_t {
    lv_obj_t * obj;
    struct _lv_obj_raster_t * next;
    uint8_t * mem;              /*The pixels of every part*/
    uint32_t mem_size;
    uint64_t used;
    uint32_t pinned, failed_epoch;
    int32_t failed_w, failed_h;
    bool page;
    raster_part_t * parts;
    uint32_t part_cnt;
    int32_t w;                  /*The size it was drawn at, with the ext. draw size*/
    int32_t h;
    lv_color_t matte;           /*What it's drawn over, if matte_on*/
    lv_opa_t opa;               /*What it's composited with*/
    uint16_t scale;             /*256: native size; shrink at the round rim without rebuilding*/
    bool matte_on;
    bool dirty;
    raster_scale_entry_t * scaled;
    lv_obj_rows_t * rows;
    bool rows_enabled;
    lv_obj_t * rows_focus;
    uint16_t rows_preview, rows_minimum;
    lv_opa_t requested_opa;
    lv_color32_t * sample_mip;
    uint32_t sample_bytes;
    uint16_t sample_w, sample_h;
} lv_obj_raster_t;

#if CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
struct raster_scale_entry_t {
    raster_scale_entry_t * next;
    lv_obj_raster_t * owner;
    uint8_t * pixels;
    uint32_t bytes;             /*Payload plus this metadata: both use the PSRAM raster allocator*/
    uint32_t frame;
    uint16_t scale;
    uint8_t part_cnt;
    raster_part_t parts[5];     /*Opaque SIMD-copy core and exact AA edge strips*/
};
#endif

/*Per row: the longest run of opaque pixels and the extent of visible ones, inclusive.
 *An empty range has x1 > x2.*/
typedef struct {
    int16_t o1, o2;
    int16_t v1, v2;
} row_span_t;

/*Consecutive rows with the same spans*/
typedef struct {
    int32_t y1, y2;
    row_span_t s;
} row_group_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static lv_obj_raster_t * get_raster(const lv_obj_t * obj);
static void free_pixels(lv_obj_raster_t * r);
static void get_area(lv_obj_t * obj, lv_area_t * area);
static bool build(lv_obj_raster_t * r);
static bool admit(lv_obj_raster_t * owner, uint32_t bytes);
static void pin_visible(lv_display_t * disp);
static void build_mip(lv_obj_raster_t * r, const lv_draw_buf_t * master);
static uint64_t cache_touch(void);
static void render(lv_obj_t * obj, lv_draw_buf_t * buf, const lv_area_t * area);
static uint32_t scan_rows(const lv_draw_buf_t * buf, row_group_t * groups);
static uint32_t split(const row_group_t * groups, uint32_t group_cnt, lv_area_t * areas, bool * opaque);
static void band_cost(const row_group_t * groups, uint32_t g1, uint32_t g2, lv_area_t * core, lv_area_t * vis);
static uint32_t band_parts(const lv_area_t * core, const lv_area_t * vis, lv_area_t * areas, bool * opaque);
static void copy_part(raster_part_t * part, const lv_draw_buf_t * src, uint8_t * dst, lv_color_format_t cf,
                      uint32_t stride);
static bool one_color(const lv_draw_buf_t * src, const lv_area_t * a, lv_color_t * color);
static lv_draw_buf_handlers_t * get_handlers(void);
static void * raster_malloc(size_t size);
static void raster_free(void * p);
static lv_obj_tree_walk_res_t prepare_cb(lv_obj_t * obj, void * user_data);
#if CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
static void scale_cache_forget(lv_obj_raster_t * owner);
static void scale_cache_prepare(lv_obj_raster_t * r);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/
static lv_obj_raster_t * raster_head;
static uint32_t raster_cnt;
static uint32_t raster_builds;
static uint32_t s_source_bytes, s_page_bytes, s_prepare_epoch;
static bool s_cache_paused;
#ifndef ESP_PLATFORM
static uint64_t s_cache_clock;
#endif
static uint32_t s_cache_evictions, s_cache_rejections, s_negative_hits, s_retry_epoch = 1;
static size_t s_failed_min = SIZE_MAX;
static lv_draw_buf_malloc_cb_t s_raw_malloc;
static lv_draw_buf_free_cb_t s_raw_free;
static uint32_t s_sample_bytes;
static uint32_t s_sample_builds;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
static uint64_t s_raster_build_us;
#endif
static lv_draw_buf_handlers_t raster_handlers;
static bool raster_handlers_init;
#if CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
static raster_scale_entry_t * s_scale_head;
static uint32_t s_scale_frame;
static bool s_scale_enabled = true;
static lv_obj_raster_scale_cache_stats_t s_scale_stats;
#endif

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_obj_set_raster(lv_obj_t * obj, bool en)
{
    LV_CHECK_OBJ(obj, &lv_obj_class, return);

    lv_obj_raster_t * r = get_raster(obj);
    if(en == (r != NULL)) return;

    if(en) {
        lv_obj_allocate_spec_attr(obj);
        r = raster_malloc(sizeof(lv_obj_raster_t));
        LV_ASSERT_MALLOC(r);
        if(r == NULL) return;
        lv_memzero(r, sizeof(*r));
        r->obj = obj;
        r->opa = r->requested_opa = LV_OPA_COVER;
        r->scale = LV_SCALE_NONE;
        r->dirty = true;
        r->next = raster_head;
        raster_head = r;
        raster_cnt++;
        obj->spec_attr->raster = r;
    }
    else {
        lv_obj_raster_delete(obj);
    }
    lv_obj_invalidate(obj);
}

void lv_obj_set_raster_matte(lv_obj_t * obj, lv_color_t color)
{
    LV_CHECK_OBJ(obj, &lv_obj_class, return);
    lv_obj_set_raster(obj, true);
    lv_obj_raster_t * r = get_raster(obj);
    if(r == NULL) return;
    r->matte = color;
    r->matte_on = true;
    r->dirty = true;
    lv_obj_invalidate(obj);
}

void lv_obj_set_raster_opa(lv_obj_t * obj, lv_opa_t opa)
{
    LV_CHECK_OBJ(obj, &lv_obj_class, return);
    lv_obj_raster_t * r = get_raster(obj);
    if(r == NULL) return;
    r->requested_opa = opa;
    if(r->rows_focus && (r->scale < r->rows_minimum ||
       (r->scale < r->rows_preview && !r->rows))) opa = LV_OPA_TRANSP;
    if(r->opa == opa) return;
    lv_obj_raster_invalidate_draw(obj);
    r->opa = opa;
    lv_obj_raster_invalidate_draw(obj);
}

void lv_obj_set_raster_scale(lv_obj_t * obj, uint16_t scale)
{
    LV_CHECK_OBJ(obj, &lv_obj_class, return);
    lv_obj_raster_t * r = get_raster(obj);
    scale = LV_MIN(scale, LV_SCALE_NONE);
    if(r == NULL || r->scale == scale) return;
    lv_obj_raster_invalidate_draw(obj);
    r->scale = scale;
    lv_obj_set_raster_opa(obj, r->requested_opa);
    lv_obj_raster_invalidate_draw(obj);
}

void lv_obj_raster_invalidate_draw(const lv_obj_t * obj)
{
    lv_obj_raster_t * r = get_raster(obj);
    if(!r) return;
    /* Moving or recompositing this bitmap changes its ancestors, not its
     * captured content. Note material revisions even for an invisible row. */
    lv_obj_raster_mark_dirty(obj, false);
    bool composite = lv_obj_get_layer_type(obj) == LV_LAYER_TYPE_NONE;
    if(composite && (r->opa <= LV_OPA_MIN || r->scale == 0)) return;
    lv_area_t area;
    get_area((lv_obj_t *)obj, &area);
    if(composite && !r->dirty && r->mem &&
       lv_area_get_width(&area) == r->w && lv_area_get_height(&area) == r->h) {
        int32_t w = r->w * r->scale / LV_SCALE_NONE;
        int32_t h = r->h * r->scale / LV_SCALE_NONE;
        area.x1 += (r->w - w) / 2;
        area.y1 += (r->h - h) / 2;
        /* Row spans/scale variants use floor(size*scale), but bitmap fallback
         * transforms the inclusive last pixel. Keep the same center and cover
         * its possible extra right/bottom pixel, even at subpixel row sizes. */
        int32_t image_w = (r->w - 1) * r->scale / LV_SCALE_NONE + 1;
        int32_t image_h = (r->h - 1) * r->scale / LV_SCALE_NONE + 1;
        area.x2 = area.x1 + LV_MAX(w, image_w) - 1;
        area.y2 = area.y1 + LV_MAX(h, image_h) - 1;
        /* Non-matte parts are independently resampled and can round an edge
         * out by one pixel. Their source bound remains the conservative path. */
        if(!r->matte_on) get_area((lv_obj_t *)obj, &area);
    }
    if(lv_obj_area_is_visible(obj, &area)) lv_inv_area(lv_obj_get_display(obj), &area);
}

void lv_obj_set_raster_rows(lv_obj_t * obj, bool enabled)
{
#if defined(CONFIG_ORB_RENDER_ROWS_AT_SIZE) && CONFIG_ORB_RENDER_ROWS_AT_SIZE
    lv_obj_raster_t * r = get_raster(obj);
    if(!r || r->rows_enabled == enabled) return;
    r->rows_enabled = enabled;
    r->dirty = true;
    lv_obj_invalidate(obj);
#else
    LV_UNUSED(obj);
    LV_UNUSED(enabled);
#endif
}

void lv_obj_set_raster_rows_focus(lv_obj_t * obj, lv_obj_t * title, uint16_t preview_scale, uint16_t minimum_scale)
{
#if defined(CONFIG_ORB_RENDER_ROWS_AT_SIZE) && CONFIG_ORB_RENDER_ROWS_AT_SIZE
    lv_obj_set_raster_matte(obj, lv_color_black());
    lv_obj_set_raster_rows(obj, true);
    lv_obj_raster_t * r = get_raster(obj);
    if(r) { r->rows_focus = title; r->rows_preview = preview_scale; r->rows_minimum = minimum_scale; }
#else
    LV_UNUSED(obj); LV_UNUSED(title); LV_UNUSED(preview_scale); LV_UNUSED(minimum_scale);
#endif
}

bool lv_obj_raster_rows_ready(const lv_obj_t * obj)
{
    lv_obj_raster_t * r = get_raster(obj);
    return r && r->rows;
}

bool lv_obj_raster_presentation_visible(const lv_obj_t * obj)
{
    for(const lv_obj_t * ancestor = obj; ancestor; ancestor = ancestor->parent) {
        lv_obj_raster_t * r = get_raster(ancestor);
        if(!r) continue;
        if(r->opa <= LV_OPA_MIN || !r->scale) return false;
        if(obj != ancestor && r->rows && r->rows_focus && r->scale < r->rows_preview) {
            const lv_obj_t * content = obj;
            while(content && content != ancestor && content != r->rows_focus) content = content->parent;
            if(content != r->rows_focus) return false;
        }
    }
    return true;
}

bool lv_obj_raster_sample_pixel(lv_obj_t * obj, int32_t x, int32_t y, lv_color32_t * color)
{
    lv_obj_raster_t * r = get_raster(obj);
    if(!r || r->dirty || !r->mem || !r->scale || !r->opa) return false;
    lv_area_t area;
    get_area(obj, &area);
    if(r->rows && r->scale != LV_SCALE_NONE && lv_obj_rows_sample(r->rows, &area, r->scale, x, y, color)) {
        color->alpha = r->opa;
        return true;
    }
    int32_t w = r->w * r->scale / 256, h = r->h * r->scale / 256;
    int32_t dx = area.x1 + (r->w - w) / 2, dy = area.y1 + (r->h - h) / 2;
    if(x < dx || y < dy || x >= dx + w || y >= dy + h) return false;
    int32_t sx = (x - dx) * 256 / r->scale, sy = (y - dy) * 256 / r->scale;
    for(uint32_t i = 0; i < r->part_cnt; i++) {
        raster_part_t * part = &r->parts[i];
        if(sx < part->area.x1 || sx > part->area.x2 || sy < part->area.y1 || sy > part->area.y2) continue;
        const uint8_t * p = part->buf.data + (sy - part->area.y1) * part->buf.header.stride;
        unsigned px = sx - part->area.x1;
        if(part->buf.header.cf == LV_COLOR_FORMAT_RGB565 || part->buf.header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED) {
            uint16_t c;
            lv_memcpy(&c, p + 2 * px, 2);
            if(part->buf.header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED) c = (uint16_t)((c << 8) | (c >> 8));
            color->red = ((c >> 11) * 255 + 15) / 31;
            color->green = (((c >> 5) & 63) * 255 + 31) / 63;
            color->blue = ((c & 31) * 255 + 15) / 31;
            color->alpha = r->opa;
        }
        else if(part->buf.header.cf == LV_COLOR_FORMAT_A8) {
            *color = lv_color_to_32(part->color, (p[px] * r->opa + 127) / 255);
        }
        else if(part->buf.header.cf == LV_COLOR_FORMAT_ARGB8888) {
            lv_memcpy(color, p + px * 4, 4);
            color->alpha = (color->alpha * r->opa + 127) / 255;
        }
        else return false;
        return true;
    }
    return false;
}

bool lv_obj_raster_sample_area(lv_obj_t * obj, const lv_area_t * sample, lv_color32_t * color)
{
    lv_obj_raster_t * r = get_raster(obj);
    if(!r || r->dirty || !r->mem || !r->scale || !r->opa) return false;
    lv_area_t area;
    get_area(obj, &area);
    if(r->rows && r->scale != LV_SCALE_NONE && lv_obj_rows_sample_area(r->rows, &area, r->scale, sample, color)) {
        color->alpha = (color->alpha * r->opa + 127) / 255;
        return true;
    }
    int32_t w = r->w * r->scale / 256, h = r->h * r->scale / 256;
    int32_t dx = area.x1 + (r->w - w) / 2, dy = area.y1 + (r->h - h) / 2;
    lv_area_t box = {dx, dy, dx + w - 1, dy + h - 1}, clip;
    if(!lv_area_intersect(&clip, &box, sample)) return false;
    if(r->sample_mip) {
        /* Cold 4x area-averaged content mip: reduced material sampling never
         * scans the full-resolution PSRAM raster on a moving frame. */
        uint32_t x0 = (clip.x1 - dx) * 65536u / r->scale;
        uint32_t y0 = (clip.y1 - dy) * 65536u / r->scale;
        uint32_t x1 = LV_MIN((uint32_t)r->w * 256, (clip.x2 + 1 - dx) * 65536u / r->scale);
        uint32_t y1 = LV_MIN((uint32_t)r->h * 256, (clip.y2 + 1 - dy) * 65536u / r->scale);
        uint64_t red = 0, green = 0, blue = 0, alpha = 0, weights = 0;
        for(uint32_t y = y0 / 1024; y < (y1 + 1023) / 1024; y++) {
            uint32_t wy = LV_MIN(y1, (y + 1) * 1024) - LV_MAX(y0, y * 1024);
            for(uint32_t x = x0 / 1024; x < (x1 + 1023) / 1024; x++) {
                uint32_t wx = LV_MIN(x1, (x + 1) * 1024) - LV_MAX(x0, x * 1024);
                uint64_t weight = wx * wy;
                lv_color32_t c = r->sample_mip[y * r->sample_w + x];
                alpha += c.alpha * weight;
                red += c.red * c.alpha * weight;
                green += c.green * c.alpha * weight;
                blue += c.blue * c.alpha * weight;
                weights += weight;
            }
        }
        if(!alpha || !weights) return false;
        color->red = (red + alpha / 2) / alpha;
        color->green = (green + alpha / 2) / alpha;
        color->blue = (blue + alpha / 2) / alpha;
        uint32_t a = (alpha + weights / 2) / weights;
        a = (a * lv_area_get_size(&clip) + lv_area_get_size(sample) / 2) / lv_area_get_size(sample);
        color->alpha = (a * r->opa + 127) / 255;
        return true;
    }
#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
    return false;
#endif
    /* Off-build/test seam: bounded sampling of existing parts. The fused build
     * admits its mip at source creation, so it never uses this warm fallback. */
    unsigned red = 0, green = 0, blue = 0, alpha = 0;
    for(unsigned y = 0; y < 4; y++) for(unsigned x = 0; x < 4; x++) {
        lv_color32_t c;
        int px = sample->x1 + (2 * x + 1) * lv_area_get_width(sample) / 8;
        int py = sample->y1 + (2 * y + 1) * lv_area_get_height(sample) / 8;
        if(!lv_obj_raster_sample_pixel(obj, px, py, &c)) continue;
        alpha += c.alpha; red += c.red * c.alpha; green += c.green * c.alpha; blue += c.blue * c.alpha;
    }
    if(!alpha) return false;
    color->red = (red + alpha / 2) / alpha;
    color->green = (green + alpha / 2) / alpha;
    color->blue = (blue + alpha / 2) / alpha;
    color->alpha = (alpha + 8) / 16;
    return true;
}

bool lv_obj_raster_sample_ready(lv_obj_t * obj)
{
    lv_obj_raster_t * r = get_raster(obj);
    return r && !r->dirty && r->mem && ((r->rows && r->scale != LV_SCALE_NONE) || r->sample_mip);
}

bool lv_obj_get_raster(const lv_obj_t * obj)
{
    LV_CHECK_OBJ(obj, &lv_obj_class, return false);
    return get_raster(obj) != NULL;
}

bool lv_obj_raster_is_opaque(const lv_obj_t * obj)
{
    lv_obj_raster_t * r = get_raster(obj);
    /*A source's opaque children do not cover the space left by its compositor.*/
    return r != NULL && r->opa == LV_OPA_COVER && r->scale == LV_SCALE_NONE;
}

void lv_obj_invalidate_raster(lv_obj_t * obj)
{
    LV_CHECK_OBJ(obj, &lv_obj_class, return);
    lv_obj_raster_mark_dirty(obj, true);
    lv_obj_invalidate(obj);
}

void lv_obj_raster_prepare(lv_obj_t * obj)
{
    LV_CHECK_OBJ(obj, &lv_obj_class, return);
    if(s_cache_paused || raster_cnt == 0) return;

    lv_display_t * disp = lv_obj_get_display(obj);
    LV_ASSERT_MSG(!disp->rendering_in_progress, "Raster caches can't be prepared while rendering");
    if(disp->rendering_in_progress) return;

    lv_obj_update_layout(obj);
    pin_visible(disp);
    lv_obj_tree_walk(obj, prepare_cb, NULL);
}

bool lv_obj_raster_ready(const lv_obj_t * obj)
{
    if(!obj) return false;
    lv_obj_raster_t * r = get_raster(obj);
    if(!r || r->dirty || !r->mem) return false;
    lv_area_t area;
    get_area((lv_obj_t *)obj, &area);
    return lv_area_get_width(&area) == r->w && lv_area_get_height(&area) == r->h;
}

void lv_obj_raster_set_allocator(lv_draw_buf_malloc_cb_t malloc_cb, lv_draw_buf_free_cb_t free_cb)
{
    LV_CHECK_ARG(malloc_cb != NULL && free_cb != NULL, return);
    lv_draw_buf_handlers_t * h = get_handlers();
    LV_UNUSED(h);
    s_raw_malloc = malloc_cb;
    s_raw_free = free_cb;
}

void lv_obj_raster_get_stats(lv_obj_raster_stats_t * stats)
{
    LV_CHECK_ARG(stats != NULL, return);
    lv_memzero(stats, sizeof(*stats));
    stats->source_bytes = s_source_bytes;
    stats->source_budget = RASTER_BUDGET;
    stats->page_bytes = s_page_bytes;
    stats->page_budget = PAGE_BUDGET;
    stats->evictions = s_cache_evictions;
    stats->rejections = s_cache_rejections;
    stats->negative_hits = s_negative_hits;
    stats->count = raster_cnt;
    stats->builds = raster_builds;
    stats->build_us = 0;
    stats->mip_builds = s_sample_builds;
    stats->mip_bytes = s_sample_bytes;
    stats->mip_budget = 192u * 1024u;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    stats->build_us = s_raster_build_us;
#endif
    stats->bytes = 0;
    lv_obj_raster_t * r;
    for(r = raster_head; r; r = r->next) stats->bytes += r->mem_size + r->sample_bytes;
#if CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
    stats->bytes += s_scale_stats.bytes;
#endif
}

void lv_obj_raster_scale_cache_get_stats(lv_obj_raster_scale_cache_stats_t * stats)
{
    LV_CHECK_ARG(stats != NULL, return);
    lv_memzero(stats, sizeof(*stats));
#if CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
    *stats = s_scale_stats;
    stats->budget = CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE_KIB * 1024u;
#endif
}

void lv_obj_raster_scale_cache_set_enabled(bool enabled)
{
#if CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
    /*Only changes selection at the next refresh; queued image descriptors keep
     *their sources alive until that refresh has drained the previous frame.*/
    s_scale_enabled = enabled;
#else
    LV_UNUSED(enabled);
#endif
}

void lv_obj_raster_mark_dirty(const lv_obj_t * obj, bool self)
{
    lv_obj_material_note_invalidation(obj);
    if(raster_cnt == 0) return;

    const lv_obj_t * o = self ? obj : obj->parent;
    for(; o; o = o->parent) {
        lv_obj_raster_t * r = get_raster(o);
        if(r) r->dirty = true;
    }
}

bool lv_obj_raster_draw(lv_layer_t * layer, lv_obj_t * obj)
{
    lv_obj_raster_t * r = get_raster(obj);
    if(r == NULL) return false;
    /* The compositor's hidden state is authoritative even before its first
     * bitmap exists. Refresh can then defer cold work until a row is shown. */
    if(r->opa <= LV_OPA_MIN || r->scale == 0) return true;
    if(r->dirty || r->mem == NULL) return false;
    r->used = cache_touch();

    lv_area_t area;
    get_area(obj, &area);
    if(lv_area_get_width(&area) != r->w || lv_area_get_height(&area) != r->h) return false;

    /*As lv_obj_redraw() would*/
    obj->rendered = 1;

    if(lv_obj_rows_draw(r->rows, layer, &area, r->scale, r->opa, r->matte)) return true;

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.base.obj = obj;
    dsc.base.part = LV_PART_MAIN;
    dsc.base.layer = layer;
    dsc.opa = r->opa;
    dsc.scale_x = dsc.scale_y = r->scale;

#if CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
    if(s_scale_enabled && r->scaled && r->scaled->scale == r->scale) {
        raster_scale_entry_t * e = r->scaled;
        int32_t x = area.x1 + (r->w - r->w * r->scale / LV_SCALE_NONE) / 2;
        int32_t y = area.y1 + (r->h - r->h * r->scale / LV_SCALE_NONE) / 2;
        dsc.scale_x = dsc.scale_y = LV_SCALE_NONE;
        bool drew = false;
        for(uint32_t i = 0; i < e->part_cnt; i++) {
            lv_area_t coords = e->parts[i].area;
            lv_area_move(&coords, x, y);
            if(!lv_area_is_on(&coords, &layer->_clip_area)) continue;
            dsc.src = &e->parts[i].buf;
            lv_draw_image(layer, &dsc, &coords);
            drew = true;
        }
        if(drew) s_scale_stats.blits++;
        return true;
    }
#endif

    uint32_t i;
    for(i = 0; i < r->part_cnt; i++) {
        raster_part_t * part = &r->parts[i];
        lv_area_t coords = part->area;
        if(r->scale == LV_SCALE_NONE) {
            lv_area_move(&coords, area.x1, area.y1);
        }
        else {
            /*Parts share the same center/scale. Matte list rows use one part,
             * keeping their rounded corners and text in one resampling pass.*/
            int32_t x = area.x1 + (r->w - r->w * r->scale / LV_SCALE_NONE) / 2;
            int32_t y = area.y1 + (r->h - r->h * r->scale / LV_SCALE_NONE) / 2;
            lv_area_move(&coords, x + coords.x1 * r->scale / LV_SCALE_NONE - coords.x1,
                         y + coords.y1 * r->scale / LV_SCALE_NONE - coords.y1);
        }
        if(!lv_area_is_on(&coords, &layer->_clip_area)) continue;
        dsc.src = &part->buf;
        dsc.recolor = part->color;
        lv_draw_image(layer, &dsc, &coords);
    }
    return true;
}

void lv_obj_raster_refresh(lv_display_t * disp)
{
    if(raster_cnt == 0 || disp->inv_p == 0) return;

    pin_visible(disp);
    lv_obj_raster_t * r;
    for(r = raster_head; r; r = r->next) {
        lv_obj_t * obj = r->obj;
        if(r->opa <= LV_OPA_MIN || r->scale == 0) continue;
        lv_area_t area;
        get_area(obj, &area);
        bool stale = r->dirty || r->mem == NULL || lv_area_get_width(&area) != r->w ||
                     lv_area_get_height(&area) != r->h;
        if(!stale || lv_obj_get_display(obj) != disp) continue;

        /*Only what this frame shows: the rest waits until it's on screen*/
        if(!lv_obj_area_is_visible(obj, &area)) continue;
        uint32_t i;
        for(i = 0; i < disp->inv_p; i++) {
            if(!disp->inv_area_joined[i] && lv_area_is_on(&area, &disp->inv_areas[i])) break;
        }
        if(i == disp->inv_p) continue;

        build(r);
    }
#if CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
    /*All mutation/eviction happens before task creation. Draw workers only read
     *these buffers, even when several bands refer to the same scaled variant.*/
    s_scale_frame++;
    if(s_scale_frame == 0) s_scale_frame++;
    for(r = raster_head; r; r = r->next) {
        r->scaled = NULL;
        if(!s_scale_enabled || lv_obj_get_display(r->obj) != disp) continue;
        lv_area_t area;
        get_area(r->obj, &area);
        if(r->opa <= LV_OPA_MIN || r->scale == 0 || !lv_obj_area_is_visible(r->obj, &area)) continue;
        /*Reserve all hits before any miss can evict: raster traversal order is
         *not draw order, so a late hit may already be needed by this frame.*/
        for(raster_scale_entry_t * e = s_scale_head; e; e = e->next) {
            if(e->owner == r && e->scale == r->scale) {
                e->frame = s_scale_frame;
                break;
            }
        }
    }
    for(r = raster_head; r; r = r->next) {
        if(!s_scale_enabled || r->opa <= LV_OPA_MIN || r->scale == 0 || lv_obj_get_display(r->obj) != disp) continue;
        lv_area_t area;
        get_area(r->obj, &area);
        if(lv_obj_area_is_visible(r->obj, &area)) scale_cache_prepare(r);
    }
#endif
}

void lv_obj_raster_delete(lv_obj_t * obj)
{
    lv_obj_raster_t * r = get_raster(obj);
    if(r == NULL) return;

    lv_obj_raster_t ** p = &raster_head;
    while(*p != r) p = &(*p)->next;
    *p = r->next;
    raster_cnt--;

    free_pixels(r);
    raster_free(r);
    obj->spec_attr->raster = NULL;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static lv_obj_raster_t * get_raster(const lv_obj_t * obj)
{
    return obj->spec_attr ? obj->spec_attr->raster : NULL;
}

static void free_pixels(lv_obj_raster_t * r)
{
    lv_obj_rows_destroy(r->rows);
    r->rows = NULL;
#ifdef ESP_PLATFORM
    orb_memory_release(ORB_MEMORY_MIPS, r->sample_bytes);
#endif
    raster_free(r->sample_mip);
    r->sample_mip = NULL;
    s_sample_bytes -= r->sample_bytes;
    r->sample_bytes = 0;
#if CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
    scale_cache_forget(r);
#endif
    uint32_t charge = r->mem_size + r->part_cnt * sizeof(raster_part_t);
    if(r->part_cnt) {
#ifdef ESP_PLATFORM
        orb_memory_release(r->page ? ORB_MEMORY_PAGE : ORB_MEMORY_RASTER, charge);
#endif
        if(r->page) s_page_bytes -= charge;
        else s_source_bytes -= charge;
    }
    raster_free(r->mem);
    raster_free(r->parts);
    r->mem = NULL;
    r->parts = NULL;
    r->mem_size = 0;
    r->part_cnt = 0;
}

static void get_area(lv_obj_t * obj, lv_area_t * area)
{
    int32_t ext = lv_obj_get_ext_draw_size(obj);
    *area = obj->coords;
    lv_area_increase(area, ext, ext);
}

/* Only called before draw tasks exist. Pin the visible working set first,
 * so traversal order can never evict pixels needed by a later band. */
static void pin_visible(lv_display_t * disp)
{
    if(++s_prepare_epoch == 0) ++s_prepare_epoch;
    for(lv_obj_raster_t * r = raster_head; r; r = r->next) {
        if(lv_obj_get_display(r->obj) != disp || r->opa <= LV_OPA_MIN || !r->scale) continue;
        lv_area_t area;
        get_area(r->obj, &area);
        if(lv_obj_area_is_visible(r->obj, &area)) r->pinned = s_prepare_epoch;
    }
}

static bool admit(lv_obj_raster_t * owner, uint32_t bytes)
{
    uint32_t budget = owner->page ? PAGE_BUDGET : RASTER_BUDGET;
    uint32_t * used = owner->page ? &s_page_bytes : &s_source_bytes;
    if(bytes > budget) { s_cache_rejections++; return false; }
    while(bytes > budget - *used) {
        lv_obj_raster_t * oldest = NULL;
        for(lv_obj_raster_t * r = raster_head; r; r = r->next) {
            if(r == owner || !r->mem || r->page != owner->page || r->pinned == s_prepare_epoch) continue;
            if(!oldest || r->used < oldest->used) oldest = r;
        }
        if(!oldest) { s_cache_rejections++; return false; }
        free_pixels(oldest);
        oldest->dirty = true;
        s_cache_evictions++;
    }
#ifdef ESP_PLATFORM
    return orb_memory_cache_admit(owner->page ? ORB_MEMORY_PAGE : ORB_MEMORY_RASTER, bytes);
#else
    return true;
#endif
}

void lv_obj_raster_cache_retry(void)
{
    s_failed_min = SIZE_MAX;
    if(++s_retry_epoch == 0) ++s_retry_epoch;
}

void lv_obj_raster_cache_release_frame(void)
{
    if(++s_prepare_epoch == 0) ++s_prepare_epoch;
}

void lv_obj_raster_cache_set_paused(bool paused)
{
    s_cache_paused = paused;
}

static bool build(lv_obj_raster_t * r)
{
    if(s_cache_paused) return false;
    LV_PROFILER_REFR_BEGIN;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    uint64_t started = lv_draw_sw_profile_time_us();
#endif
    lv_obj_t * obj = r->obj;
    lv_area_t area;
    get_area(obj, &area);
    int32_t w = lv_area_get_width(&area);
    int32_t h = lv_area_get_height(&area);
    if(r->failed_epoch == s_retry_epoch && r->failed_w == w && r->failed_h == h) {
        s_negative_hits++;
        LV_PROFILER_REFR_END;
        return false;
    }
    r->pinned = s_prepare_epoch;
    free_pixels(r);
    r->page = w >= lv_display_get_horizontal_resolution(lv_obj_get_display(obj)) &&
              h >= lv_display_get_vertical_resolution(lv_obj_get_display(obj));
    r->w = w;
    r->h = h;
    /*Cleared first, so a change made while drawing (it shouldn't happen) draws again next frame*/
    r->dirty = false;
    if(w <= 0 || h <= 0) {
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
        s_raster_build_us += lv_draw_sw_profile_time_us() - started;
#endif
        LV_PROFILER_REFR_END;
        return true;
    }

    /* A known opaque matte never needs the 868,624-byte full-screen ARGB
     * master plus a second RGB565 copy. Render its final source in place. */
    lv_color_format_t cf = lv_display_get_color_format(lv_obj_get_display(obj));
    if(r->matte_on && (cf == LV_COLOR_FORMAT_RGB565 || cf == LV_COLOR_FORMAT_RGB565_SWAPPED)) {
        uint32_t stride = (uint32_t)LV_ALIGN_UP((uint64_t)w * 2, 4);
        uint64_t bytes = (uint64_t)stride * h;
        bool admitted = bytes <= UINT32_MAX && admit(r, (uint32_t)bytes + sizeof(raster_part_t));
        bool ok = admitted;
        uint8_t * pixels = ok ? raster_malloc((size_t)bytes) : NULL;
        raster_part_t * part = pixels ? raster_malloc(sizeof(*part)) : NULL;
        ok = pixels && part;
        if(ok) {
            uint16_t matte = lv_color_to_u16(r->matte);
            if(cf == LV_COLOR_FORMAT_RGB565_SWAPPED) matte = lv_color_swap_16(matte);
            for(int32_t y = 0; y < h; y++) {
                uint16_t * row = (uint16_t *)(pixels + (size_t)y * stride);
                for(int32_t x = 0; x < w; x++) row[x] = matte;
            }
            lv_draw_buf_t draw;
            lv_draw_buf_init(&draw, w, h, cf, stride, pixels, (uint32_t)bytes);
            render(obj, &draw, &area);
            lv_memzero(part, sizeof(*part));
            part->area = (lv_area_t){0, 0, w - 1, h - 1};
            part->buf = draw;
            part->buf.header.flags |= LV_IMAGE_FLAGS_ALLOCATED;
            part->buf.handlers = get_handlers();
            r->mem = pixels; r->parts = part; r->part_cnt = 1;
            r->mem_size = (uint32_t)bytes;
            uint32_t charge = r->mem_size + sizeof(*part);
            if(r->page) s_page_bytes += charge; else s_source_bytes += charge;
            r->used = cache_touch();
            raster_builds++;
            build_mip(r, &draw);
            if(r->rows_enabled) r->rows = lv_obj_rows_create(obj, r->matte,
                get_handlers()->buf_malloc_cb, get_handlers()->buf_free_cb, r->rows_focus, r->rows_preview);
            if(r->rows && r->scale >= r->rows_minimum) r->opa = r->requested_opa;
        }
        else {
#ifdef ESP_PLATFORM
            if(admitted) orb_memory_release(r->page ? ORB_MEMORY_PAGE : ORB_MEMORY_RASTER,
                                           (size_t)bytes + sizeof(raster_part_t));
#endif
            raster_free(pixels); raster_free(part);
            r->dirty = true;
            r->failed_epoch = s_retry_epoch; r->failed_w = w; r->failed_h = h;
        }
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
        s_raster_build_us += lv_draw_sw_profile_time_us() - started;
#endif
        LV_PROFILER_REFR_END;
        return ok;
    }
    /* Transparent cold preparation is bounded even when a caller marks an
     * entire page as a raster. Oversize content uses the ordinary renderer. */
    if((uint64_t)w * h * 4 + (uint64_t)h * (sizeof(row_group_t) + 8) +
       MAX_PARTS * (sizeof(lv_area_t) + sizeof(bool)) > BUILD_BUDGET) {
        r->dirty = true;
        r->failed_epoch = s_retry_epoch; r->failed_w = w; r->failed_h = h;
        s_cache_rejections++;
        LV_PROFILER_REFR_END;
        return false;
    }
    size_t build_bytes = (size_t)w * h * 4 + (size_t)h * (sizeof(row_group_t) + 8) +
                       MAX_PARTS * (sizeof(lv_area_t) + sizeof(bool));
#ifdef ESP_PLATFORM
    if(!orb_memory_reserve(ORB_MEMORY_RASTER_BUILD, build_bytes)) {
        r->dirty = true;
        r->failed_epoch = s_retry_epoch; r->failed_w = w; r->failed_h = h;
        LV_PROFILER_REFR_END;
        return false;
    }
#else
    /* Host telemetry follows the device charge without imposing board admission
     * limits on desktop fixtures. This payload is not a retained cache entry. */
    orb_memory_track(ORB_MEMORY_RASTER_BUILD, build_bytes);
#endif
    lv_draw_buf_t * master = lv_draw_buf_create_ex(get_handlers(), w, h,
                                                   LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    /* A matte is initialized opaque and source-over painting preserves that
     * fact. Scanning/splitting every ARGB pixel only rediscovers one rectangle. */
    row_group_t * groups = r->matte_on ? NULL : raster_malloc(sizeof(row_group_t) * h);
    lv_area_t * areas = raster_malloc(sizeof(lv_area_t) * MAX_PARTS);
    bool * opaque = raster_malloc(sizeof(bool) * MAX_PARTS);
    bool ok = master && (r->matte_on || groups) && areas && opaque;
    if(ok) {
        if(r->matte_on) {
            /*Opaque from the start: everything drawn over it stays opaque*/
            lv_color32_t m = lv_color_to_32(r->matte, LV_OPA_COVER);
            int32_t y, x;
            for(y = 0; y < h; y++) {
                lv_color32_t * px = lv_draw_buf_goto_xy(master, 0, y);
                for(x = 0; x < w; x++) px[x] = m;
            }
        }
        else {
            lv_draw_buf_clear(master, NULL);
        }
        render(obj, master, &area);

        uint32_t part_cnt;
        if(r->matte_on) {
            areas[0] = (lv_area_t){0, 0, w - 1, h - 1};
            opaque[0] = true;
            part_cnt = 1;
        }
        else {
            uint32_t group_cnt = scan_rows(master, groups);
            part_cnt = split(groups, group_cnt, areas, opaque);
        }

        /*Opaque parts share one plane in the display's format; it's a copy only if the formats match*/
        bool plane_ok = cf == LV_COLOR_FORMAT_RGB565 || cf == LV_COLOR_FORMAT_RGB565_SWAPPED;
        uint32_t plane_stride = (uint32_t)LV_ALIGN_UP(w * 2, 4);
        uint32_t size = 0;
        bool need_plane = false;
        uint32_t i;
        lv_color_t colors[MAX_PARTS];
        bool mono[MAX_PARTS];
        for(i = 0; i < part_cnt; i++) {
            if(!plane_ok) opaque[i] = false;
            colors[i] = lv_color_black();
            mono[i] = !opaque[i] && one_color(master, &areas[i], &colors[i]);
            if(opaque[i]) need_plane = true;
            else size += lv_area_get_size(&areas[i]) * (mono[i] ? 1 : 4);
        }
        if(need_plane) size += plane_stride * h;

        bool admitted = admit(r, size + sizeof(raster_part_t) * part_cnt);
        ok = admitted;
        r->parts = ok && part_cnt ? raster_malloc(sizeof(raster_part_t) * part_cnt) : NULL;
        r->mem = ok && size ? raster_malloc(size) : NULL;
        ok = ok && (part_cnt == 0 || r->parts) && (size == 0 || r->mem);
        if(ok) {
            uint8_t * plane = r->mem;
            uint8_t * next = need_plane ? r->mem + plane_stride * h : r->mem;
            for(i = 0; i < part_cnt; i++) {
                raster_part_t * part = &r->parts[i];
                part->area = areas[i];
                part->color = colors[i];
                if(opaque[i]) {
                    copy_part(part, master, plane + areas[i].y1 * plane_stride + areas[i].x1 * 2, cf, plane_stride);
                }
                else if(mono[i]) {
                    copy_part(part, master, next, LV_COLOR_FORMAT_A8, lv_area_get_width(&areas[i]));
                    next += lv_area_get_size(&areas[i]);
                }
                else {
                    copy_part(part, master, next, LV_COLOR_FORMAT_ARGB8888, lv_area_get_width(&areas[i]) * 4);
                    next += lv_area_get_size(&areas[i]) * 4;
                }
            }
            r->part_cnt = part_cnt;
            r->mem_size = size;
            uint32_t charge = size + part_cnt * sizeof(raster_part_t);
            if(r->page) s_page_bytes += charge; else s_source_bytes += charge;
            r->used = cache_touch();
            raster_builds++;
        }
#ifdef ESP_PLATFORM
        else if(admitted) orb_memory_release(r->page ? ORB_MEMORY_PAGE : ORB_MEMORY_RASTER,
                                             size + sizeof(raster_part_t) * part_cnt);
#endif
    }

    if(ok && master) build_mip(r, master);
    if(master) lv_draw_buf_destroy(master);
    raster_free(groups);
    raster_free(areas);
    raster_free(opaque);
#ifdef ESP_PLATFORM
    orb_memory_release(ORB_MEMORY_RASTER_BUILD, build_bytes);
#else
    orb_memory_untrack(ORB_MEMORY_RASTER_BUILD, build_bytes);
#endif
    if(!ok) {
        LV_LOG_WARN("Out of memory for a raster cache of %" LV_PRId32 "x%" LV_PRId32, w, h);
        free_pixels(r);
        r->dirty = true;
        r->failed_epoch = s_retry_epoch; r->failed_w = w; r->failed_h = h;
    }
    else if(r->rows_enabled && r->matte_on) {
        r->rows = lv_obj_rows_create(obj, r->matte, get_handlers()->buf_malloc_cb, get_handlers()->buf_free_cb, r->rows_focus, r->rows_preview);
    }
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    s_raster_build_us += lv_draw_sw_profile_time_us() - started;
#endif
    LV_PROFILER_REFR_END;
    return ok;
}

static void build_mip(lv_obj_raster_t * r, const lv_draw_buf_t * master)
{
#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
    int32_t w = master->header.w, h = master->header.h;
    if(w <= 1024 && h <= 1024) {
        r->sample_w = (w + 3) / 4;
        r->sample_h = (h + 3) / 4;
        r->sample_bytes = (uint32_t)r->sample_w * r->sample_h * sizeof(lv_color32_t);
        bool admitted = r->sample_bytes <= 192u * 1024u - s_sample_bytes;
#ifdef ESP_PLATFORM
        admitted = admitted && orb_memory_cache_admit(ORB_MEMORY_MIPS, r->sample_bytes);
#endif
        r->sample_mip = admitted ? raster_malloc(r->sample_bytes) : NULL;
        if(r->sample_mip) {
            s_sample_bytes += r->sample_bytes;
            s_sample_builds++;
            for(unsigned y = 0; y < r->sample_h; y++) for(unsigned x = 0; x < r->sample_w; x++) {
                uint32_t red = 0, green = 0, blue = 0, alpha = 0, count = 0;
                for(unsigned sy = y * 4; sy < LV_MIN((int)(y + 1) * 4, h); sy++) {
                    for(unsigned sx = x * 4; sx < LV_MIN((int)(x + 1) * 4, w); sx++) {
                        lv_color32_t c;
                        if(master->header.cf == LV_COLOR_FORMAT_ARGB8888) {
                            const lv_color32_t * src = lv_draw_buf_goto_xy(master, 0, sy);
                            c = src[sx];
                        }
                        else {
                            const uint16_t * src = lv_draw_buf_goto_xy(master, 0, sy);
                            uint16_t color = src[sx];
                            if(master->header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED) color = lv_color_swap_16(color);
                            c = (lv_color32_t){.red = ((color >> 11) * 255 + 15) / 31,
                                .green = (((color >> 5) & 63) * 255 + 31) / 63,
                                .blue = ((color & 31) * 255 + 15) / 31, .alpha = 255};
                        }
                        red += c.red * c.alpha; green += c.green * c.alpha; blue += c.blue * c.alpha;
                        alpha += c.alpha; count++;
                    }
                }
                lv_color32_t * dst = &r->sample_mip[y * r->sample_w + x];
                dst->red = alpha ? (red + alpha / 2) / alpha : 0;
                dst->green = alpha ? (green + alpha / 2) / alpha : 0;
                dst->blue = alpha ? (blue + alpha / 2) / alpha : 0;
                dst->alpha = (alpha + count / 2) / count;
            }
        }
        else {
#ifdef ESP_PLATFORM
            if(admitted) orb_memory_release(ORB_MEMORY_MIPS, r->sample_bytes);
#endif
            r->sample_bytes = 0;
        }
    }
#else
    LV_UNUSED(r); LV_UNUSED(master);
#endif
}

/*As lv_snapshot does: the widget alone, into its own layer, waiting until it's done*/
static void render(lv_obj_t * obj, lv_draw_buf_t * buf, const lv_area_t * area)
{
    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = buf;
    layer.display = lv_obj_get_display(obj);
    layer.buf_area = *area;
    layer.color_format = buf->header.cf;
    layer._clip_area = *area;
    layer.phy_clip_area = *area;

    lv_display_t * disp = layer.display;
    lv_display_t * disp_old = lv_refr_get_disp_refreshing();
    lv_layer_t * layer_old = disp->layer_head;
    disp->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp);

    lv_obj_redraw(&layer, obj);
    layer.all_tasks_added = true;
    while(layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }

    disp->layer_head = layer_old;
    lv_refr_set_disp_refreshing(disp_old);
}

static uint32_t scan_rows(const lv_draw_buf_t * buf, row_group_t * groups)
{
    int32_t w = buf->header.w;
    int32_t h = buf->header.h;
    uint32_t cnt = 0;
    int32_t y;
    for(y = 0; y < h; y++) {
        const lv_color32_t * px = lv_draw_buf_goto_xy(buf, 0, y);
        row_span_t s = {.o1 = 1, .o2 = 0, .v1 = 1, .v2 = 0};
        int32_t run = -1;
        int32_t x;
        for(x = 0; x <= w; x++) {
            uint8_t a = x < w ? px[x].alpha : 0;
            if(a == 0xFF) {
                if(run < 0) run = x;
                continue;
            }
            if(run >= 0) {
                if(x - run > s.o2 - s.o1 + 1) {
                    s.o1 = (int16_t)run;
                    s.o2 = (int16_t)(x - 1);
                }
                run = -1;
            }
        }
        for(x = 0; x < w && px[x].alpha == 0; x++);
        if(x < w) {
            s.v1 = (int16_t)x;
            for(x = w - 1; px[x].alpha == 0; x--);
            s.v2 = (int16_t)x;
        }

        if(cnt && lv_memcmp(&groups[cnt - 1].s, &s, sizeof(s)) == 0) {
            groups[cnt - 1].y2 = y;
        }
        else {
            groups[cnt].y1 = y;
            groups[cnt].y2 = y;
            groups[cnt].s = s;
            cnt++;
        }
    }
    return cnt;
}

/*The opaque core and the visible extent of the rows in groups [g1, g2]*/
static void band_cost(const row_group_t * groups, uint32_t g1, uint32_t g2, lv_area_t * core, lv_area_t * vis)
{
    lv_area_set(core, INT16_MIN, groups[g1].y1, INT16_MAX, groups[g2].y2);
    lv_area_set(vis, INT16_MAX, groups[g1].y1, INT16_MIN, groups[g2].y2);
    uint32_t g;
    for(g = g1; g <= g2; g++) {
        const row_span_t * s = &groups[g].s;
        core->x1 = LV_MAX(core->x1, s->o1);
        core->x2 = LV_MIN(core->x2, s->o2);
        if(s->v1 <= s->v2) {
            vis->x1 = LV_MIN(vis->x1, s->v1);
            vis->x2 = LV_MAX(vis->x2, s->v2);
        }
    }
    /*Even-aligned, so the plane rows the copy starts on are 32-bit aligned*/
    core->x1 = LV_ALIGN_UP(core->x1, 2);
    core->x2 = ((core->x2 + 1) & ~1) - 1;
}

static uint32_t band_parts(const lv_area_t * core, const lv_area_t * vis, lv_area_t * areas, bool * opaque)
{
    if(vis->x1 > vis->x2) return 0;
    if(core->x1 > core->x2) {
        areas[0] = *vis;
        opaque[0] = false;
        return 1;
    }

    uint32_t n = 0;
    areas[n] = *core;
    opaque[n++] = true;
    if(vis->x1 < core->x1) {
        lv_area_set(&areas[n], vis->x1, vis->y1, core->x1 - 1, vis->y2);
        opaque[n++] = false;
    }
    if(vis->x2 > core->x2) {
        lv_area_set(&areas[n], core->x2 + 1, vis->y1, vis->x2, vis->y2);
        opaque[n++] = false;
    }
    return n;
}

/*Splits the rows into horizontal bands, each an opaque core between two
 *blended edges, choosing the bands with the lowest drawing cost.*/
static uint32_t split(const row_group_t * groups, uint32_t group_cnt, lv_area_t * areas, bool * opaque)
{
    if(group_cnt == 0) return 0;

    uint32_t * best = raster_malloc(sizeof(uint32_t) * (group_cnt + 1));
    uint32_t * from = raster_malloc(sizeof(uint32_t) * (group_cnt + 1));
    uint32_t n = 0;
    if(best && from) {
        /*best[i]: the lowest cost of drawing the first i groups; from[i]: where its last band starts*/
        best[0] = 0;
        uint32_t i;
        for(i = 1; i <= group_cnt; i++) {
            best[i] = UINT32_MAX;
            uint32_t j;
            for(j = i; j-- > 0 && i - j <= MAX_BAND_GROUPS;) {
                lv_area_t core, vis;
                band_cost(groups, j, i - 1, &core, &vis);
                lv_area_t a[3];
                bool o[3];
                uint32_t k, cnt = band_parts(&core, &vis, a, o);
                uint32_t cost = best[j] + cnt * COST_PART;
                for(k = 0; k < cnt; k++) cost += lv_area_get_size(&a[k]) * (o[k] ? 1 : COST_ALPHA_PX);
                if(cost < best[i]) {
                    best[i] = cost;
                    from[i] = j;
                }
            }
        }

        /*Walk back from the end; bands come out bottom first, which doesn't matter*/
        for(i = group_cnt; i > 0; i = from[i]) {
            lv_area_t core, vis;
            band_cost(groups, from[i], i - 1, &core, &vis);
            if(n + 3 > MAX_PARTS) {
                n = MAX_PARTS + 1;
                break;
            }
            n += band_parts(&core, &vis, &areas[n], &opaque[n]);
        }
    }

    if(n > MAX_PARTS || !best || !from) {
        /*Too irregular (or no memory to work it out): blend the whole visible extent*/
        lv_area_t core, vis;
        band_cost(groups, 0, group_cnt - 1, &core, &vis);
        lv_area_set(&core, 1, 0, 0, 0);
        n = band_parts(&core, &vis, areas, opaque);
    }
    raster_free(best);
    raster_free(from);
    return n;
}

static void copy_part(raster_part_t * part, const lv_draw_buf_t * src, uint8_t * dst, lv_color_format_t cf,
                      uint32_t stride)
{
    const lv_area_t * a = &part->area;
    int32_t w = lv_area_get_width(a);
    int32_t h = lv_area_get_height(a);
    int32_t y;
    for(y = 0; y < h; y++) {
        const lv_color32_t * s = lv_draw_buf_goto_xy(src, a->x1, a->y1 + y);
        uint8_t * d = dst + y * stride;
        if(cf == LV_COLOR_FORMAT_ARGB8888) {
            lv_memcpy(d, s, w * 4);
            continue;
        }
        if(cf == LV_COLOR_FORMAT_A8) {
            int32_t x;
            for(x = 0; x < w; x++) d[x] = s[x].alpha;
            continue;
        }
        uint16_t * d16 = (uint16_t *)d;
        int32_t x;
        for(x = 0; x < w; x++) {
            uint16_t c = (uint16_t)(((s[x].red & 0xF8) << 8) | ((s[x].green & 0xFC) << 3) | (s[x].blue >> 3));
            d16[x] = cf == LV_COLOR_FORMAT_RGB565_SWAPPED ? (uint16_t)(c << 8 | c >> 8) : c;
        }
    }

    /*Marked as allocated, like any draw buffer, so it's drawn as it is (it's never freed on its own)*/
    lv_draw_buf_t * buf = &part->buf;
    lv_memzero(buf, sizeof(*buf));
    buf->header.magic = LV_IMAGE_HEADER_MAGIC;
    buf->header.cf = cf;
    buf->header.w = w;
    buf->header.h = h;
    buf->header.stride = stride;
    buf->header.flags = LV_IMAGE_FLAGS_ALLOCATED;
    buf->data = dst;
    buf->unaligned_data = dst;
    buf->handlers = get_handlers();
    /*A part of the plane ends where its last row does, not a whole stride later*/
    buf->data_size = stride * (h - 1) + w * lv_color_format_get_size(cf);
}

static uint64_t cache_touch(void)
{
#ifdef ESP_PLATFORM
    return orb_memory_cache_touch();
#else
    return ++s_cache_clock;
#endif
}

#ifdef ESP_PLATFORM
static lv_obj_raster_t * oldest_cache(orb_memory_owner_t owner)
{
    lv_obj_raster_t * oldest = NULL;
    for(lv_obj_raster_t * r = raster_head; r; r = r->next) {
        if(r->pinned == s_prepare_epoch) continue;
        bool eligible = owner == ORB_MEMORY_MIPS ? r->sample_mip != NULL :
            r->mem && r->page == (owner == ORB_MEMORY_PAGE);
        if(eligible && (!oldest || r->used < oldest->used)) oldest = r;
    }
    return oldest;
}

static uint64_t cache_oldest(void * ctx)
{
    lv_obj_raster_t * r = oldest_cache((orb_memory_owner_t)(uintptr_t)ctx);
    return r ? r->used : UINT64_MAX;
}

static bool cache_evict(void * ctx)
{
    orb_memory_owner_t owner = (orb_memory_owner_t)(uintptr_t)ctx;
    lv_obj_raster_t * r = oldest_cache(owner);
    if(!r) return false;
    if(owner == ORB_MEMORY_MIPS) {
        orb_memory_release(ORB_MEMORY_MIPS, r->sample_bytes);
        s_sample_bytes -= r->sample_bytes;
        raster_free(r->sample_mip);
        r->sample_mip = NULL; r->sample_bytes = 0;
    }
    else { free_pixels(r); r->dirty = true; }
    s_cache_evictions++;
    return true;
}
#endif

static void * raster_buf_malloc(size_t bytes, lv_color_format_t cf)
{
    if(bytes >= s_failed_min) { s_negative_hits++; return NULL; }
    void * data = s_raw_malloc(bytes, cf);
    if(!data) s_failed_min = LV_MIN(s_failed_min, bytes);
    return data;
}

static void raster_buf_free(void * data) { s_raw_free(data); }

static lv_draw_buf_handlers_t * get_handlers(void)
{
    if(!raster_handlers_init) {
        lv_draw_buf_init_with_default_handlers(&raster_handlers);
        s_raw_malloc = raster_handlers.buf_malloc_cb;
        s_raw_free = raster_handlers.buf_free_cb;
        raster_handlers.buf_malloc_cb = raster_buf_malloc;
        raster_handlers.buf_free_cb = raster_buf_free;
        raster_handlers_init = true;
#ifdef ESP_PLATFORM
        orb_memory_cache_register(ORB_MEMORY_RASTER, cache_oldest, cache_evict, (void *)(uintptr_t)ORB_MEMORY_RASTER);
        orb_memory_cache_register(ORB_MEMORY_PAGE, cache_oldest, cache_evict, (void *)(uintptr_t)ORB_MEMORY_PAGE);
        orb_memory_cache_register(ORB_MEMORY_MIPS, cache_oldest, cache_evict, (void *)(uintptr_t)ORB_MEMORY_MIPS);
#endif
    }
    return &raster_handlers;
}

#if CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE
static void scale_cache_remove(raster_scale_entry_t ** link)
{
    raster_scale_entry_t * e = *link;
    *link = e->next;
    if(e->owner->scaled == e) e->owner->scaled = NULL;
    s_scale_stats.bytes -= e->bytes;
    raster_free(e->pixels);
    raster_free(e);
}

static void scale_cache_forget(lv_obj_raster_t * owner)
{
    raster_scale_entry_t ** link = &s_scale_head;
    while(*link) {
        if((*link)->owner == owner) scale_cache_remove(link);
        else link = &(*link)->next;
    }
    owner->scaled = NULL;
}

static bool scale_cache_admit(uint32_t bytes)
{
    const uint32_t budget = CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE_KIB * 1024u;
    if(bytes > budget) return false;
    uint32_t pinned = 0;
    for(raster_scale_entry_t * e = s_scale_head; e; e = e->next) {
        if(e->frame == s_scale_frame) pinned += e->bytes;
    }
    /*An impossible request must not churn other reusable variants before it
     *falls back. Worst-case AA output can be larger than its opaque payload.*/
    if(bytes > budget - pinned) return false;
    while(bytes > budget - s_scale_stats.bytes) {
        raster_scale_entry_t ** victim = NULL;
        raster_scale_entry_t ** link = &s_scale_head;
        /*The list is MRU first. Entries selected for this refresh stay pinned
         *until every band has finished, even under a deliberately tiny budget.*/
        while(*link) {
            if((*link)->frame != s_scale_frame) victim = link;
            link = &(*link)->next;
        }
        if(victim == NULL) return false;
        scale_cache_remove(victim);
        s_scale_stats.evictions++;
    }
    return true;
}

/*RGB565A8 keeps exact transformed coverage at the outer edge. Pulling only a
 *verified opaque rectangle into the display format enables orb_lv_simd copies
 *without an all-255 mask, and leaves the filter's original AA bytes untouched.*/
static uint8_t scale_cache_split(const uint8_t * alpha, int32_t w, int32_t h,
                                  lv_area_t * areas, bool * opaque)
{
    lv_area_t core = {0, -1, w - 1, -1};
    bool gap = false;
    for(int32_t y = 0; y < h; y++) {
        int32_t left = 0, right = w - 1;
        while(left < w && alpha[y * w + left] != LV_OPA_COVER) left++;
        while(right >= left && alpha[y * w + right] != LV_OPA_COVER) right--;
        if(left > right) {
            if(core.y1 >= 0) gap = true;
            continue;
        }
        if(gap) { core.x2 = -1; break; }
        if(core.y1 < 0) core.y1 = y;
        core.y2 = y;
        core.x1 = LV_MAX(core.x1, left);
        core.x2 = LV_MIN(core.x2, right);
    }
    bool valid = core.y1 >= 0 && core.x1 <= core.x2;
    /*An arbitrary mask can contain holes between the boundary samples. This
     *path currently admits only opaque matte sources, but verify rather than
     *making their future implementation part of the cache's safety contract.*/
    for(int32_t y = core.y1; valid && y <= core.y2; y++) {
        for(int32_t x = core.x1; x <= core.x2; x++) {
            if(alpha[y * w + x] != LV_OPA_COVER) { valid = false; break; }
        }
    }
    if(!valid) {
        lv_area_set(&areas[0], 0, 0, w - 1, h - 1);
        opaque[0] = false;
        return 1;
    }
    uint8_t n = 0;
    areas[n] = core;
    opaque[n++] = true;
    if(core.y1 > 0) {
        lv_area_set(&areas[n], 0, 0, w - 1, core.y1 - 1);
        opaque[n++] = false;
    }
    if(core.y2 < h - 1) {
        lv_area_set(&areas[n], 0, core.y2 + 1, w - 1, h - 1);
        opaque[n++] = false;
    }
    if(core.x1 > 0) {
        lv_area_set(&areas[n], 0, core.y1, core.x1 - 1, core.y2);
        opaque[n++] = false;
    }
    if(core.x2 < w - 1) {
        lv_area_set(&areas[n], core.x2 + 1, core.y1, w - 1, core.y2);
        opaque[n++] = false;
    }
    return n;
}

static void scale_cache_prepare(lv_obj_raster_t * r)
{
    if(r->dirty || !r->mem || !r->matte_on || r->part_cnt != 1 ||
       r->opa <= LV_OPA_MIN || r->scale == 0 || r->scale == LV_SCALE_NONE) return;
    raster_part_t * source = &r->parts[0];
    lv_color_format_t cf = source->buf.header.cf;
    if((cf != LV_COLOR_FORMAT_RGB565 && cf != LV_COLOR_FORMAT_RGB565_SWAPPED) ||
       source->area.x1 != 0 || source->area.y1 != 0 ||
       lv_area_get_width(&source->area) != r->w || lv_area_get_height(&source->area) != r->h) return;

    raster_scale_entry_t ** link = &s_scale_head;
    while(*link) {
        raster_scale_entry_t * e = *link;
        if(e->owner == r && e->scale == r->scale) {
            *link = e->next;
            e->next = s_scale_head;
            s_scale_head = e;
            e->frame = s_scale_frame;
            r->scaled = e;
            s_scale_stats.hits++;
            return;
        }
        link = &e->next;
    }
    s_scale_stats.misses++;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    uint64_t started = lv_draw_sw_profile_time_us();
#endif
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.scale_x = dsc.scale_y = r->scale;
    lv_area_t area;
    lv_image_buf_get_transformed_area(&area, r->w, r->h, 0, r->scale, r->scale, &dsc.pivot);
    int32_t w = lv_area_get_width(&area), h = lv_area_get_height(&area);
    if(w <= 0 || h <= 0 || (uint64_t)w * h * 3 + sizeof(raster_scale_entry_t) > UINT32_MAX) return;
    uint32_t temporary_bytes = (uint32_t)w * h * 3;
    /*Admit against the worst-case retained output before allocating scratch.
     *Transient transform storage is explicit PSRAM, never internal draw/DMA RAM.*/
    if(!scale_cache_admit(temporary_bytes + sizeof(raster_scale_entry_t))) return;
    uint8_t * temporary = raster_malloc(temporary_bytes);
    if(!temporary) { s_scale_stats.alloc_fail++; return; }
    lv_draw_sw_transform(&area, source->buf.data, r->w, r->h, source->buf.header.stride,
                         &dsc, NULL, cf, temporary);
    lv_area_t areas[5];
    bool opaque[5];
    uint8_t count = scale_cache_split(temporary + w * h * 2, w, h, areas, opaque);
    uint32_t payload = 0;
    for(uint8_t i = 0; i < count; i++) payload += lv_area_get_size(&areas[i]) * (opaque[i] ? 2 : 3);
    raster_scale_entry_t * e = raster_malloc(sizeof(*e));
    uint8_t * pixels = raster_malloc(payload);
    if(!e || !pixels) {
        raster_free(e);
        raster_free(pixels);
        raster_free(temporary);
        s_scale_stats.alloc_fail++;
        return;
    }
    lv_memzero(e, sizeof(*e));
    e->owner = r;
    e->scale = r->scale;
    e->frame = s_scale_frame;
    e->pixels = pixels;
    e->bytes = payload + sizeof(*e);
    e->part_cnt = count;
    uint8_t * next = pixels;
    for(uint8_t i = 0; i < count; i++) {
        raster_part_t * p = &e->parts[i];
        p->area = areas[i];
        lv_area_move(&p->area, area.x1, area.y1);
        uint32_t pw = lv_area_get_width(&areas[i]), ph = lv_area_get_height(&areas[i]);
        uint32_t bytes = pw * ph * (opaque[i] ? 2 : 3);
        lv_draw_buf_t * b = &p->buf;
        b->header.magic = LV_IMAGE_HEADER_MAGIC;
        b->header.cf = opaque[i] ? cf : LV_COLOR_FORMAT_RGB565A8;
        b->header.w = pw;
        b->header.h = ph;
        b->header.stride = pw * 2;
        b->header.flags = LV_IMAGE_FLAGS_ALLOCATED;
        b->data = b->unaligned_data = next;
        b->data_size = bytes;
        b->handlers = get_handlers();
        for(uint32_t y = 0; y < ph; y++) {
            for(uint32_t x = 0; x < pw; x++) {
                uint32_t from = (areas[i].y1 + y) * w + areas[i].x1 + x;
                uint16_t color;
                lv_memcpy(&color, temporary + from * 2, sizeof(color));
                if(opaque[i] && cf == LV_COLOR_FORMAT_RGB565_SWAPPED) color = lv_color_swap_16(color);
                lv_memcpy(next + (y * pw + x) * 2, &color, sizeof(color));
                if(!opaque[i]) next[pw * ph * 2 + y * pw + x] = temporary[w * h * 2 + from];
            }
        }
        next += bytes;
    }
    raster_free(temporary);
    e->next = s_scale_head;
    s_scale_head = e;
    r->scaled = e;
    s_scale_stats.bytes += e->bytes;
    s_scale_stats.peak_bytes = LV_MAX(s_scale_stats.peak_bytes, s_scale_stats.bytes);
    s_scale_stats.builds++;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    s_scale_stats.build_us += (uint32_t)(lv_draw_sw_profile_time_us() - started);
#endif
}
#endif

/*True if every visible pixel of the area has the same color, then it's drawn as A8*/
static bool one_color(const lv_draw_buf_t * src, const lv_area_t * a, lv_color_t * color)
{
    bool found = false;
    lv_color32_t c = {0};
    int32_t y, x;
    for(y = a->y1; y <= a->y2; y++) {
        const lv_color32_t * px = lv_draw_buf_goto_xy(src, 0, y);
        for(x = a->x1; x <= a->x2; x++) {
            if(px[x].alpha == 0) continue;
            if(!found) {
                c = px[x];
                found = true;
            }
            else if(px[x].red != c.red || px[x].green != c.green || px[x].blue != c.blue) {
                return false;
            }
        }
    }
    *color = lv_color_make(c.red, c.green, c.blue);
    return true;
}

static void * raster_malloc(size_t size)
{
    return get_handlers()->buf_malloc_cb(size, LV_COLOR_FORMAT_UNKNOWN);
}

static void raster_free(void * p)
{
    if(p) get_handlers()->buf_free_cb(p);
}

static lv_obj_tree_walk_res_t prepare_cb(lv_obj_t * obj, void * user_data)
{
    LV_UNUSED(user_data);
    lv_obj_raster_t * r = get_raster(obj);
    if(r == NULL) return LV_OBJ_TREE_WALK_NEXT;

    lv_area_t area;
    get_area(obj, &area);
    if(r->dirty || r->mem == NULL || lv_area_get_width(&area) != r->w || lv_area_get_height(&area) != r->h) {
        build(r);
    }
    return r->page && !r->dirty && r->mem ? LV_OBJ_TREE_WALK_SKIP_CHILDREN : LV_OBJ_TREE_WALK_NEXT;
}
