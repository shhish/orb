#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "orb_display_cadence.h"

#define ORB_SCROLL_STALL_LIMIT_US 33000u
#define ORB_SCROLL_DAMAGE_LIMIT_PERMILLE 1100u

typedef struct {
    unsigned target_fps;
    bool fps_pass, stall_pass, request_invalidation_pass, damage_pass, invalidation_pass;
} orb_scroll_gates_t;

/* Compare integer observations before formatting FPS. A rounded 49.995 fps
 * receipt must not pass 50 fps, nor may one oversized frame hide in a mean.
 * Sample durations are bounded to 60 seconds by bench_scroll. */
static inline orb_scroll_gates_t orb_scroll_gates(bool fast_list, uint64_t frames, uint64_t moving_frames,
                                                uint64_t moving_us, uint32_t viewport_pixels,
                                                const orb_display_cadence_t *cadence)
{
    orb_scroll_gates_t gates = {.target_fps = fast_list ? 50u : 30u};
    gates.fps_pass = moving_us && moving_frames * 1000000u >= moving_us * gates.target_fps;
    /* Home's spring has its own timer; it can move without any lv_anim tick.
     * All its callbacks are still covered by the handler wall-time maximum. */
    gates.stall_pass = cadence->loops &&
        cadence->handler_max_us <= ORB_SCROLL_STALL_LIMIT_US &&
        cadence->animation_gap_max_us <= ORB_SCROLL_STALL_LIMIT_US &&
        !cadence->handler_stalls && !cadence->animation_stalls;
    gates.request_invalidation_pass = viewport_pixels && frames &&
        cadence->invalidated_pixels * 1000u <=
        frames * viewport_pixels * ORB_SCROLL_DAMAGE_LIMIT_PERMILLE;
    gates.damage_pass = viewport_pixels && cadence->damage_frames &&
        cadence->damage_max_pixels * 1000u <=
        (uint64_t)viewport_pixels * ORB_SCROLL_DAMAGE_LIMIT_PERMILLE;
    gates.invalidation_pass = gates.request_invalidation_pass && gates.damage_pass;
    return gates;
}
