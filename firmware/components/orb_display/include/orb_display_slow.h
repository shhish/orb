#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Bench-only inclusive wall time, including preemption and wire waits. These
 * exclusive phases partition a handler; timer time overlaps them deliberately.
 * Keep only the four worst stalls, then emit after motion has finished. */
#define ORB_DISPLAY_PHASES 8
#define ORB_DISPLAY_SLOW_HANDLERS 4

typedef struct {
    uint64_t sequence, at_us, handler_us;
    uint64_t phase_us[ORB_DISPLAY_PHASES];
    uint64_t timer_us;
    uintptr_t timer_callback;
    bool moving;
} orb_display_slow_handler_t;

typedef struct {
    bool active;
    unsigned phase;
    int64_t phase_at, timer_at;
    uint64_t phase_us[ORB_DISPLAY_PHASES];
    uint64_t timer_max_us;
    uintptr_t timer_callback;
} orb_display_phase_clock_t;

static inline const char *orb_display_phase_name(unsigned phase)
{
    const char *names[ORB_DISPLAY_PHASES] = {
        "other", "prepare", "layout", "damage", "raster", "sync", "draw", "finish"
    };
    return phase < ORB_DISPLAY_PHASES ? names[phase] : "unknown";
}

static inline void orb_display_phase_begin(orb_display_phase_clock_t *clock, int64_t now)
{
    memset(clock, 0, sizeof(*clock));
    clock->active = true;
    clock->phase_at = now;
}

static inline void orb_display_phase_mark(orb_display_phase_clock_t *clock, int64_t now, unsigned phase)
{
    if (!clock->active || phase >= ORB_DISPLAY_PHASES) return;
    if (now >= clock->phase_at) clock->phase_us[clock->phase] += now - clock->phase_at;
    clock->phase = phase;
    clock->phase_at = now;
}

static inline void orb_display_phase_timer(orb_display_phase_clock_t *clock, int64_t now,
                                           uintptr_t callback, bool entering)
{
    if (!clock->active) return;
    if (entering) clock->timer_at = now;
    else if (now >= clock->timer_at) {
        uint64_t elapsed = now - clock->timer_at;
        if (elapsed > clock->timer_max_us) {
            clock->timer_max_us = elapsed;
            clock->timer_callback = callback;
        }
    }
}

static inline void orb_display_phase_finish(orb_display_phase_clock_t *clock, int64_t now,
                                            orb_display_slow_handler_t *sample)
{
    orb_display_phase_mark(clock, now, 0);
    memcpy(sample->phase_us, clock->phase_us, sizeof(sample->phase_us));
    sample->timer_us = clock->timer_max_us;
    sample->timer_callback = clock->timer_callback;
    clock->active = false;
}

static inline void orb_display_slow_insert(orb_display_slow_handler_t *worst,
                                           const orb_display_slow_handler_t *sample)
{
    if (sample->handler_us <= 33000) return;
    for (unsigned i = 0; i < ORB_DISPLAY_SLOW_HANDLERS; i++) {
        if (sample->handler_us <= worst[i].handler_us) continue;
        for (unsigned j = ORB_DISPLAY_SLOW_HANDLERS - 1; j > i; j--) worst[j] = worst[j - 1];
        worst[i] = *sample;
        return;
    }
}
