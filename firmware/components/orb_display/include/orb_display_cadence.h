#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "orb_display_slow.h"

/* UI-thread bench-only observations, independent of SCROLL_TRACE/accounting.
 * Inclusive wall times include preemption; frequency is sampled before handler
 * work, not continuously. Anim ticks are observed timer last_run changes. */
typedef struct {
    bool active, ui_cpu_available;
    float ui_cpu_pct;
    uint32_t ui_priority, ui_core;
    uint64_t loops, lock_us, handler_us, handler_max_us, handler_outside_refresh_us, refresh_us;
    uint64_t timer_wait_us, fairness_wait_us, wait_requested_us, wait_overrun_us, wait_max_us;
    uint64_t invalidations, invalidated_pixels, refreshes, render_starts;
    /* Exact union of retained refresh rectangles after blur expansion/joining;
     * request pixels above deliberately retain overlap for source diagnosis. */
    uint64_t damage_frames, damage_pixels, damage_max_pixels;
    uint64_t handler_stalls, animation_stalls;
    uint64_t phase_us[ORB_DISPLAY_PHASES], phase_max_us[ORB_DISPLAY_PHASES];
    orb_display_slow_handler_t slow[ORB_DISPLAY_SLOW_HANDLERS];
    uint64_t animation_ticks, animation_gap_max_us, animation_late_max_ms;
    uint64_t frequency_samples, frequency_below_max, moving_frequency_samples, moving_frequency_below_max;
    uint32_t frequency_min_mhz, frequency_max_mhz, moving_frequency_min_mhz, moving_frequency_max_mhz;
    uint32_t animation_period_ms, indev_period_ms;
} orb_display_cadence_t;

/* Call with lv_lock held; begin resets only this benchmark's observations.
 * end freezes them before reports/teardown. No allocation or console output. */
void orb_display_cadence_begin(void);
void orb_display_cadence_end(void);
/* Previous sampled movement window, matching bench_scroll moving CPU/FPS. */
void orb_display_cadence_set_moving(bool moving);
void orb_display_cadence_snapshot(orb_display_cadence_t *out);
