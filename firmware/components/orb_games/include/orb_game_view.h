#pragma once

#include "lvgl.h"
#include "orb_game_logic.h"

/* All view APIs are UI-thread/LVGL-lock owned. No timer, model step or storage
 * lives here; update only damages changed sprite old/new bounds (HIG §7). */
typedef struct orb_game_view orb_game_view_t;
#define ORB_GAME_CLOSE 100
/* action is orb_game_action_t or ORB_GAME_CLOSE. */
typedef void (*orb_game_view_action_cb_t)(int action, float x, float y, void *context);
typedef struct {
    uint64_t updates, changed_updates, dirty_pixels, update_us;
    uint32_t last_dirty_pixels, max_dirty_pixels;
    uint32_t ring_cache_bytes;
    uint64_t bead_draw_calls, bead_masks_missing, bead_ineligible, bead_serial, bead_dispatched;
    uint64_t bead_drain_us, bead_native_us;
    uint64_t bead_custom_slices, bead_custom_us, bead_custom_max_us, bead_sync_slices, bead_sync_us;
    uint32_t bead_prepares, bead_rejected_prepares;
    uint16_t bead_total_sprites, bead_batch_sprites, bead_reject_index, bead_reject_radius;
    uint8_t bead_reject_shape, bead_reject_flags;
    bool bead_masks_ready, bead_source_ready, bead_eligible;
} orb_game_view_stats_t;
orb_game_view_t *orb_game_view_create(lv_obj_t *parent, orb_game_kind_t kind,
                                     orb_game_view_action_cb_t callback, void *context);
void orb_game_view_update(orb_game_view_t *view, const orb_game_state_t *state,
                          uint32_t high_score, bool paused, bool save_error);
void orb_game_view_destroy(orb_game_view_t *view);
void orb_game_view_stats(const orb_game_view_t *view, orb_game_view_stats_t *out);
#ifndef ESP_PLATFORM
/* Simulator oracle exercises the production allocation-failure fallback. */
void orb_game_view_test_native_rings(orb_game_view_t *view);
void orb_game_view_test_serial_orbit(orb_game_view_t *view, bool serial);
#endif
