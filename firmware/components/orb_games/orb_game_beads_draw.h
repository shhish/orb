#pragma once

#include "orb_game_native.h"

typedef struct orb_game_beads_draw orb_game_beads_draw_t;
typedef struct {
    uint64_t custom_calls, custom_us, custom_max_us, sync_calls, sync_us;
} orb_game_beads_draw_stats_t;
/* UI-only aggregates of retired slices. Slice wall spans can overlap; they
 * include preemption and are not exclusive CPU or additive frame latency. */
void orb_game_beads_draw_stats(const orb_game_beads_draw_t *source, orb_game_beads_draw_stats_t *out);

/* Optional, bounded PSRAM source/pose charged to the existing games budget.
 * Coverage is copied at creation; no view or caller pointer escapes to tasks. */
orb_game_beads_draw_t *orb_game_beads_draw_create(const orb_game_native_beads_t *masks);
void orb_game_beads_draw_release(orb_game_beads_draw_t *source);
/* UI only. false requests the existing synchronous path; true owns each
 * accepted task until UI completion/FAILED/abort retirement. revision changes
 * whenever the integer position, opacity, count or colour arrays can change. */
bool orb_game_beads_draw_dispatch(orb_game_beads_draw_t *source, lv_layer_t *layer,
                                   size_t count, const int16_t *x, const int16_t *y,
                                   const uint8_t *opacity, const orb_game_sprite_t *sprites,
                                   uint64_t revision);
