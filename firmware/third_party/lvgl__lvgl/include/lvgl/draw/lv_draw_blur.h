/**
 * @file lv_draw_blur.h
 *
 */

#ifndef LV_DRAW_BLUR_H
#define LV_DRAW_BLUR_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "../config/lv_conf_internal.h"
#include "lv_draw.h"
#include "lv_color.h"
#include "../core/lv_area.h"
#include "../core/lv_style.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    lv_draw_dsc_base_t base;

    /**
     * The intensity of blur.
     */
    int32_t blur_radius;

    /**
     * The corner radius of the blurred area
     */
    int32_t corner_radius;

    /**
     * Sets whether to prefer speed or precision
     */
    lv_blur_quality_t quality;

    /**
     * ORB PATCH: a tint mixed into the result as it is written, as a frosted
     * surface's background. Tint is mixed during bilinear reconstruction.
     * 0 opacity: none.
     */
    lv_color_t tint;
    lv_opa_t tint_opa;

    /* ORB: averaged low-resolution motion sample; zero retains normal quality.
     * Kept in the descriptor so workers never inspect live UI state. */
    uint8_t downsample_factor;
    /* UI-thread snapshot: moving backdrops bypass retained/copy caches. */
    bool backdrop_moving;

} lv_draw_blur_dsc_t;

#if LV_USE_DRAW_SW
#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
/* Orb's prepared, reduced material renderer. Install before dispatch starts;
 * false preserves the reference renderer for unsupported inputs or OOM. */
typedef bool (*lv_draw_sw_blur_fused_cb_t)(lv_draw_task_t *, const lv_draw_blur_dsc_t *, const lv_area_t *);
void lv_draw_sw_blur_set_fused_cb(lv_draw_sw_blur_fused_cb_t callback);
#endif
/* Stage 1 counters also count exact-path kernels in Stage 0. No hot-loop I/O. */
typedef struct {
    uint64_t map_hits, map_builds, base_hits, base_builds;
    uint64_t source_recaptures, blur_kernels, fallbacks;
    uint64_t preparation_us;
    uint64_t solid_fills, moving_bypasses;
    size_t bytes, peak_bytes, budget_bytes;
    uint32_t entries;
} lv_draw_sw_material_stats_t;
void lv_draw_sw_material_get_stats(lv_draw_sw_material_stats_t * out);

/* Admit only Orb's managed glass controls. Unsupported scenes, custom drawing,
 * OOM, or an incomplete dependency list use the exact compatibility renderer.
 * max_width/height include every actual press geometry, not just SDF sizes.
 * A new screen retires inactive-screen leases; Orb retries at DRAW_MAIN_BEGIN.
 * Returns true only for a newly admitted lease, false for an existing lease. */
bool lv_obj_material_register(lv_obj_t * obj, int32_t max_width, int32_t max_height,
                             int32_t padding, void * (*alloc_cb)(size_t), void (*free_cb)(void *));
void lv_obj_material_unregister(lv_obj_t * obj);
void lv_obj_material_capture_base(lv_layer_t * layer, lv_obj_t * obj);
void lv_obj_material_set_enabled(bool enabled);
void lv_obj_material_set_active(lv_obj_t * obj, bool active);
#if defined(CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST) && CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST
/* Exact A/B seam; quiesce draw workers before changing it. Solid detection
 * verifies every RGB565 backdrop pixel, never a sparse sample. */
void lv_draw_sw_blur_set_solid_enabled(bool enabled);
#endif
/* ORB: exact, bounded backdrop cache. Disabled after lv_init until configured.
 * Hooks must be supplied together; NULL uses lv_malloc/lv_free. Embedded
 * targets should supply their PSRAM allocator explicitly. Four leased slots
 * share budget_bytes (payload bytes, excluding allocator overhead); one entry
 * defaults to at most 96 KiB. */
typedef void * (*lv_draw_sw_blur_cache_alloc_cb_t)(size_t bytes);
typedef void (*lv_draw_sw_blur_cache_free_cb_t)(void * pointer);
typedef struct {
    size_t budget_bytes;       /* zero disables and frees the cache */
    size_t max_entry_bytes;    /* zero selects min(budget_bytes, 96 KiB) */
    lv_draw_sw_blur_cache_alloc_cb_t alloc_cb;
    lv_draw_sw_blur_cache_free_cb_t free_cb;
} lv_draw_sw_blur_cache_config_t;
typedef struct {
    uint64_t hits, misses, evictions;
    uint64_t busy_bypasses, oversize_bypasses, allocation_failures;
    size_t bytes, peak_bytes, budget_bytes; /* bytes includes in-flight reservations */
    uint32_t leased_entries;
} lv_draw_sw_blur_cache_stats_t;

/* Configure after lv_init, before the first screen is drawn. For runtime A/B,
 * stop dispatching and call lv_draw_wait_for_finish first. Both configure and
 * clear return INVALID without changing anything while a slot is leased.
 * Configure resets statistics; clear preserves counters and the configuration.
 * Allocator callbacks run without the metadata lock. Deinit frees all entries
 * after draw workers stop. Quiesce draws before lv_deinit, especially on OSes
 * that terminate workers instead of joining them. No cache API may race
 * lv_init/lv_deinit. */
lv_result_t lv_draw_sw_blur_cache_configure(const lv_draw_sw_blur_cache_config_t * config);
lv_result_t lv_draw_sw_blur_cache_clear(void);
void lv_draw_sw_blur_cache_get_stats(lv_draw_sw_blur_cache_stats_t * out);
#endif

#if (defined(CONFIG_ORB_DISPLAY_GLASS_MOTION_QUALITY) && CONFIG_ORB_DISPLAY_GLASS_MOTION_QUALITY) || \
    (defined(CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) && CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) || \
    (defined(CONFIG_ORB_DISPLAY_TE_PACING) && CONFIG_ORB_DISPLAY_TE_PACING)
/* UI-thread only. Scroll/translated content notes actual movement. The settle
 * timer restores normal quality and/or settled material admission even if
 * content stops invalidating. Pacing consumes the same movement lifecycle. */
void lv_draw_sw_blur_note_motion(lv_display_t * display);
void lv_draw_sw_blur_note_object_motion(lv_obj_t * obj);
bool lv_draw_sw_blur_motion_active(lv_display_t * display);
#endif

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Initialize a blur draw descriptor
 * @param dsc       pointer to a draw descriptor
 */
void lv_draw_blur_dsc_init(lv_draw_blur_dsc_t * dsc);

/**
 * Try to get a blur draw descriptor from a draw task.
 * @param task      draw task
 * @return          the task's draw descriptor or NULL if the task is not of type LV_DRAW_TASK_TYPE_BLUR
 */
lv_draw_blur_dsc_t * lv_draw_task_get_blur_dsc(lv_draw_task_t * task);

/**
 * Create a blur draw task
 * @param layer     pointer to a layer
 * @param dsc       pointer to an initialized `lv_draw_blur_dsc_t` variable
 * @param coords    coordinates of the character
 */
void lv_draw_blur(lv_layer_t * layer, const lv_draw_blur_dsc_t * dsc, const lv_area_t * coords);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_DRAW_BLUR_H*/
