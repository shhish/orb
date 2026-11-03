/**
 * @file lv_obj_raster.h
 *
 * ORB PATCH: raster cache (see ORB_PATCHES.md).
 *
 * Core Animation's `shouldRasterize` for LVGL: a widget marked with
 * lv_obj_set_raster() is drawn once, children included, into a bitmap, and
 * frame after frame that bitmap is copied to the screen instead of drawing the
 * widget again. Moving the widget (scrolling, layout) keeps the bitmap; a
 * change inside it (style, state, size, text, a child added, moved or removed)
 * redraws it the next time it's on screen.
 *
 * The bitmap is split into an opaque part in the display's color format, which
 * is a plain copy, and small parts for the edges (rounded corners,
 * anti-aliasing), which are blended: A8 with a color where the part has one
 * color (text), ARGB8888 otherwise.
 */

#ifndef LV_OBJ_RASTER_H
#define LV_OBJ_RASTER_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lv_obj.h"
#include "../draw/lv_draw_buf.h"

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    uint32_t source_bytes, source_budget, page_bytes, page_budget;
    uint32_t evictions, rejections, negative_hits;
    uint32_t count;     /**< widgets with a raster cache */
    uint32_t bytes;     /**< pixels held by them */
    uint32_t builds;    /**< bitmaps drawn since start */
    uint64_t build_us;  /**< source-build elapsed sum; zero without ORB draw profiling */
    uint32_t mip_builds, mip_bytes, mip_budget;
} lv_obj_raster_stats_t;

typedef struct {
    uint32_t hits, misses, builds, evictions, alloc_fail;
    uint32_t bytes, peak_bytes, budget;
    uint32_t blits;             /**< band draws that use already-scaled IMAGE tasks */
    uint32_t build_us;          /**< cumulative creation time; available with draw profiling */
} lv_obj_raster_scale_cache_stats_t;

typedef struct {
    uint32_t rows, glyphs, bytes, peak_bytes, budget;
    uint32_t builds, fallbacks, draws, pixels;
    uint32_t evictions;
    uint32_t size_builds, size_reuses; /**< Unique immutable A4 payloads and duplicate size buckets */
} lv_obj_raster_rows_stats_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Draw a widget and its children from a cached bitmap.
 * Widgets with a transformation or `opa_layered` are drawn as usual, so a press
 * animation that scales the widget doesn't need a new bitmap every frame.
 * @param obj   pointer to a widget
 * @param en    true: cache; false: draw as usual and free the bitmap
 */
void lv_obj_set_raster(lv_obj_t * obj, bool en);

/**
 * Draw the raster cache over a solid color when it's made, for a widget that
 * always sits on that color (e.g. the black canvas of an AMOLED UI): the
 * bitmap is then opaque, and drawing it is a plain copy, corners included.
 * Turns the raster cache on.
 * @param obj   pointer to a widget
 * @param color the color the widget always sits on
 */
void lv_obj_set_raster_matte(lv_obj_t * obj, lv_color_t color);

/**
 * The opacity a raster cached widget is composited with, as a Core Animation
 * layer's: changing it keeps the bitmap (a style opacity would redraw it).
 * @param obj   pointer to a raster cached widget
 * @param opa   LV_OPA_TRANSP .. LV_OPA_COVER
 */
void lv_obj_set_raster_opa(lv_obj_t * obj, lv_opa_t opa);

/**
 * Shrink the cached bitmap about its center without rebuilding it. The logical
 * layout and hit area stay unchanged. Intended for non-interactive round-list
 * rows; 256 is native size and 0 hides the bitmap. Larger values clamp to 256.
 */
void lv_obj_set_raster_scale(lv_obj_t * obj, uint16_t scale);

/**
 * @param obj   pointer to a widget
 * @return      true if the widget is drawn from a raster cache
 */
bool lv_obj_get_raster(const lv_obj_t * obj);

/** Whether a raster compositor can preserve its widget's normal cover test. */
bool lv_obj_raster_is_opaque(const lv_obj_t * obj);

/**
 * Redraw the bitmap of a raster cached widget, or of the nearest raster cached
 * ancestor. For changes LVGL can't see, e.g. a draw event that reads app state.
 * @param obj   pointer to a widget
 */
void lv_obj_invalidate_raster(lv_obj_t * obj);

/**
 * Draw now any missing or outdated bitmaps of `obj` and its descendants, e.g.
 * for content that will scroll into view, so it doesn't cost a frame later.
 * Not allowed while rendering.
 * @param obj   pointer to a widget
 */
void lv_obj_raster_prepare(lv_obj_t * obj);

/** Whether the current source is retained and needs no cold preparation. */
bool lv_obj_raster_ready(const lv_obj_t * obj);

/**
 * Set the allocator for raster bitmaps, by default LVGL's: e.g. to keep them in
 * a large, slower memory (PSRAM). Set it before the first cache is drawn.
 * @param malloc_cb     allocates (the color format argument can be ignored)
 * @param free_cb       frees what `malloc_cb` returned
 */
void lv_obj_raster_set_allocator(lv_draw_buf_malloc_cb_t malloc_cb, lv_draw_buf_free_cb_t free_cb);

/**
 * @param stats     filled with the current totals
 */
void lv_obj_raster_get_stats(lv_obj_raster_stats_t * stats);

/** Start a new input workload. Failed allocation sizes remain suppressed until
 * this boundary; never retry an oversized/failed cache on every scroll frame. */
void lv_obj_raster_cache_retry(void);

/** Release the completed refresh's cache pins before an explicit memory trim.
 * Hold the LVGL lock and drain drawing first; the next preparation repins sources. */
void lv_obj_raster_cache_release_frame(void);

/** Defer optional cold preparation during a locked memory transaction.
 * Resume before the next refresh, which prepares dirty sources normally. */
void lv_obj_raster_cache_set_paused(bool paused);

/** Exact-scale row variants; payload and metadata are included in raster bytes.
 * Creation precedes RENDER_START, so consult builds/build_us as well as DRAW.
 * With the Kconfig option disabled these counters are zero. */
void lv_obj_raster_scale_cache_get_stats(lv_obj_raster_scale_cache_stats_t * stats);

/** A/B selection for the next refresh. Call from the LVGL thread between frames. */
void lv_obj_raster_scale_cache_set_enabled(bool enabled);

/** Optional glyph-at-size rows. Unsupported subtrees retain the raster path;
 * the default-off Kconfig makes this a no-op in ordinary builds. */
void lv_obj_set_raster_rows(lv_obj_t * obj, bool enabled);
/** Immutable native row content; below preview_scale only title is presented,
 * centered at the 20px glyph floor, without changing font/layout/tree state. */
void lv_obj_set_raster_rows_focus(lv_obj_t * obj, lv_obj_t * title, uint16_t preview_scale, uint16_t minimum_scale);
bool lv_obj_raster_rows_ready(const lv_obj_t * obj);
bool lv_obj_raster_presentation_visible(const lv_obj_t * obj);
void lv_obj_raster_rows_get_stats(lv_obj_raster_rows_stats_t * stats);
/** Release unpinned atlas glyphs between frames, e.g. under memory pressure. */
void lv_obj_raster_rows_drop_unused(void);
/** Cold-admission metadata callback. 0 rejects unknown fonts safely. */
void lv_obj_raster_rows_set_font_size_cb(uint16_t (*cb)(const lv_font_t * font));

/** Current retained presentation for reduced glass backdrop sampling. No
 * allocation; false outside the transformed matte root or for unsupported
 * raster formats. Alpha is presentation opacity, RGB is unpremultiplied. */
bool lv_obj_raster_sample_pixel(lv_obj_t * obj, int32_t x, int32_t y, lv_color32_t * color);
bool lv_obj_raster_sample_area(lv_obj_t * obj, const lv_area_t * area, lv_color32_t * color);
bool lv_obj_raster_sample_ready(lv_obj_t * obj);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_OBJ_RASTER_H*/
