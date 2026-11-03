#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Three admitted RGB565 planes, never a per-frame output plane. The caller
 * captures settled LVGL content; render_band writes directly into DMA bands. */
#define ORB_NAV_SNAPSHOT_SLOTS 3

typedef struct orb_nav_compositor orb_nav_compositor_t;
typedef enum {
    ORB_NAV_TRANSITION_SLIDE,
    ORB_NAV_TRANSITION_IRIS,
    ORB_NAV_TRANSITION_SHEET,
    ORB_NAV_TRANSITION_BACK,
    ORB_NAV_TRANSITION_FADE,
    /* An unavailable map neighbour: move just the current plane, then return. */
    ORB_NAV_TRANSITION_BOUNDARY,
} orb_nav_transition_kind_t;
typedef enum {
    ORB_NAV_DIRECTION_LEFT,
    ORB_NAV_DIRECTION_RIGHT,
    ORB_NAV_DIRECTION_UP,
    ORB_NAV_DIRECTION_DOWN,
} orb_nav_direction_t;
typedef struct { int16_t x, y, width, height; } orb_nav_token_rect_t;
typedef struct {
    uint32_t captures, bands, frames;
    uint32_t background_prepares, background_hits;
    uint64_t pixels;
    size_t snapshot_bytes, sprite_bytes;
} orb_nav_compositor_stats_t;
typedef void *(*orb_nav_snapshot_alloc_fn)(size_t bytes, void *context);
typedef void (*orb_nav_snapshot_free_fn)(void *pixels, void *context);

orb_nav_compositor_t *orb_nav_compositor_create(uint16_t width, uint16_t height);
/* Called before fonts, caches or network startup. The first matching create
 * claims these already allocated planes; allocation failure is a boot error. */
bool orb_nav_compositor_reserve_boot(uint16_t width, uint16_t height);
/* Optional admission allocator for controlled memory pools and failure tests. */
orb_nav_compositor_t *orb_nav_compositor_create_with_allocator(uint16_t width, uint16_t height,
    orb_nav_snapshot_alloc_fn allocate, orb_nav_snapshot_free_fn release, void *context);
void orb_nav_compositor_destroy(orb_nav_compositor_t *c);
/* Default create reserves all three planes before any capture/cache activity.
 * Controlled allocators may use lazy admission for fault-injection tests. */
bool orb_nav_compositor_capture_begin(orb_nav_compositor_t *c, unsigned slot, uint64_t revision);
/* Retain the previous place for Back without copying/re-rasterizing it. */
bool orb_nav_compositor_swap(orb_nav_compositor_t *c, unsigned a, unsigned b);
uint16_t *orb_nav_compositor_pixels(orb_nav_compositor_t *c, unsigned slot);
bool orb_nav_compositor_capture_band(orb_nav_compositor_t *c, unsigned slot,
                                    int x, int y, int width, int height,
                                    const void *pixels, size_t stride, bool swapped);
/* Complete is for a full off-screen raster directly into pixels(slot). */
bool orb_nav_compositor_capture_complete(orb_nav_compositor_t *c, unsigned slot);
bool orb_nav_compositor_capture_end(orb_nav_compositor_t *c, unsigned slot);
bool orb_nav_compositor_valid(const orb_nav_compositor_t *c, unsigned slot, uint64_t revision);
void orb_nav_compositor_invalidate(orb_nav_compositor_t *c, unsigned slot);
void orb_nav_compositor_invalidate_all(orb_nav_compositor_t *c);

bool orb_nav_compositor_begin(orb_nav_compositor_t *c, orb_nav_transition_kind_t kind,
                             unsigned from, unsigned to, orb_nav_direction_t direction,
                             const orb_nav_token_rect_t *token, bool reverse, bool reduce_motion);
/* Configure/return the proven-invisible reverse-Iris prefix on a round panel,
 * including antialiased panel-edge pixels and the rim. Cancellation settles
 * at this source-identical endpoint; the rendering math is unchanged. */
float orb_nav_compositor_reverse_iris_round_start(orb_nav_compositor_t *c);
/* Isolate the adaptive black/white glyph from the flat launch disc. Its tiny
 * cached A8 sprite rides to the real destination header; no disc covers text. */
bool orb_nav_compositor_token_glyph(orb_nav_compositor_t *c,
                                  const orb_nav_token_rect_t *header, uint16_t ink);
/* Progress is normalized displacement, with a diminishing response past bounds.
 * Grabbing an in-flight transition preserves its current presentation. */
void orb_nav_compositor_drag(orb_nav_compositor_t *c, float progress);
void orb_nav_compositor_release(orb_nav_compositor_t *c, bool commit, float velocity_per_second);
void orb_nav_compositor_retarget(orb_nav_compositor_t *c, bool commit);
bool orb_nav_compositor_tick(orb_nav_compositor_t *c, uint32_t elapsed_ms);
void orb_nav_compositor_cancel(orb_nav_compositor_t *c);
bool orb_nav_compositor_active(const orb_nav_compositor_t *c);
float orb_nav_compositor_progress(const orb_nav_compositor_t *c);
bool orb_nav_compositor_committed(const orb_nav_compositor_t *c);
/* This only reads admitted immutable snapshots. No LVGL tree or allocations. */
bool orb_nav_compositor_render_band(orb_nav_compositor_t *c, int x, int y,
                                   int width, int height, void *pixels,
                                   size_t stride, bool swapped);
void orb_nav_compositor_stats(const orb_nav_compositor_t *c, orb_nav_compositor_stats_t *stats);

#ifdef __cplusplus
}
#endif
