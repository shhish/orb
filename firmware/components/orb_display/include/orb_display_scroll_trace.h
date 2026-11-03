#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Debug-only, bounded capture. No printing/allocation occurs while appending.
 * Coordinates are literal x/y; pressed and released use separate event kinds.
 * WAIT_BEGIN/END: a=requested/actual microseconds, b=1 timer or 2 fairness.
 * LOOP_END: a=UI lock + handler elapsed microseconds, b=next timer milliseconds.
 * INVALIDATE: a=first y, b=last y of the panel-rounded dirty rectangle. */
typedef enum {
    ORB_SCROLL_TRACE_TOUCH_SAMPLE = 1,
    ORB_SCROLL_TRACE_TOUCH_RELEASE,
    ORB_SCROLL_TRACE_INPUT_READ,
    ORB_SCROLL_TRACE_INPUT_RELEASE,
    ORB_SCROLL_TRACE_INPUT_INJECT,
    ORB_SCROLL_TRACE_INJECT_RELEASE,
    ORB_SCROLL_TRACE_SCROLL,
    ORB_SCROLL_TRACE_INVALIDATE,
    ORB_SCROLL_TRACE_REFR_START,
    ORB_SCROLL_TRACE_REFR_END,
    ORB_SCROLL_TRACE_RENDER_START,
    ORB_SCROLL_TRACE_COMPLETE,
    ORB_SCROLL_TRACE_LOOP_BEGIN,
    ORB_SCROLL_TRACE_LOOP_END,
    ORB_SCROLL_TRACE_WAIT_BEGIN,
    ORB_SCROLL_TRACE_WAIT_END,
} orb_display_scroll_trace_kind_t;

/* Keep a complete 1–2 s diagnostic bounded on the USB console. Summary
 * counters/histograms continue after this raw prefix fills. */
#define ORB_SCROLL_TRACE_CAPACITY 2048u
#define ORB_SCROLL_TRACE_INTERVAL_BUCKETS 128u
typedef struct {
    int64_t at_us;
    int32_t a, b;
    uint8_t kind;
} orb_display_scroll_trace_event_t;

typedef struct {
    bool available, active;
    int64_t started_us, ended_us;
    uint32_t count, dropped, completions, loops;
    uint64_t handler_us, timer_wait_us, fairness_wait_us;
    /* Complete-to-complete intervals, 1 ms bins; last includes >=127 ms.
     * Summary keeps counting after the finite raw-event capture fills. */
    uint32_t intervals[ORB_SCROLL_TRACE_INTERVAL_BUCKETS];
} orb_display_scroll_trace_t;

bool orb_display_scroll_trace_begin(void);
void orb_display_scroll_trace_end(void);
void orb_display_scroll_trace_record(orb_display_scroll_trace_kind_t kind, int64_t at_us,
                                     int32_t a, int32_t b);
void orb_display_scroll_trace_snapshot(orb_display_scroll_trace_t *out);
bool orb_display_scroll_trace_read(unsigned index, orb_display_scroll_trace_event_t *out);
