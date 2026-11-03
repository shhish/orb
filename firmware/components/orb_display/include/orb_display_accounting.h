#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bounded diagnostics, independent of the old 4 ms render-latency buckets.
 * Interval buckets have 1 ms resolution; the last bucket is unbounded. */
#define ORB_DISPLAY_CADENCE_BUCKETS 128
#define ORB_DISPLAY_BAND_TRACE 8

typedef struct {
    int64_t draw_start_us, queued_us, sender_us, dma_start_us, dma_end_us;
    uint32_t bytes;
    int16_t y1, y2;
    bool ok;
} orb_display_band_trace_t;

typedef struct {
    uint64_t scheduled, completed, dropped;
    uint64_t preparation_us, scene_prepare_us, whole_update_us, cold_prepare_us;
    uint32_t completion_interval[ORB_DISPLAY_CADENCE_BUCKETS];
    uint32_t whole_update[ORB_DISPLAY_CADENCE_BUCKETS];
    uint64_t band_sequence;
    orb_display_band_trace_t bands[ORB_DISPLAY_BAND_TRACE];
    uint64_t paced_scheduled, paced_on_slot, paced_late, paced_skipped;
    uint64_t paced_boundaries, paced_unpaced, paced_admitted_30;
    uint32_t paced_period_us, paced_divisor, paced_max_error_us;
} orb_display_accounting_t;

/* Caller serializes access. Completed means final SPI result drain, never
 * FLUSH_FINISH or an ISR. An abandoned presentation is counted exactly once. */
static inline void orb_accounting_complete(orb_display_accounting_t *stats,
                                           int64_t *previous_completion,
                                           int64_t start, int64_t completion, bool ok)
{
    if (!ok) {
        stats->dropped++;
        return;
    }
    stats->completed++;
    uint64_t elapsed = completion >= start ? (uint64_t)(completion - start) : 0;
    stats->whole_update_us += elapsed;
    unsigned bucket = elapsed / 1000;
    if (bucket >= ORB_DISPLAY_CADENCE_BUCKETS) bucket = ORB_DISPLAY_CADENCE_BUCKETS - 1;
    stats->whole_update[bucket]++;
    if (*previous_completion) {
        uint64_t interval = completion - *previous_completion;
        bucket = interval / 1000;
        if (bucket >= ORB_DISPLAY_CADENCE_BUCKETS) bucket = ORB_DISPLAY_CADENCE_BUCKETS - 1;
        stats->completion_interval[bucket]++;
    }
    *previous_completion = completion;
}

static inline void orb_accounting_band(orb_display_accounting_t *stats,
                                       orb_display_band_trace_t band)
{
    stats->bands[stats->band_sequence % ORB_DISPLAY_BAND_TRACE] = band;
    stats->band_sequence++;
}
