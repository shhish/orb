#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Latest physical touch sample, safe off the LVGL thread. Gesture services
 * use this to suppress case taps while a finger operates the screen. */
bool orb_display_touch_pressed(void);
/* All touch initialization steps succeeded; sleep must fail closed otherwise. */
bool orb_display_touch_ready(void);
/* Physical GPIO wake only, with interrupts masked: queue a fresh controller
 * read through the ordinary touch task; never synthesizes a contact. */
void orb_display_touch_sleep_wake(void);

#include "esp_err.h"
#include "lvgl.h"
#include "orb_display_accounting.h"
#include "orb_display_response.h"
#include "orb_display_cpu.h"
#include "orb_display_cadence.h"
#include "orb_display_scroll_trace.h"

/*
 * LVGL on the panel. Frames are drawn in bands in internal RAM and each band
 * is DMA'd straight to the panel, timed so its write never crosses the
 * panel's scan (no tearing).
 *
 * LVGL runs on its own task. Anything that touches LVGL from another task
 * takes lv_lock() first.
 */
esp_err_t orb_display_start(void);

/* Admit adaptive halves of the existing render arena after UI/network startup
 * and reserve checks. Small live refreshes and immutable snapshot transitions
 * overlap; other large refreshes retain a full band. Safe from any task;
 * disabled builds make this a no-op. */
void orb_display_request_double_buffer(void);

/* UI-thread only. True certifies that an opaque immutable snapshot compositor
 * covers the entire display: no live glass or other neighbourhood reads may
 * remain visible. Its full-frame damage may use half-band ping-pong and skip
 * the live glass band guard. Clear before returning to live content; clearing
 * fences pending DMA and restores the full arena before any live drawing. */
void orb_display_set_snapshot_transition(bool active);

void orb_display_brightness(int pct);
/* UI-thread power fence. Requests serialize with all pixel DMA; the 120 ms
 * panel settling window runs on lcd_send. Completion wakes the UI. */
bool orb_display_panel_awake(bool awake);
bool orb_display_panel_ready(bool awake);
bool orb_display_set_brightness(int pct);
/* UI-thread only: dark surfaces retain damage without rendering/flushing.
 * Waking forces a whole safe frame before the power adapter can brighten. */
void orb_display_set_panel_on(bool on);
/* Input/service threads may interrupt the UI's idle deadline. */
void orb_display_wake(void);
/* Explicit queued UI work may run one ordinary LVGL handler pass while dark.
 * This requests service only, never viewing intent or semantic input. */
void orb_display_wake_service(void);
/* UI-thread only: dark input notifications make this policy timer due now. */
void orb_display_set_power_timer(lv_timer_t *timer);
/* Call with LVGL ownership. While eligible and dark, only pending physical
 * input and poll run, on notifications from the shared main-service slot.
 * No LVGL animation/refresh timer or independent UI deadline is serviced. */
void orb_display_set_sleep_poll(bool eligible, void (*poll)(void));
/* Safe off the UI thread; touch uses the same eligibility to wait for IRQ. */
bool orb_display_sleep_poll(void);

/*
 * Chooses how many rows the next band gets, starting at row y, of an area
 * ending at y_last, at most max_rows. Used to keep anything whose drawing
 * reads what's around it (backdrop blur) whole in one band.
 */
typedef int32_t (*orb_display_band_guard_t)(int32_t y, int32_t y_last, int32_t max_rows);
void orb_display_set_band_guard(orb_display_band_guard_t guard);

typedef struct {
    float fps;              /* frames rendered per second */
    float frame_ms;         /* rendering and sending one frame */
    float draw_ms;          /* of which, LVGL drawing bands */
    float wait_ms;          /* of which, LVGL waiting for the wire to take a band */
    float hold_ms;          /* the sender holding bands clear of the panel's scan */
    float send_ms;          /* bands on the wire */
    float bands_per_frame;
    float kb_per_frame;
    uint32_t torn;          /* bands whose write met the scan anyway */
} orb_display_stats_t;

/* Averages since the last call. */
void orb_display_stats(orb_display_stats_t *out);

#if LV_USE_PROFILER_BUILTIN
void orb_display_profile(int ms);
#endif

/* Monotonic counters: independent consumers can compare snapshots without
 * stealing/resetting each other's samples. Frames complete at final DMA. */
#define ORB_DISPLAY_LATENCY_BUCKETS 64
#define ORB_DISPLAY_LATENCY_BUCKET_US 4000
#define ORB_DISPLAY_INTERVAL_BUCKETS 128

typedef struct {
    int64_t at_us;
    uint64_t frames, frame_us, draw_us, wait_us, hold_us, send_us;
    uint64_t bands, bytes, torn;
    uint64_t copy_us, queue_wait_us, pipeline_bands, pipeline_dropped;
    uint64_t transfer_errors, failed_frames;
    int32_t last_transfer_error; /* Most recent failed band; ESP_OK until the first failure. */
    /* Bucket i: [i*4 ms, (i+1)*4 ms); final bucket includes all >=252 ms.
     * Successful complete frames only. Compare monotonic snapshots for p95. */
    uint32_t frame_latency[ORB_DISPLAY_LATENCY_BUCKETS];
    /* Final successful DMA intervals in 1 ms bins; last bin includes >=127 ms.
     * Independent of diagnostic config. A snapshot delta includes the interval
     * crossing its start boundary; failed frames break the pair. */
    uint32_t completion_interval[ORB_DISPLAY_INTERVAL_BUCKETS];
    uint64_t paced_scheduled, paced_on_slot, paced_late, paced_skipped;
    uint32_t paced_period_us, paced_divisor;
    uint32_t paced_lead_us, paced_capacity_us;
    uint64_t half_frames, full_frames;
    /* SPI ISR endpoints separate true pixel transfer from queue/cleanup wall
     * time. Ownership still ends only after the driver result-drain fence. */
    uint64_t dma_wire_us, dma_queue_us, dma_cleanup_us;
    uint64_t dma_timed_bands, dma_timed_frames;
} orb_display_counters_t;
void orb_display_counters(orb_display_counters_t *out);

/* True only while the opt-in PSRAM copy/DMA pipeline remains healthy. */
bool orb_display_pipeline_active(void);

/* Stage 0: non-resetting counters; disabled builds return zeros. Cold scene
 * construction belongs to the update budget even when done before refresh. */
void orb_display_accounting(orb_display_accounting_t *out);
bool orb_display_accounting_active(void);
void orb_display_account_cold_prepare(uint64_t elapsed_us);

/* UI-thread only: finish queued bands before a quiet-fixture sample starts. */
bool orb_display_drain(void);

/* UI-thread only, outside a display refresh: drain DMA and borrow its existing
 * internal arena for cold snapshot bands, then copy completed RGB565 rows to
 * the caller's preallocated PSRAM destination. No panel writes or new arena. */
bool orb_display_capture_snapshot(lv_obj_t *object, lv_draw_buf_t *destination);

/* UI-only benchmark seams. Drain before navigation, then begin in the first
 * scaffold RENDER_START callback; feedback is its certified changed region. A NULL
 * region reports an explicitly non-causal first-band proxy. */
uint32_t orb_display_response_begin(int64_t input_us, const lv_area_t *feedback);
void orb_display_response_settled(uint32_t token);
void orb_display_response_snapshot(orb_display_response_t *out);
void orb_display_response_end(uint32_t token);
/* Last live TE period, or zero when no current awake-panel sample exists. */
uint32_t orb_display_panel_period_us(void);
/* Scheduling is enabled and the awake panel has a valid TE sample. Actual
 * deadline success is measured by the paced_* counter deltas above. */
bool orb_display_te_pacing_active(void);
/* UI-thread only. Producers may coalesce pose publication while damage is
 * waiting for TE admission, then sample the newest pose at REFR_START. */
bool orb_display_refresh_pending(void);
