/*
 * LVGL renders in bands in internal RAM and each band goes straight to the
 * panel by DMA. The opt-in pipeline copies finished bands to two PSRAM
 * snapshots so rendering can overlap sending without a second internal buffer.
 * Adaptive buffering splits the same arena into two halves for small live
 * refreshes and immutable snapshot transitions; other larger redraws keep a
 * full band so backdrop blur stays whole.
 * Internal RAM takes a full-screen fill in 0.17 ms where PSRAM
 * takes 8.4 ms, so nothing is drawn in PSRAM.
 *
 * Tearing: the panel scans its memory top to bottom every 16.8 ms while bands
 * arrive at their own pace. Each band is held until its write won't cross the
 * scan. The scan model (te_hold here, the crossing test in orb_display_geom.c)
 * follows Muse Gadgets' muse_lcd_bands.c, Copyright (c) Meta Platforms, Inc.
 * and affiliates, Apache License 2.0.
 */
#include "orb_display.h"
#include "orb_display_accounting.h"
#include "orb_mem.h"

#include <string.h>
#include <limits.h>
#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_pm.h"
#include "esp_rom_sys.h"
#include "esp_private/esp_clk.h"
#include "misc/lv_timer_private.h"
#include "display/lv_display_private.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "orb_board.h"
#include "orb_display_geom.h"
#include "orb_display_pipeline.h"
#include "orb_display_pacing.h"
#include "orb_display_snapshot.h"
#include "orb_display_touch.h"
#include "orb_display_power.h"
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
#include "orb_display_buffers.h"
#endif

static const char *TAG = "display";

#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
static orb_display_scroll_trace_event_t *s_scroll_trace_events;
static orb_display_scroll_trace_t s_scroll_trace;
static int64_t s_scroll_trace_previous_completion;
static portMUX_TYPE s_scroll_trace_lock = portMUX_INITIALIZER_UNLOCKED;
#define SCROLL_TRACE(kind, at, a, b) orb_display_scroll_trace_record(kind, at, a, b)
#else
#define SCROLL_TRACE(kind, at, a, b) ((void)0)
#endif

bool orb_display_scroll_trace_begin(void)
{
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
    /* Bench setup owns LVGL; capture writers never allocate. Retain this
     * debug-only PSRAM arena for subsequent surface captures. */
    if (!s_scroll_trace_events) {
        s_scroll_trace_events = heap_caps_calloc(ORB_SCROLL_TRACE_CAPACITY, sizeof(*s_scroll_trace_events),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    portENTER_CRITICAL(&s_scroll_trace_lock);
    memset(&s_scroll_trace, 0, sizeof(s_scroll_trace));
    s_scroll_trace.available = s_scroll_trace_events != NULL;
    s_scroll_trace.active = s_scroll_trace.available;
    s_scroll_trace.started_us = esp_timer_get_time();
    s_scroll_trace_previous_completion = 0;
    portEXIT_CRITICAL(&s_scroll_trace_lock);
    return s_scroll_trace.available;
#else
    return false;
#endif
}

void orb_display_scroll_trace_end(void)
{
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
    portENTER_CRITICAL(&s_scroll_trace_lock);
    if (s_scroll_trace.active) {
        s_scroll_trace.active = false;
        s_scroll_trace.ended_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_scroll_trace_lock);
#endif
}

void orb_display_scroll_trace_record(orb_display_scroll_trace_kind_t kind, int64_t at_us,
                                     int32_t a, int32_t b)
{
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
    portENTER_CRITICAL(&s_scroll_trace_lock);
    if (s_scroll_trace.active && at_us >= s_scroll_trace.started_us) {
        if (s_scroll_trace.count < ORB_SCROLL_TRACE_CAPACITY) {
            s_scroll_trace_events[s_scroll_trace.count++] = (orb_display_scroll_trace_event_t) {
                .at_us = at_us, .a = a, .b = b, .kind = kind,
            };
        } else s_scroll_trace.dropped++;
        if (kind == ORB_SCROLL_TRACE_COMPLETE) {
            if (s_scroll_trace_previous_completion && at_us >= s_scroll_trace_previous_completion) {
                uint64_t interval = (uint64_t)(at_us - s_scroll_trace_previous_completion) / 1000;
                if (interval >= ORB_SCROLL_TRACE_INTERVAL_BUCKETS) interval = ORB_SCROLL_TRACE_INTERVAL_BUCKETS - 1;
                s_scroll_trace.intervals[interval]++;
            }
            s_scroll_trace_previous_completion = at_us;
            s_scroll_trace.completions++;
        } else if (kind == ORB_SCROLL_TRACE_LOOP_END) {
            s_scroll_trace.loops++;
            if (a > 0) s_scroll_trace.handler_us += a;
        } else if (kind == ORB_SCROLL_TRACE_WAIT_END && a > 0) {
            if (b == 1) s_scroll_trace.timer_wait_us += a;
            else if (b == 2) s_scroll_trace.fairness_wait_us += a;
        }
    }
    portEXIT_CRITICAL(&s_scroll_trace_lock);
#else
    (void)kind; (void)at_us; (void)a; (void)b;
#endif
}

void orb_display_scroll_trace_snapshot(orb_display_scroll_trace_t *out)
{
    memset(out, 0, sizeof(*out));
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
    portENTER_CRITICAL(&s_scroll_trace_lock);
    *out = s_scroll_trace;
    portEXIT_CRITICAL(&s_scroll_trace_lock);
#endif
}

bool orb_display_scroll_trace_read(unsigned index, orb_display_scroll_trace_event_t *out)
{
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
    portENTER_CRITICAL(&s_scroll_trace_lock);
    bool valid = index < s_scroll_trace.count;
    if (valid) *out = s_scroll_trace_events[index];
    portEXIT_CRITICAL(&s_scroll_trace_lock);
    return valid;
#else
    (void)index; (void)out;
    return false;
#endif
}

void orb_display_cpu_snapshot(orb_display_cpu_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->at_us = esp_timer_get_time();
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY && INCLUDE_xTaskGetIdleTaskHandle
    /* Idle TCBs live for the scheduler's lifetime. Copy only numeric fields;
     * sampling must neither allocate a task list nor dereference retired tasks.
     * vTaskGetInfo(..., pdFALSE, ...) avoids the stack high-water scan. */
    TaskStatus_t idle;
    out->cores = portNUM_PROCESSORS;
    if (out->cores > ORB_DISPLAY_CPU_CORES) return;
    for (unsigned core = 0; core < out->cores; core++) {
        vTaskGetInfo(xTaskGetIdleTaskHandleForCore(core), &idle, pdFALSE, eReady);
        out->idle[core] = idle.ulRunTimeCounter;
    }
    out->runtime = portGET_RUN_TIME_COUNTER_VALUE();
    out->counter_bits = sizeof(configRUN_TIME_COUNTER_TYPE) * 8;
    out->valid = true;
#endif
}

#define UI_CORE 1
#define SENDER_CORE 0
#define UI_PRIORITY 5

static orb_display_cadence_t s_cadence;
static orb_display_phase_clock_t s_phase_clock;
static bool s_cadence_moving;
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
static configRUN_TIME_COUNTER_TYPE s_cadence_ui_runtime, s_cadence_runtime;
#endif
static int64_t s_cadence_refresh_started, s_cadence_animation_at;

/* Diagnostic only: exact rectangle union, not the sum of overlapping requests
 * nor the circle-clipped wire area. Fixed scratch keeps it out of the heap. */
static void cadence_damage(const lv_display_t *disp)
{
    if (!s_cadence.active) return;
    int32_t edges[LV_INV_BUF_SIZE * 2];
    unsigned count = 0;
    for (unsigned i = 0; i < disp->inv_p; i++) {
        if (disp->inv_area_joined[i]) continue;
        edges[count++] = disp->inv_areas[i].x1;
        edges[count++] = disp->inv_areas[i].x2 + 1;
    }
    for (unsigned i = 1; i < count; i++) {
        int32_t edge = edges[i];
        unsigned j = i;
        while (j && edges[j - 1] > edge) { edges[j] = edges[j - 1]; j--; }
        edges[j] = edge;
    }
    uint64_t pixels = 0;
    for (unsigned x = 1; x < count; x++) {
        int32_t width = edges[x] - edges[x - 1];
        if (!width) continue;
        lv_area_t spans[LV_INV_BUF_SIZE];
        unsigned n = 0;
        for (unsigned i = 0; i < disp->inv_p; i++) {
            const lv_area_t *a = &disp->inv_areas[i];
            if (disp->inv_area_joined[i] || a->x1 > edges[x - 1] || a->x2 < edges[x - 1]) continue;
            unsigned j = n++;
            while (j && spans[j - 1].y1 > a->y1) { spans[j] = spans[j - 1]; j--; }
            spans[j] = *a;
        }
        int32_t end = -1;
        for (unsigned i = 0; i < n; i++) {
            int32_t start = LV_MAX(end + 1, spans[i].y1);
            if (spans[i].y2 >= start) pixels += (uint64_t)width * (spans[i].y2 - start + 1);
            end = LV_MAX(end, spans[i].y2);
        }
    }
    if (!pixels) return;
    s_cadence.damage_frames++;
    s_cadence.damage_pixels += pixels;
    if (pixels > s_cadence.damage_max_pixels) s_cadence.damage_max_pixels = pixels;
}

void orb_display_cadence_begin(void)
{
    memset(&s_cadence, 0, sizeof(s_cadence));
    memset(&s_phase_clock, 0, sizeof(s_phase_clock));
    s_cadence.active = true;
    s_cadence_moving = false;
    s_cadence.ui_priority = UI_PRIORITY;
    s_cadence.ui_core = UI_CORE;
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
    TaskStatus_t task;
    vTaskGetInfo(xTaskGetCurrentTaskHandle(), &task, pdFALSE, eReady);
    s_cadence_ui_runtime = task.ulRunTimeCounter;
    s_cadence_runtime = portGET_RUN_TIME_COUNTER_VALUE();
    s_cadence.ui_cpu_available = true;
#endif
    s_cadence.indev_period_ms = LV_DEF_REFR_PERIOD;
    s_cadence_refresh_started = s_cadence_animation_at = 0;
}

void orb_display_cadence_end(void)
{
    if (!s_cadence.active) return;
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
    TaskStatus_t task;
    vTaskGetInfo(xTaskGetCurrentTaskHandle(), &task, pdFALSE, eReady);
    configRUN_TIME_COUNTER_TYPE runtime = portGET_RUN_TIME_COUNTER_VALUE() - s_cadence_runtime;
    configRUN_TIME_COUNTER_TYPE used = task.ulRunTimeCounter - s_cadence_ui_runtime;
    s_cadence.ui_cpu_available = runtime && used <= runtime;
    s_cadence.ui_cpu_pct = s_cadence.ui_cpu_available ? 100.0f * used / runtime : 0;
#endif
    s_cadence.active = false;
    s_phase_clock.active = false;
}
void orb_display_cadence_set_moving(bool moving) { s_cadence_moving = moving; }

void orb_display_cadence_snapshot(orb_display_cadence_t *out) { *out = s_cadence; }

static int64_t cadence_loop_begin(void)
{
    if (!s_cadence.active) return 0;
    unsigned mhz = esp_clk_cpu_freq() / 1000000;
    s_cadence.frequency_samples++;
    if (mhz < CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) s_cadence.frequency_below_max++;
    if (!s_cadence.frequency_min_mhz || mhz < s_cadence.frequency_min_mhz)
        s_cadence.frequency_min_mhz = mhz;
    if (mhz > s_cadence.frequency_max_mhz) s_cadence.frequency_max_mhz = mhz;
    if (s_cadence_moving) {
        s_cadence.moving_frequency_samples++;
        if (mhz < CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) s_cadence.moving_frequency_below_max++;
        if (!s_cadence.moving_frequency_min_mhz || mhz < s_cadence.moving_frequency_min_mhz)
            s_cadence.moving_frequency_min_mhz = mhz;
        if (mhz > s_cadence.moving_frequency_max_mhz) s_cadence.moving_frequency_max_mhz = mhz;
    }
    return esp_timer_get_time();
}

static uint32_t cadence_animation_before(void)
{
    if (!s_cadence.active) return 0;
    lv_timer_t *animation = lv_anim_get_timer();
    if (!animation) return 0;
    if (animation->paused) s_cadence_animation_at = 0;
    s_cadence.animation_period_ms = animation->period;
    uint32_t age = lv_tick_elaps(animation->last_run);
    uint32_t late = !animation->paused && age > animation->period ? age - animation->period : 0;
    if (late > s_cadence.animation_late_max_ms) s_cadence.animation_late_max_ms = late;
    return animation->last_run;
}

static void cadence_loop_handled(int64_t began, int64_t locked, uint32_t animation_before, uint64_t refresh_before)
{
    if (!began || !s_cadence.active) return;
    int64_t now = esp_timer_get_time();
    uint64_t elapsed = now - locked;
    s_cadence.loops++;
    s_cadence.lock_us += locked - began;
    s_cadence.handler_us += elapsed;
    uint64_t refreshed = s_cadence.refresh_us - refresh_before;
    if (refreshed < elapsed) s_cadence.handler_outside_refresh_us += elapsed - refreshed;
    if (elapsed > s_cadence.handler_max_us) s_cadence.handler_max_us = elapsed;
    if (elapsed > 33000) s_cadence.handler_stalls++;
    if (s_phase_clock.active) {
        orb_display_slow_handler_t sample = {
            .sequence = s_cadence.loops, .at_us = locked,
            .handler_us = elapsed, .moving = s_cadence_moving,
        };
        orb_display_phase_finish(&s_phase_clock, now, &sample);
        for (unsigned i = 0; i < ORB_DISPLAY_PHASES; i++) {
            s_cadence.phase_us[i] += sample.phase_us[i];
            if (sample.phase_us[i] > s_cadence.phase_max_us[i]) s_cadence.phase_max_us[i] = sample.phase_us[i];
        }
        orb_display_slow_insert(s_cadence.slow, &sample);
    }
    lv_timer_t *animation = lv_anim_get_timer();
    if (animation && animation->last_run != animation_before) {
        s_cadence.animation_ticks++;
        uint64_t gap = s_cadence_animation_at ? now - s_cadence_animation_at : 0;
        if (gap > s_cadence.animation_gap_max_us) s_cadence.animation_gap_max_us = gap;
        if (gap > 33000) s_cadence.animation_stalls++;
        s_cadence_animation_at = now;
    }
}

static void cadence_wait(int64_t began, uint32_t next)
{
    if (!began || !s_cadence.active) return;
    uint64_t elapsed = esp_timer_get_time() - began;
    uint32_t requested = next ? (next > 10 ? 10000u : next * 1000u) : 100u;
    s_cadence.wait_requested_us += requested;
    if (next) s_cadence.timer_wait_us += elapsed;
    else s_cadence.fairness_wait_us += elapsed;
    if (elapsed > requested) s_cadence.wait_overrun_us += elapsed - requested;
    if (elapsed > s_cadence.wait_max_us) s_cadence.wait_max_us = elapsed;
}
/* Fits a 44 pt glass control, swollen by a press, whose blur must stay in one
 * band; any more costs internal RAM that networking needs. */
#if CONFIG_ORB_DISPLAY_TALL_BANDS
#define BAND_ROWS 104
#else
#define BAND_ROWS 84
#endif
#define BAND_BYTES (ORB_LCD_W * BAND_ROWS * 2)
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
#define HALF_BAND_BYTES (BAND_BYTES / 2u)
#define HALF_BAND_STRIDE ((HALF_BAND_BYTES + 63u) & ~63u)
#define DRAW_ARENA_BYTES (2u * HALF_BAND_STRIDE)
#else
#define DRAW_ARENA_BYTES BAND_BYTES
#endif
/* The board already splits a window into bounded SPI transactions. Taller
 * render bands don't need a larger internal DMA-descriptor allocation. */
#define TRANSFER_BYTES (ORB_LCD_W * 84 * 2)

typedef struct {
    const uint8_t *px;   /* NULL: a brightness change instead */
    lv_area_t area;
    int brightness;
    int panel_request;  /* 0: ordinary job; +/-1: awake/asleep request. */
    bool command_fence;
    bool last;
    bool power_owned;    /* Sender releases this job's CPU-frequency reference. */
    int64_t frame_start;
    uint32_t response_token;
    bool response_settled;
    int slot;            /* -1: an internal LVGL draw buffer */
#if CONFIG_ORB_DISPLAY_TE_PACING
    int64_t deadline_us, gate_start_us;
    uint8_t pace_mode;
    uint32_t ui_work_us;
    bool overlapped;
#endif
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    int64_t update_start, draw_start, queued;
#endif
} job_t;

static lv_display_t *s_disp;
static QueueHandle_t s_jobs;
static SemaphoreHandle_t s_flush_done, s_drain_done;
static SemaphoreHandle_t s_panel_done;
static TaskHandle_t s_ui_task;
static lv_timer_t *s_power_timer;
static atomic_bool s_external_wake, s_sleep_poll, s_service_wake;
static void (*s_sleep_poll_callback)(void);
static bool s_panel_on = true, s_panel_result;
static orb_display_band_guard_t s_guard;
static portMUX_TYPE s_pipeline_lock = portMUX_INITIALIZER_UNLOCKED;
static orb_pipeline_t s_pipeline;
static uint8_t *s_snapshots[ORB_PIPELINE_SLOTS];
static QueueHandle_t s_free_slots;
/* Only the UI accesses the buffer and pending-flush state. The request flag is
 * shared with main under s_pipeline_lock, independently of LVGL's lock. */
static void *s_draw_buf;
static bool s_flush_pending;
static bool s_snapshot_transition;
static bool s_independent_bands;
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
#define DOUBLE_BUFFER_RESERVE (24u * 1024u)
#define DOUBLE_BUFFER_DMA_BLOCK (8u * 1024u)
static bool s_double_buffer_requested, s_double_buffer_attempted, s_double_buffer_admitted, s_double_buffered;
static uint32_t s_half_frames, s_full_frames;
#endif

/* The panel's scan, from its tearing-effect line. */
static volatile int64_t s_te_rise, s_te_fall, s_te_period, s_te_blank;
/* PSRAM pixels run at 40 MHz, internal pixels at 80 MHz. Keep their learned
 * rates separate so failure fallback does not inherit the slower estimate. */
static float s_fixed_us = 60, s_ns_per_byte[2] = { 31.5f, 50.0f };

/* Half the width of the panel's visible circle on each row, in columns either side of the centre. */
static int16_t s_half_chord[ORB_LCD_H];

static orb_display_counters_t s_st;
static int64_t s_interval_completion;
static int64_t s_last_dma_done_us;
static bool s_last_dma_timed;
static int64_t s_frame_start, s_draw_start, s_wait_start;
static portMUX_TYPE s_st_lock = portMUX_INITIALIZER_UNLOCKED;
static orb_display_response_t s_response;
static uint32_t s_response_sequence;
static uint32_t s_frame_response_token;
static bool s_frame_response_settled;
#if CONFIG_ORB_DISPLAY_TE_PACING
static orb_pacing_t s_pacing;
static int64_t s_gate_deadline, s_gate_start, s_frame_deadline, s_frame_gate_start;
static uint8_t s_gate_mode, s_frame_pace_mode;
static bool s_gate_pending;
static bool s_pace_frame_started, s_pace_final_queued;
static uint32_t s_frame_pace_wait, s_last_sender_work, s_frame_admission_work;
static uint64_t s_frame_draw_baseline;
#endif
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
static orb_display_accounting_t *s_accounting;
static int64_t s_update_start, s_refresh_start, s_iteration_start, s_previous_completion;
static bool s_update_final_queued;
#endif

/* Draw tasks already keep the CPU fast while running. A reference per actual
 * flush job extends that protection across TE/DMA waits without relying on a
 * last-flush event: circular clipping can leave a render with no final flush. */
#if CONFIG_ORB_DYNAMIC_FREQUENCY_SCALING
static esp_pm_lock_handle_t s_band_power;
static portMUX_TYPE s_band_power_lock = portMUX_INITIALIZER_UNLOCKED;

static void band_power_init(void)
{
    esp_err_t err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "display_band", &s_band_power);
    if (err != ESP_OK) ESP_LOGW(TAG, "display CPU lock unavailable: %s", esp_err_to_name(err));
}

static bool band_power_acquire(void)
{
    if (!s_band_power) return false;
    /* Serialize calls for this handle as required by the public PM API. Its
     * acquire/release operations are ISR-safe; neither waits on a task. */
    portENTER_CRITICAL(&s_band_power_lock);
    esp_err_t err = esp_pm_lock_acquire(s_band_power);
    portEXIT_CRITICAL(&s_band_power_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "display CPU lock acquire: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static void band_power_release(bool owned)
{
    if (!owned) return;
    portENTER_CRITICAL(&s_band_power_lock);
    esp_err_t err = esp_pm_lock_release(s_band_power);
    portEXIT_CRITICAL(&s_band_power_lock);
    if (err != ESP_OK) ESP_LOGE(TAG, "display CPU lock release: %s", esp_err_to_name(err));
}
#else
static void band_power_init(void) { }
static bool band_power_acquire(void) { return false; }
static void band_power_release(bool owned) { (void)owned; }
#endif

static void IRAM_ATTR on_te(void *arg)
{
    int64_t t = esp_timer_get_time();
    if (gpio_get_level(ORB_LCD_TE_GPIO)) {
        if (s_te_rise) {
            s_te_period = t - s_te_rise;
        }
        s_te_rise = t;
    } else {
        s_te_blank = t - s_te_rise;
        s_te_fall = t;   /* the scan starts */
    }
}

/* The scan timing, read whole (the interrupt writes each value as two words). */
static bool te_now(int64_t *fall, int64_t *p, int64_t *scan)
{
    int64_t f, q, blank;
    do {
        f = s_te_fall;
        q = s_te_period;
        blank = s_te_blank;
    } while (f != s_te_fall || q != s_te_period || blank != s_te_blank);
    *fall = f;
    *p = q;
    *scan = q - blank;
    /* The first edge after sleep spans the entire dark interval. Reject it
     * until a fresh period exists; no waiter may extrapolate silent TE. */
    return orb_display_te_fresh(esp_timer_get_time(), f, q, blank);
}

static bool te_meets(int64_t t0, int64_t w, int y1, int y2, int64_t fall, int64_t p, int64_t scan)
{
    return orb_geom_te_meets(t0, w, y1, y2, fall, p, scan, ORB_LCD_H);
}

/* The sender outranks the UI. A sub-tick vTaskDelay(0) keeps it runnable
 * and prevents the UI from drawing the other buffer during a scan/deadline
 * hold. Block on a private microsecond timer so DMA overlap remains real. */
static esp_timer_handle_t s_sender_wake;

static void sender_wake(void *arg)
{
    xTaskNotifyGive((TaskHandle_t)arg);
}

static void sender_wait_until(int64_t until)
{
    int64_t remaining;
    while ((remaining = until - esp_timer_get_time()) > 0) {
        if (remaining <= 200) {
            esp_rom_delay_us((uint32_t)remaining);
        } else if (s_sender_wake && esp_timer_start_once(s_sender_wake, remaining) == ESP_OK) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
            vTaskDelay(ticks ? ticks : 1);
        }
    }
}

/* Holds until a write of rows [y1, y2) taking about w would stay clear of the
 * scan. A write that meets the scan wherever it starts goes now. */
static void te_hold(int y1, int y2, int64_t w)
{
    for (int tries = 0; tries < 3; tries++) {
        int64_t fall, p, scan, now = esp_timer_get_time();
        if (!te_now(&fall, &p, &scan)) {
            return;
        }
        /* Measured CASET/RASET/queue setup precedes the first pixel ISR by
         * about 110 us per band. Varying only wire duration at `now` missed
         * narrow crossing intervals after that start delay. Prove the entire
         * start/duration/TE-phase window safe, including its interior. The
         * shortest tiny transfers need a genuine lower bound, not +100 us. */
        int64_t lo = w > 100 ? (w - 100) / 2 : 1;
        int64_t hi = w * 3 / 2 + 100;
        int64_t d = orb_geom_te_delay_window(now, 500, lo, hi, y1, y2,
                                            fall, p, scan, 150, ORB_LCD_H);
        if (d <= 0 || d >= 2 * p) return;
        sender_wait_until(now + d);
    }
}

static bool send_band(const job_t *j)
{
    const lv_area_t *a = &j->area;
    size_t bytes = (size_t)lv_area_get_size(a) * 2;
    bool external = j->slot >= 0;
    /* Switching from command80 to pixel40 and back costs about 30 us each. */
    int64_t w = (int64_t)(s_fixed_us + (external ? 60 : 0) + bytes * s_ns_per_byte[external] / 1000);
    int64_t t0 = esp_timer_get_time();
#if CONFIG_ORB_DISPLAY_TE_PACING
    /* Earlier bands still stream immediately. Delay only the last band and
     * retain its buffer/PM lease until real SPI drain. Scan avoidance below
     * remains authoritative; a conflicting/slow write is reported as late. */
    if (j->last && j->deadline_us) {
        int64_t until = j->deadline_us - w;
        sender_wait_until(until);
        s_frame_pace_wait += (uint32_t)(esp_timer_get_time() - t0);
    }
#endif
    int64_t scan_wait_start = esp_timer_get_time();
    te_hold(a->y1, a->y2 + 1, w);
    int64_t t1 = esp_timer_get_time();
#if CONFIG_ORB_DISPLAY_TE_PACING
    /* Capacity is draw+transport work, not waiting for the previous panel scan.
     * Including this phase-dependent wait feeds an early cold scan conflict
     * back into an ever larger budget/divisor. Keep real HOLD, lateness and
     * completion intervals unchanged; only the admission estimate excludes it. */
    s_frame_pace_wait += (uint32_t)(t1 - scan_wait_start);
#else
    (void)scan_wait_start;
#endif
    esp_err_t err = orb_board_panel_write(a->x1, a->y1, a->x2, a->y2, j->px, bytes);
    /* The ISR callback precedes result cleanup. This fence reclaims every
     * descriptor and reports underflow even when queueing failed halfway. */
    esp_err_t submitted = err;
    esp_err_t finished = orb_board_panel_finish();
    if (err == ESP_OK) err = finished;
    int64_t t2 = esp_timer_get_time();
    bool ok = err == ESP_OK;
    int64_t dma_start = t1, dma_done = t2;
    bool timed = ok && orb_board_panel_timing(&dma_start, &dma_done) &&
        dma_start >= t1 && dma_done >= dma_start && dma_done <= t2;
    s_last_dma_done_us = timed ? dma_done : t2;
    s_last_dma_timed = timed;
#if CONFIG_ORB_DISPLAY_TE_PACING
    s_last_sender_work = (uint32_t)(t2 - t1);
#endif
    if (ok && bytes > 32 * 1024) {
        float rate = (float)(timed ? dma_done - dma_start : t2 - t1) * 1000 / bytes;
        s_ns_per_byte[external] += (rate - s_ns_per_byte[external]) / 16;
    }
    int64_t fall, p, scan;
    bool torn = false;
    if (ok && te_now(&fall, &p, &scan)) {
        int64_t began = timed ? dma_start : t1;
        /* Cleanup may be delayed for several scans after the real SPI end.
         * Anchor the retrospective model near that transfer, not near now. */
        int64_t cycles = (began - fall) / p;
        if (began < fall && (began - fall) % p) cycles--;
        fall += cycles * p;
        torn = te_meets(began, timed ? dma_done - dma_start : t2 - t1,
                        a->y1, a->y2 + 1, fall, p, scan);
    }
    portENTER_CRITICAL(&s_st_lock);
    s_st.hold_us += t1 - t0;
    s_st.send_us += t2 - t1;
    if (timed) {
        s_st.dma_timed_bands++;
        s_st.dma_wire_us += dma_done - dma_start;
        s_st.dma_queue_us += dma_start - t1;
        s_st.dma_cleanup_us += t2 - dma_done;
    }
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    if (s_accounting) orb_accounting_band(s_accounting, (orb_display_band_trace_t){
        .draw_start_us = j->draw_start, .queued_us = j->queued, .sender_us = t0,
        .dma_start_us = timed ? dma_start : t1, .dma_end_us = timed ? dma_done : t2, .bytes = bytes,
        .y1 = a->y1, .y2 = a->y2, .ok = ok,
    });
#endif
    if (ok) {
        s_st.bytes += bytes;
        s_st.bands++;
        s_st.torn += torn;
    } else {
        s_st.transfer_errors++;
        s_st.last_transfer_error = err;
    }
    portEXIT_CRITICAL(&s_st_lock);
    if (!ok) ESP_LOGE(TAG, "panel transfer failed: %s (queue=%s finish=%s source=%s bytes=%u)",
                     esp_err_to_name(err), esp_err_to_name(submitted), esp_err_to_name(finished),
                     j->slot >= 0 ? "PSRAM" : "internal", (unsigned)bytes);
    return ok;
}

static void send_task(void *arg)
{
    const esp_timer_create_args_t wake_args = {
        .callback = sender_wake, .arg = xTaskGetCurrentTaskHandle(), .name = "lcd_deadline",
    };
    if (esp_timer_create(&wake_args, &s_sender_wake) != ESP_OK)
        ESP_LOGW(TAG, "precise sender wait unavailable; using RTOS ticks");
    job_t j;
    orb_frame_result_t frame = { 0 };
#if CONFIG_ORB_DISPLAY_TE_PACING
    int64_t pace_frame_start = 0;
    uint32_t sender_work = 0;
#endif
    for (;;) {
        bool ready = orb_board_panel_ready(true);
        orb_board_panel_service();
        if (!ready && orb_board_panel_ready(true)) orb_display_wake();
        uint32_t wait_ms = orb_board_panel_wait_ms();
        TickType_t wait = wait_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
        if (xQueueReceive(s_jobs, &j, wait ? wait : 1) != pdTRUE) {
            orb_board_panel_service();
            orb_display_wake();
            continue;
        }
        if (!j.px) {
            if (j.panel_request) {
                s_panel_result = orb_board_panel_awake(j.panel_request > 0) == ESP_OK;
                xSemaphoreGive(s_panel_done);
            } else if (j.brightness >= 0) {
                s_panel_result = orb_board_panel_set_brightness(j.brightness) == ESP_OK;
            } else xSemaphoreGive(s_drain_done);
            if (j.command_fence) xSemaphoreGive(s_panel_done);
            continue;
        }
#if CONFIG_ORB_DISPLAY_TE_PACING
        if (pace_frame_start != j.frame_start) {
            s_frame_pace_wait = 0;
            sender_work = 0;
            pace_frame_start = j.frame_start;
        }
#endif
        bool transmit = true;
        if (j.slot >= 0) {
            portENTER_CRITICAL(&s_pipeline_lock);
            bool owned = orb_pipeline_begin(&s_pipeline, (unsigned)j.slot);
            transmit = s_pipeline.enabled;
            portEXIT_CRITICAL(&s_pipeline_lock);
            assert(owned);
        }
        bool ok = transmit && send_band(&j);
#if CONFIG_ORB_DISPLAY_TE_PACING
        if (transmit) sender_work += s_last_sender_work;
#endif
        /* Count successful presentation at the final pixel interrupt, after
         * the result fence proves success. Task wakeup/descriptor cleanup is
         * not when pixels reached the panel and can cross a TE boundary. */
        int64_t completed = ok ? s_last_dma_done_us : esp_timer_get_time();
        bool frame_ok = orb_frame_complete(&frame, j.frame_start, ok, j.last);
        if (frame_ok) SCROLL_TRACE(ORB_SCROLL_TRACE_COMPLETE, completed, 0, 0);
        portENTER_CRITICAL(&s_st_lock);
        if (!transmit) s_st.pipeline_dropped++;
        if (frame_ok) {
            uint64_t elapsed = completed - j.frame_start;
            s_st.frames++;
            if (s_last_dma_timed) s_st.dma_timed_frames++;
            s_st.frame_us += elapsed;
            unsigned bucket = elapsed / ORB_DISPLAY_LATENCY_BUCKET_US;
            if (bucket >= ORB_DISPLAY_LATENCY_BUCKETS) bucket = ORB_DISPLAY_LATENCY_BUCKETS - 1;
            s_st.frame_latency[bucket]++;
            if (s_interval_completion) {
                uint64_t interval = (uint64_t)(completed - s_interval_completion) / 1000;
                if (interval >= ORB_DISPLAY_INTERVAL_BUCKETS) interval = ORB_DISPLAY_INTERVAL_BUCKETS - 1;
                s_st.completion_interval[interval]++;
            }
            s_interval_completion = completed;
        } else if (j.last) {
            s_st.failed_frames++;
            s_interval_completion = 0;
        }
#if CONFIG_ORB_DISPLAY_TE_PACING
        if (j.last) {
            orb_pacing_complete(&s_pacing, j.deadline_us, j.gate_start_us, completed,
                                s_frame_pace_wait, j.pace_mode, frame_ok && s_last_dma_timed,
                                j.overlapped ? (j.ui_work_us > sender_work ? j.ui_work_us : sender_work) : 0);
            s_frame_pace_wait = 0;
        }
#endif
        orb_response_complete(&s_response, j.response_token, j.response_settled,
                              j.area.x1, j.area.y1, j.area.x2, j.area.y2,
                              completed, ok, frame_ok);
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
        if (j.last && s_accounting) {
#if CONFIG_ORB_DISPLAY_TE_PACING
            /* Immediate feedback/forced capture is an explicit phase boundary,
             * not a T/2T interval. The first paced frame starts a fresh pair. */
            bool paced = j.deadline_us && j.pace_mode;
            if (!paced) s_previous_completion = 0;
#endif
            orb_accounting_complete(s_accounting, &s_previous_completion,
                                    j.update_start, completed, frame_ok);
#if CONFIG_ORB_DISPLAY_TE_PACING
            if (!paced) s_previous_completion = 0;
#endif
        }
#endif
        portEXIT_CRITICAL(&s_st_lock);
        portENTER_CRITICAL(&s_pipeline_lock);
        if (!ok) {
            if (j.slot >= 0) orb_pipeline_disable(&s_pipeline);
            else s_pipeline.redraw = true;
        }
        bool retired = j.slot < 0 || orb_pipeline_retire(&s_pipeline, (unsigned)j.slot);
        portEXIT_CRITICAL(&s_pipeline_lock);
        assert(retired);
        if (j.slot >= 0) {
            unsigned slot = (unsigned)j.slot;
            /* Only the sender returns a queued slot, after all SPI results
             * were drained (or the disabled pipeline discarded the job). */
            BaseType_t returned = xQueueSend(s_free_slots, &slot, 0);
            assert(returned == pdTRUE);
        } else xSemaphoreGive(s_flush_done);
        /* Wake the renderer before releasing transport protection, so a ready
         * UI task can retain the CPU through its normal RTOS PM lock. */
        band_power_release(j.power_owned);
    }
}

static bool snapshot_band(job_t *job)
{
    portENTER_CRITICAL(&s_pipeline_lock);
    bool enabled = s_pipeline.enabled;
    portEXIT_CRITICAL(&s_pipeline_lock);
    if (!enabled) return false;
    unsigned slot;
    int64_t waited = esp_timer_get_time();
    xQueueReceive(s_free_slots, &slot, portMAX_DELAY);
    portENTER_CRITICAL(&s_st_lock);
    s_st.queue_wait_us += esp_timer_get_time() - waited;
    portEXIT_CRITICAL(&s_st_lock);
    portENTER_CRITICAL(&s_pipeline_lock);
    bool claimed = orb_pipeline_claim(&s_pipeline, slot);
    portEXIT_CRITICAL(&s_pipeline_lock);
    if (!claimed) {
        xQueueSend(s_free_slots, &slot, 0);
        return false; /* An in-flight error disabled it while we waited. */
    }
    size_t bytes = (size_t)lv_area_get_size(&job->area) * 2;
    assert(bytes <= BAND_BYTES);
    int64_t started = esp_timer_get_time();
    memcpy(s_snapshots[slot], job->px, bytes);
    portENTER_CRITICAL(&s_st_lock);
    s_st.copy_us += esp_timer_get_time() - started;
    portEXIT_CRITICAL(&s_st_lock);
    portENTER_CRITICAL(&s_pipeline_lock);
    bool queued = orb_pipeline_queue(&s_pipeline, slot);
    if (!queued) {
        bool canceled = orb_pipeline_cancel_copy(&s_pipeline, slot);
        assert(canceled);
    }
    portEXIT_CRITICAL(&s_pipeline_lock);
    if (!queued) {
        xQueueSend(s_free_slots, &slot, 0);
        return false;
    }
    job->px = s_snapshots[slot];
    job->slot = (int)slot;
    return true;
}

/* The PSRAM pipeline releases LVGL's source after copy+queue. Internal sources
 * remain owned until SPI result drain, including when LVGL has two buffers. */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    job_t job = { .px = px_map, .area = *area, .slot = -1,
                  .last = lv_display_flush_is_last(disp), .frame_start = s_frame_start,
                  .response_token = s_frame_response_token,
                  .response_settled = s_frame_response_settled,
                  .power_owned = band_power_acquire() };
#if CONFIG_ORB_DISPLAY_TE_PACING
    job.deadline_us = s_frame_deadline;
    job.gate_start_us = s_frame_gate_start;
    job.pace_mode = s_frame_pace_mode;
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
    job.overlapped = s_double_buffered;
#endif
    if (job.last) {
        s_pace_final_queued = true;
        /* End-to-end latency contains overlapping producer/sender work. The
         * slower lane limits sustainable throughput, not their serial sum. */
        portENTER_CRITICAL(&s_st_lock);
        uint64_t drawing = s_st.draw_us - s_frame_draw_baseline;
        portEXIT_CRITICAL(&s_st_lock);
        uint64_t preparation = s_frame_gate_start > 0 && s_frame_start >= s_frame_gate_start ?
            (uint64_t)(s_frame_start - s_frame_gate_start) : 0;
        uint64_t work = preparation + drawing + s_frame_admission_work;
        job.ui_work_us = work > UINT32_MAX ? UINT32_MAX : (uint32_t)work;
    }
#endif
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    job.update_start = s_update_start;
    job.draw_start = s_draw_start;
    job.queued = esp_timer_get_time();
    if (job.last) s_update_final_queued = true;
#endif
    bool copied = snapshot_band(&job);
#if CONFIG_ORB_DISPLAY_TE_PACING
    job.overlapped |= copied;
#endif
    int64_t waited = esp_timer_get_time();
    s_flush_pending = true;
    xQueueSend(s_jobs, &job, portMAX_DELAY);
    portENTER_CRITICAL(&s_st_lock);
    s_st.queue_wait_us += esp_timer_get_time() - waited;
    if (copied) s_st.pipeline_bands++;
    portEXIT_CRITICAL(&s_st_lock);
    if (copied) xSemaphoreGive(s_flush_done);
}

static void flush_wait_cb(lv_display_t *disp)
{
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
    s_flush_pending = false;
}

/* Admission costs no late allocation: the early arena is only 48 bytes larger
 * at 84 rows. Keep the previous runtime/audio margins, but no longer demand a
 * second 78 KiB contiguous block after Wi-Fi has fragmented the heap. */
static void try_double_buffer(void)
{
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
    portENTER_CRITICAL(&s_pipeline_lock);
    bool requested = s_double_buffer_requested;
    portEXIT_CRITICAL(&s_pipeline_lock);
    if (!requested || s_double_buffer_attempted) return;
    s_double_buffer_attempted = true;
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = internal | MALLOC_CAP_DMA;
    size_t free_internal = heap_caps_get_free_size(internal);
    size_t free_dma = heap_caps_get_free_size(dma);
    size_t largest = heap_caps_get_largest_free_block(dma);
    if (free_internal < DOUBLE_BUFFER_RESERVE || free_dma < DOUBLE_BUFFER_RESERVE ||
        largest < DOUBLE_BUFFER_DMA_BLOCK) {
        ESP_LOGW(TAG, "adaptive internal buffers skipped: free internal/DMA %u/%u, largest DMA %u",
                 (unsigned)free_internal, (unsigned)free_dma, (unsigned)largest);
        return;
    }
    s_double_buffer_admitted = true;
    ESP_LOGI(TAG, "adaptive internal buffers admitted: 2 x %u bytes; full-band fallback %u; "
             "arena %u; free internal/DMA %u/%u, largest DMA %u",
             (unsigned)HALF_BAND_BYTES, (unsigned)BAND_BYTES, (unsigned)DRAW_ARENA_BYTES,
             (unsigned)free_internal, (unsigned)free_dma, (unsigned)largest);
#endif
}

#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
static void install_draw_buffers(bool use_halves)
{
    if (use_halves != s_double_buffered) {
        /* Both layouts alias the arena. Drain the previous refresh's DMA before
         * resetting buf_act, even if LVGL had already swapped to the other half. */
        int64_t wait_start = esp_timer_get_time();
        if (s_flush_pending) {
            flush_wait_cb(s_disp);
            lv_display_flush_ready(s_disp);
        }
        portENTER_CRITICAL(&s_st_lock);
        s_st.wait_us += esp_timer_get_time() - wait_start;
        portEXIT_CRITICAL(&s_st_lock);
        lv_display_set_buffers(s_disp, s_draw_buf,
                               use_halves ? (uint8_t *)s_draw_buf + HALF_BAND_STRIDE : NULL,
                               use_halves ? HALF_BAND_BYTES : BAND_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
        s_double_buffered = use_halves;
    }
}

/* RENDER_START is after invalidation joining/blur expansion, before LVGL reads
 * buf_act. Only complete live regions or a no-blur snapshot scene may use the
 * halves. Never slice live glass merely to admit overlap. */
static void select_draw_buffers(void)
{
#if CONFIG_ORB_DISPLAY_TE_PACING
    int64_t began = esp_timer_get_time();
#endif
    s_independent_bands = s_snapshot_transition;
#if CONFIG_ORB_DISPLAY_LIVE_DOUBLE_BUFFER
    if (!s_independent_bands) s_independent_bands = orb_display_live_bands_independent(s_disp);
#endif
    bool use_halves = s_double_buffer_admitted &&
        orb_display_half_buffers_fit_scene(s_disp, HALF_BAND_BYTES, s_independent_bands);
#if CONFIG_ORB_DISPLAY_TE_PACING
    /* This PSRAM-tree proof is producer service. Exclude only the later DMA
     * fence when switching layouts, which is a pipeline wait rather than CPU. */
    s_frame_admission_work = (uint32_t)(esp_timer_get_time() - began);
#endif
    install_draw_buffers(use_halves);
    portENTER_CRITICAL(&s_st_lock);
    if (use_halves) s_half_frames++;
    else s_full_frames++;
    portEXIT_CRITICAL(&s_st_lock);
}
#endif

static void pipeline_init(void)
{
#if CONFIG_ORB_DISPLAY_PSRAM_PIPELINE
    const size_t storage = (BAND_BYTES + 63u) & ~63u;
    s_free_slots = xQueueCreate(ORB_PIPELINE_SLOTS, sizeof(unsigned));
    for (unsigned i = 0; s_free_slots && i < ORB_PIPELINE_SLOTS; i++) {
        /* This IDF's PSRAM heap does not advertise MALLOC_CAP_DMA. DMA
         * accessibility is a separate address-range capability on ESP32-S3. */
        s_snapshots[i] = heap_caps_aligned_alloc(64, storage, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_snapshots[i] || !esp_ptr_dma_ext_capable(s_snapshots[i])) goto fallback;
    }
    if (!s_free_slots) goto fallback;
    for (unsigned i = 0; i < ORB_PIPELINE_SLOTS; i++) xQueueSend(s_free_slots, &i, 0);
    s_pipeline.enabled = true;
    ESP_LOGI(TAG, "experimental PSRAM pipeline: %u slots of %u bytes", ORB_PIPELINE_SLOTS, (unsigned)storage);
    return;
fallback:
    for (unsigned i = 0; i < ORB_PIPELINE_SLOTS; i++) {
        heap_caps_free(s_snapshots[i]);
        s_snapshots[i] = NULL;
    }
    if (s_free_slots) vQueueDelete(s_free_slots);
    s_free_slots = NULL;
    ESP_LOGW(TAG, "PSRAM pipeline unavailable; using blocking internal buffer");
#endif
}

/*
 * Bands cover only what the round panel shows: the corners of the square are
 * never drawn or sent. Narrower bands near the top and bottom also fit more
 * rows into the buffer.
 */
static void band_cb(lv_display_t *disp, lv_area_t *band, int32_t y_last, uint32_t buf_px)
{
    /* Frozen controls are pixels, not live blur: their old geometry must not
     * force a 72 px control into a 42-row half and violate its capacity. */
    orb_display_band_guard_t guard = s_independent_bands ? NULL : s_guard;
    orb_geom_band(s_half_chord, ORB_LCD_H, band->y1, y_last, band->x1, band->x2, buf_px, guard, &band->y2,
                  &band->x1, &band->x2);
}

/* The CO5300 takes only even-aligned windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
    if (s_cadence.active) {
        s_cadence.invalidations++;
        s_cadence.invalidated_pixels += lv_area_get_size(a);
    }
    SCROLL_TRACE(ORB_SCROLL_TRACE_INVALIDATE, esp_timer_get_time(), a->y1, a->y2);
}

static uint32_t refresh_gate(lv_display_t *disp)
{
    /* Retain all invalidation while dark, including animations below the calm
     * root. No LVGL draw or pixel job starts before SLPOUT has settled. */
    uint32_t power_delay = orb_display_power_refresh_delay(s_panel_on, orb_board_panel_ready(true));
    if (power_delay) return power_delay;
#if CONFIG_ORB_DISPLAY_TE_PACING
    /* Certified immutable compositor bands already sustain one panel period
     * with per-band scan avoidance. Do not apply live producer admission to
     * their pipelined tail latency (HIG §6: preserve continuous navigation). */
    if (s_snapshot_transition) {
        portENTER_CRITICAL(&s_st_lock);
        s_pacing.mode = 0;
        s_pacing.target_us = 0;
        s_pacing.probing = false;
        s_pacing.unpaced++;
        portEXIT_CRITICAL(&s_st_lock);
        s_gate_deadline = s_gate_start = 0;
        s_gate_mode = 0;
        s_gate_pending = true;
        return 0;
    }
    int64_t fall = 0, period = 0, scan = 0;
    bool valid = te_now(&fall, &period, &scan);
    int64_t now = esp_timer_get_time();
    bool moving = lv_draw_sw_blur_motion_active(disp);
    portENTER_CRITICAL(&s_st_lock);
    orb_pacing_plan_t plan = orb_pacing_plan(&s_pacing, now, fall,
                                            valid ? (uint32_t)period : 0, moving);
    portEXIT_CRITICAL(&s_st_lock);
    if (plan.delay_us > 2000) return plan.delay_us / 1000 - 1;
    if (plan.delay_us) {
        /* LVGL gates return milliseconds, but TE slots are microseconds. Round
         * down the coarse retry and finish the last <=2 ms here. Rounding up
         * each retry used the entire 1 ms admission tolerance before layout. */
        esp_rom_delay_us(plan.delay_us);
        now = esp_timer_get_time();
        valid = te_now(&fall, &period, &scan);
        portENTER_CRITICAL(&s_st_lock);
        plan = orb_pacing_plan(&s_pacing, now, fall, valid ? (uint32_t)period : 0, moving);
        portEXIT_CRITICAL(&s_st_lock);
        if (plan.delay_us) return 1;
    }
    s_gate_deadline = plan.deadline_us;
    s_gate_start = now;
    s_gate_mode = plan.mode;
    s_gate_pending = true;
    return 0;
#else
    (void)disp;
    return 0;
#endif
}

/* Single buffering waits before drawing; double buffering waits after drawing
 * but before flush. Account drawing at that wait's start in the latter case.
 * FLUSH_FINISH only means the driver queued it, never DMA completion. */
static void cadence_refresh_phase(lv_display_t *disp, lv_display_refresh_phase_t phase)
{
    (void)disp;
    _Static_assert(LV_DISPLAY_REFRESH_PHASE_COUNT == ORB_DISPLAY_PHASES, "refresh phase map");
    if (s_phase_clock.active) orb_display_phase_mark(&s_phase_clock, esp_timer_get_time(), phase);
}

static void cadence_timer(lv_timer_cb_t callback, bool entering)
{
    if (s_phase_clock.active)
        orb_display_phase_timer(&s_phase_clock, esp_timer_get_time(), (uintptr_t)callback, entering);
}

static void refresh_memory(bool started, int64_t now)
{
    /* Attribute the first online redraw separately from TLS/hello. Local heap
     * monitoring walks heap regions, so keep this diagnostic out of scrolling
     * and transition benchmarks after the 20-second startup window. */
    static bool active;
    if (started && !active && now < 20 * 1000000LL) {
        orb_mem_begin(ORB_MEM_UI_REFRESH);
        active = true;
    } else if (!started && active) {
        orb_mem_end(ORB_MEM_UI_REFRESH);
        active = false;
    }
}

static void on_render(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        refresh_memory(true, now);
        if (s_cadence.active) {
            s_cadence.refreshes++;
            s_cadence_refresh_started = now;
        }
        SCROLL_TRACE(ORB_SCROLL_TRACE_REFR_START, now, 0, 0);
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
        s_refresh_start = now;
        s_update_start = s_iteration_start ? s_iteration_start : now;
        s_update_final_queued = false;
#endif
        break;
    case LV_EVENT_REFR_READY:
        refresh_memory(false, now);
        if (s_cadence.active && s_cadence_refresh_started) {
            s_cadence.refresh_us += now - s_cadence_refresh_started;
            s_cadence_refresh_started = 0;
        }
        SCROLL_TRACE(ORB_SCROLL_TRACE_REFR_END, now, 0, 0);
#if CONFIG_ORB_DISPLAY_TE_PACING
        /* Layout/clipping can consume all damage without RENDER_START. A later
         * forced capture must never inherit that abandoned admission. */
        if (s_gate_pending || (s_pace_frame_started && !s_pace_final_queued)) {
            portENTER_CRITICAL(&s_st_lock);
            if (s_gate_pending ? s_gate_deadline : s_frame_deadline) s_pacing.late++;
            s_pacing.probing = false;
            s_pacing.mode = 0;
            s_pacing.target_us = 0;
            portEXIT_CRITICAL(&s_st_lock);
            s_gate_pending = false;
        }
        s_pace_frame_started = false;
#endif
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
        /* A refresh without a queued final band (e.g. circular clipping) is
         * not a successful presentation. Quiet refresh requests with no RENDER_START are free. */
        if (s_frame_start >= s_update_start && !s_update_final_queued) {
            portENTER_CRITICAL(&s_st_lock);
            if (s_accounting) s_accounting->dropped++;
            portEXIT_CRITICAL(&s_st_lock);
        }
#endif
        break;
    case LV_EVENT_RENDER_START:
        if (s_cadence.active) {
            s_cadence.render_starts++;
            cadence_damage(lv_event_get_current_target(e));
        }
        SCROLL_TRACE(ORB_SCROLL_TRACE_RENDER_START, now, 0, 0);
        s_frame_start = now;
#if CONFIG_ORB_DISPLAY_TE_PACING
        s_pace_frame_started = true;
        s_pace_final_queued = false;
        s_frame_admission_work = 0;
        s_frame_deadline = s_gate_pending ? s_gate_deadline : 0;
        s_frame_gate_start = s_gate_pending ? s_gate_start : 0;
        s_frame_pace_mode = s_gate_pending ? s_gate_mode : 0;
        s_gate_pending = false;
#endif
        portENTER_CRITICAL(&s_st_lock);
#if CONFIG_ORB_DISPLAY_TE_PACING
        s_frame_draw_baseline = s_st.draw_us;
#endif
        s_frame_response_token = s_response.token;
        s_frame_response_settled = s_response.settled;
        portEXIT_CRITICAL(&s_st_lock);
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
        portENTER_CRITICAL(&s_st_lock);
        if (s_accounting) {
            s_accounting->scheduled++;
            s_accounting->preparation_us += now - s_update_start;
            s_accounting->scene_prepare_us += s_refresh_start - s_update_start;
        }
        portEXIT_CRITICAL(&s_st_lock);
#endif
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
        select_draw_buffers();
        now = esp_timer_get_time();
#endif
        s_draw_start = now;
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        s_wait_start = now;
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
        if (s_double_buffered) {
            portENTER_CRITICAL(&s_st_lock);
            s_st.draw_us += now - s_draw_start;
            portEXIT_CRITICAL(&s_st_lock);
        }
#endif
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        portENTER_CRITICAL(&s_st_lock);
        s_st.wait_us += now - s_wait_start;
        portEXIT_CRITICAL(&s_st_lock);
        s_draw_start = now;
        break;
    case LV_EVENT_FLUSH_START:
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
        if (s_double_buffered) break;
#endif
        portENTER_CRITICAL(&s_st_lock);
        s_st.draw_us += now - s_draw_start;
        portEXIT_CRITICAL(&s_st_lock);
        break;
    case LV_EVENT_FLUSH_FINISH:
        s_draw_start = now;
        break;
    default:
        break;
    }
}



/* Raster caches are large and read in long runs once a frame: PSRAM, never the scarce internal RAM. */
static void *raster_malloc(size_t size, lv_color_format_t cf)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void raster_free(void *p)
{
    heap_caps_free(p);
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void ui_wake(void *arg)
{
    xTaskNotifyGive((TaskHandle_t)arg);
}

/* Waiting for one 100 Hz scheduler tick added 10 ms even when the next input,
 * spring or refresh was already due. A short real blocking handoff allows
 * console/draw workers to run without charging every frame a whole tick. */
static void ui_wait(esp_timer_handle_t wake, uint32_t next)
{
    if (!s_panel_on && orb_display_sleep_poll()) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        return;
    }
    /* Dark animations and indev read timers have no presentation deadline.
     * Physical input interrupts this block; the policy still polls at100 ms. */
    uint32_t delay_us = !s_panel_on ? 100000u : next ? (next > 10 ? 10000u : next * 1000u) : 100u;
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
    int reason = next ? 1 : 2;
    int64_t started = esp_timer_get_time();
    SCROLL_TRACE(ORB_SCROLL_TRACE_WAIT_BEGIN, started, delay_us, reason);
#endif
    if (wake && esp_timer_start_once(wake, delay_us) == ESP_OK) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_timer_stop(wake); /* A physical input may beat this deadline. */
    } else {
        /* Allocation/start failure preserves responsiveness, albeit at the
         * old coarse cadence. The warning on timer creation exposes fallback. */
        TickType_t ticks = pdMS_TO_TICKS((delay_us + 999u) / 1000u);
        vTaskDelay(ticks ? ticks : 1);
    }
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
    SCROLL_TRACE(ORB_SCROLL_TRACE_WAIT_END, esp_timer_get_time(),
                 (int32_t)(esp_timer_get_time() - started), reason);
#endif
}

static uint32_t ui_handle(void)
{
    bool external = atomic_exchange(&s_external_wake, false);
    bool service = atomic_exchange(&s_service_wake, false);
    if (!s_panel_on && orb_display_sleep_poll()) {
        orb_display_touch_service();
        if (s_sleep_poll_callback) s_sleep_poll_callback();
        if (!s_panel_on && orb_display_sleep_poll() && !service) return UINT32_MAX;
    }
    if (external && s_power_timer) lv_timer_ready(s_power_timer);
    return lv_timer_handler();
}

static void ui_task(void *arg)
{
    SemaphoreHandle_t started = arg;
    s_ui_task = xTaskGetCurrentTaskHandle();

    /* Keep the SPI interrupt on the UI core. The sender may wait from core 0;
     * SPI result queues fence descriptor/timing ownership across the cores. */
    esp_err_t err = orb_board_panel_start(TRANSFER_BYTES, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }

#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    /* Keep descriptor tables out of the internal graphics/audio margin. A
     * failed optional diagnostic allocation never prevents normal rendering. */
    s_accounting = heap_caps_calloc(1, sizeof(*s_accounting), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_accounting) ESP_LOGW(TAG, "render accounting unavailable: PSRAM allocation failed");
#endif
    band_power_init();
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_obj_raster_set_allocator(raster_malloc, raster_free);

    s_draw_buf = heap_caps_aligned_alloc(64, DRAW_ARENA_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(s_draw_buf);
    pipeline_init();

    s_disp = lv_display_create(ORB_LCD_W, ORB_LCD_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, BAND_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
    lv_display_set_band_cb(s_disp, band_cb);
    lv_display_set_refresh_phase_cb(s_disp, cadence_refresh_phase);
    lv_display_set_refresh_gate_cb(s_disp, refresh_gate);
    /* Damage wakes the paused refresh timer. Admit it on the next UI pass:
     * adding a second 16 ms timer after input/animation misses panel periods.
     * Clean surfaces retain LVGL's paused timer and cause no redraw. */
    lv_timer_set_period(lv_display_get_refr_timer(s_disp), 1);
    /* One tile per band. Tiles let both draw threads work on halves of a band,
     * but they'd cut through glass, whose blur must see all of it, and measured
     * no faster: what overlaps (canvas, content, glass) is drawn in order anyway. */
    lv_display_set_tile_cnt(s_disp, 1);
    lv_display_add_event_cb(s_disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(s_disp, on_render, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(s_disp, on_render, LV_EVENT_REFR_READY, NULL);
    lv_display_add_event_cb(s_disp, on_render, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_disp, on_render, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(s_disp, on_render, LV_EVENT_FLUSH_FINISH, NULL);
    lv_display_add_event_cb(s_disp, on_render, LV_EVENT_FLUSH_WAIT_START, NULL);
    lv_display_add_event_cb(s_disp, on_render, LV_EVENT_FLUSH_WAIT_FINISH, NULL);

    orb_display_touch_start(s_disp, UI_PRIORITY - 1);

    xTaskCreatePinnedToCore(send_task, "lcd_send", 3072, NULL, UI_PRIORITY + 1, NULL, SENDER_CORE);
    ESP_LOGI(TAG, "bands of %d rows; internal RAM free %u KB", BAND_ROWS,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    xSemaphoreGive(started);

    esp_timer_handle_t wake = NULL;
    const esp_timer_create_args_t wake_args = {
        .callback = ui_wake, .arg = xTaskGetCurrentTaskHandle(), .name = "ui_deadline",
    };
    if (esp_timer_create(&wake_args, &wake) != ESP_OK) {
        ESP_LOGW(TAG, "precise UI wait unavailable; using RTOS ticks");
    }
    for (;;) {
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
        int64_t iteration = esp_timer_get_time();
        SCROLL_TRACE(ORB_SCROLL_TRACE_LOOP_BEGIN, iteration, 0, 0);
#endif
        int64_t cadence_began = cadence_loop_begin();
        lv_lock();
        int64_t cadence_locked = cadence_began ? esp_timer_get_time() : 0;
        uint32_t animation_before = cadence_animation_before();
        uint64_t cadence_refresh_before = s_cadence.refresh_us;
        if (cadence_began) {
            orb_display_phase_begin(&s_phase_clock, cadence_locked);
            lv_timer_set_observer_cb(cadence_timer);
        }
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
        s_iteration_start = esp_timer_get_time();
#endif
        uint32_t next = ui_handle();
        if (cadence_began) lv_timer_set_observer_cb(NULL);
        cadence_loop_handled(cadence_began, cadence_locked, animation_before, cadence_refresh_before);
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
        int64_t handled = esp_timer_get_time();
        SCROLL_TRACE(ORB_SCROLL_TRACE_LOOP_END, handled, (int32_t)(handled - iteration),
                     next > INT32_MAX ? INT32_MAX : (int32_t)next);
#endif
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
        s_iteration_start = 0;
#endif
        /* The sender cannot take lv_lock(): the UI may be waiting for its
         * slot/completion. Redraw only here, after rendering has finished. */
        portENTER_CRITICAL(&s_pipeline_lock);
        bool redraw = s_pipeline.redraw;
        s_pipeline.redraw = false;
        portEXIT_CRITICAL(&s_pipeline_lock);
        if (redraw) lv_obj_invalidate(lv_screen_active());
        try_double_buffer();
        lv_unlock();
        int64_t cadence_wait_began = s_cadence.active ? esp_timer_get_time() : 0;
        ui_wait(wake, next);
        cadence_wait(cadence_wait_began, next);
    }
}

esp_err_t orb_display_start(void)
{
    orb_geom_half_chords(s_half_chord, ORB_LCD_H);

    s_flush_done = xSemaphoreCreateBinary();
    s_jobs = xQueueCreate(4, sizeof(job_t));
    ESP_RETURN_ON_FALSE(s_flush_done && s_jobs, ESP_ERR_NO_MEM, TAG, "queues");

    const gpio_config_t te = {
        .pin_bit_mask = 1ULL << ORB_LCD_TE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&te), TAG, "te gpio");
    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "isr service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(ORB_LCD_TE_GPIO, on_te, NULL), TAG, "te isr");

    SemaphoreHandle_t started = xSemaphoreCreateBinary();
    /* The merged kit still overflows 8 KiB on real hardware after deferred
     * rim updates. Retain the measured 12 KiB capacity; shrinking ui_tick's
     * nested scene work is a separate shell/main change. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(ui_task, "ui", 12288, started, UI_PRIORITY, NULL, UI_CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "ui task");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(started, pdMS_TO_TICKS(5000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "start");
    vSemaphoreDelete(started);
    return ESP_OK;
}

void orb_display_request_double_buffer(void)
{
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
    portENTER_CRITICAL(&s_pipeline_lock);
    s_double_buffer_requested = true;
    portEXIT_CRITICAL(&s_pipeline_lock);
#endif
}

void orb_display_set_snapshot_transition(bool active)
{
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
    /* The two views alias. Restore before re-enabling live glass's band guard,
     * including cancellation/teardown before another RENDER_START occurs. */
    if (!active && s_snapshot_transition) install_draw_buffers(false);
#endif
    s_snapshot_transition = active;
    s_independent_bands = active;
}

void orb_display_set_band_guard(orb_display_band_guard_t guard)
{
    s_guard = guard;
}

void orb_display_brightness(int pct)
{
    /* Commands queue behind bands, so they never cut into one. */
    const job_t j = { .brightness = pct };
    xQueueSend(s_jobs, &j, portMAX_DELAY);
}

bool orb_display_panel_awake(bool awake)
{
    if (!s_panel_done) s_panel_done = xSemaphoreCreateBinary();
    if (!s_panel_done) return false;
    const job_t job = { .brightness = -1, .panel_request = awake ? 1 : -1 };
    if (xQueueSend(s_jobs, &job, portMAX_DELAY) != pdTRUE) return false;
    return xSemaphoreTake(s_panel_done, portMAX_DELAY) == pdTRUE && s_panel_result;
}

bool orb_display_set_brightness(int pct)
{
    if (!s_panel_done) s_panel_done = xSemaphoreCreateBinary();
    if (!s_panel_done) return false;
    const job_t job = { .brightness = pct, .command_fence = true };
    if (xQueueSend(s_jobs, &job, portMAX_DELAY) != pdTRUE) return false;
    return xSemaphoreTake(s_panel_done, portMAX_DELAY) == pdTRUE && s_panel_result;
}

bool orb_display_panel_ready(bool awake)
{
    return orb_board_panel_ready(awake);
}

void orb_display_set_panel_on(bool on)
{
    if (on == s_panel_on) return;
    s_panel_on = on;
    if (on && s_disp) lv_obj_invalidate(lv_screen_active());
}

void orb_display_wake(void)
{
    atomic_store(&s_external_wake, true);
    if (s_ui_task) xTaskNotifyGive(s_ui_task);
}

void orb_display_wake_service(void)
{
    atomic_store(&s_service_wake, true);
    orb_display_wake();
}

void orb_display_set_power_timer(lv_timer_t *timer)
{
    s_power_timer = timer;
}

bool orb_display_sleep_poll(void)
{
    return atomic_load(&s_sleep_poll);
}

void orb_display_set_sleep_poll(bool eligible, void (*poll)(void))
{
    s_sleep_poll_callback = poll;
    bool active = eligible && poll;
    if (atomic_exchange(&s_sleep_poll, active) == active) return;
    orb_display_touch_sleep_changed();
    orb_display_wake();
}

bool orb_display_pipeline_active(void)
{
    portENTER_CRITICAL(&s_pipeline_lock);
    bool active = s_pipeline.enabled;
    portEXIT_CRITICAL(&s_pipeline_lock);
    return active;
}

void orb_display_counters(orb_display_counters_t *out)
{
    portENTER_CRITICAL(&s_st_lock);
    *out = s_st;
    out->at_us = esp_timer_get_time();
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
    out->half_frames = s_half_frames;
    out->full_frames = s_full_frames;
#endif
#if CONFIG_ORB_DISPLAY_TE_PACING
    out->paced_scheduled = s_pacing.scheduled;
    out->paced_on_slot = s_pacing.on_slot;
    out->paced_late = s_pacing.late;
    out->paced_skipped = s_pacing.skipped;
    out->paced_period_us = s_pacing.period_us;
    out->paced_divisor = s_pacing.divisor;
    if (s_pacing.mode) {
        out->paced_lead_us = s_pacing.budget_us[s_pacing.mode - 1];
        out->paced_capacity_us = s_pacing.capacity_budget_us[s_pacing.mode - 1];
    }
#endif
    portEXIT_CRITICAL(&s_st_lock);
}

bool orb_display_te_pacing_active(void)
{
#if CONFIG_ORB_DISPLAY_TE_PACING
    int64_t fall, period, scan;
    return te_now(&fall, &period, &scan) && period >= 8000 && period <= 40000;
#else
    return false;
#endif
}

bool orb_display_refresh_pending(void)
{
    return s_disp && s_disp->inv_p != 0;
}

uint32_t orb_display_response_begin(int64_t input_us, const lv_area_t *feedback)
{
    portENTER_CRITICAL(&s_st_lock);
    uint32_t token = ++s_response_sequence;
    if (!token) token = ++s_response_sequence;
    s_response = (orb_display_response_t) {
        .token = token, .input_us = input_us,
        .x1 = feedback ? feedback->x1 : 0, .y1 = feedback ? feedback->y1 : 0,
        .x2 = feedback ? feedback->x2 : ORB_LCD_W - 1,
        .y2 = feedback ? feedback->y2 : ORB_LCD_H - 1, .causal = feedback != NULL,
    };
    /* RENDER_START observers run after this driver's accounting observer but
     * before the first flush. Old queued jobs already own their prior token. */
    s_frame_response_token = token;
    s_frame_response_settled = false;
    portEXIT_CRITICAL(&s_st_lock);
    return token;
}

void orb_display_response_settled(uint32_t token)
{
    portENTER_CRITICAL(&s_st_lock);
    if (s_response.token == token) s_response.settled = true;
    portEXIT_CRITICAL(&s_st_lock);
}

void orb_display_response_snapshot(orb_display_response_t *out)
{
    portENTER_CRITICAL(&s_st_lock);
    *out = s_response;
    portEXIT_CRITICAL(&s_st_lock);
}

void orb_display_response_end(uint32_t token)
{
    portENTER_CRITICAL(&s_st_lock);
    if (s_response.token == token) s_response.token = 0;
    portEXIT_CRITICAL(&s_st_lock);
}

uint32_t orb_display_panel_period_us(void)
{
    int64_t fall, period, scan;
    return te_now(&fall, &period, &scan) && period <= UINT32_MAX ? (uint32_t)period : 0;
}

void orb_display_stats(orb_display_stats_t *out)
{
    /* Main's diagnostic loop has a small stack: do not copy the histogram or
     * pipeline counters that these averages do not use. */
    typedef struct {
        int64_t at_us;
        uint64_t frames, frame_us, draw_us, wait_us, hold_us, send_us, bands, bytes, torn;
    } stats_sample_t;
    static stats_sample_t previous;
    stats_sample_t now;
    portENTER_CRITICAL(&s_st_lock);
    now = (stats_sample_t){
        .at_us = esp_timer_get_time(), .frames = s_st.frames, .frame_us = s_st.frame_us,
        .draw_us = s_st.draw_us, .wait_us = s_st.wait_us, .hold_us = s_st.hold_us,
        .send_us = s_st.send_us, .bands = s_st.bands, .bytes = s_st.bytes, .torn = s_st.torn,
    };
    portEXIT_CRITICAL(&s_st_lock);
    float secs = (now.at_us - previous.at_us) / 1e6f;
    uint64_t frames = now.frames - previous.frames;
    float n = frames ? frames : 1;
    out->fps = secs > 0 ? frames / secs : 0;
    out->frame_ms = (now.frame_us - previous.frame_us) / n / 1000;
    out->hold_ms = (now.hold_us - previous.hold_us) / n / 1000;
    out->send_ms = (now.send_us - previous.send_us) / n / 1000;
    out->draw_ms = (now.draw_us - previous.draw_us) / n / 1000;
    out->wait_ms = (now.wait_us - previous.wait_us) / n / 1000;
    out->bands_per_frame = (now.bands - previous.bands) / n;
    out->kb_per_frame = (now.bytes - previous.bytes) / n / 1024;
    out->torn = now.torn - previous.torn;
    previous = now;
#if CONFIG_ORB_DISPLAY_INTERNAL_IMAGE_SCRATCH || CONFIG_ORB_DISPLAY_INTERNAL_BLUR_SCRATCH || \
    CONFIG_ORB_DISPLAY_INTERNAL_DRAW_MASKS
    lv_draw_scratch_stats_t scratch;
    lv_draw_scratch_get_stats(&scratch);
    ESP_LOGI(TAG, "draw scratch: internal=%u/%u/%u PSRAM=%u/%u/%u failed=%u/%u/%u charged=%u peak=%u",
             (unsigned)scratch.internal_allocations[0], (unsigned)scratch.internal_allocations[1], (unsigned)scratch.internal_allocations[2],
             (unsigned)scratch.psram_allocations[0], (unsigned)scratch.psram_allocations[1], (unsigned)scratch.psram_allocations[2],
             (unsigned)scratch.failures[0], (unsigned)scratch.failures[1], (unsigned)scratch.failures[2],
             (unsigned)scratch.internal_in_use, (unsigned)scratch.peak_internal);
#endif
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
    portENTER_CRITICAL(&s_st_lock);
    uint32_t halves = s_half_frames, full = s_full_frames;
    portEXIT_CRITICAL(&s_st_lock);
    ESP_LOGI(TAG, "adaptive refresh totals: half=%u full=%u", (unsigned)halves, (unsigned)full);
#endif
}

void orb_display_accounting(orb_display_accounting_t *out)
{
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    portENTER_CRITICAL(&s_st_lock);
    if (s_accounting) {
        *out = *s_accounting;
#if CONFIG_ORB_DISPLAY_TE_PACING
        out->paced_scheduled = s_pacing.scheduled;
        out->paced_on_slot = s_pacing.on_slot;
        out->paced_late = s_pacing.late;
        out->paced_skipped = s_pacing.skipped;
        out->paced_boundaries = s_pacing.boundaries;
        out->paced_unpaced = s_pacing.unpaced;
        out->paced_admitted_30 = s_pacing.admitted_30;
        out->paced_period_us = s_pacing.period_us;
        out->paced_divisor = s_pacing.divisor;
        out->paced_max_error_us = s_pacing.max_error_us;
#endif
    }
    else memset(out, 0, sizeof(*out));
    portEXIT_CRITICAL(&s_st_lock);
#else
    memset(out, 0, sizeof(*out));
#endif
}

void orb_display_account_cold_prepare(uint64_t elapsed_us)
{
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    portENTER_CRITICAL(&s_st_lock);
    if (s_accounting) s_accounting->cold_prepare_us += elapsed_us;
    portEXIT_CRITICAL(&s_st_lock);
#else
    (void)elapsed_us;
#endif
}

bool orb_display_capture_snapshot(lv_obj_t *object, lv_draw_buf_t *destination)
{
    if (!s_panel_on || !orb_board_panel_ready(true) || !s_disp || !s_draw_buf || !orb_display_drain()) return false;
    lv_draw_wait_for_finish();
    /* Both adaptive halves alias this arena. The FIFO/SPI fence above retires
     * their transport ownership before cold drawing borrows the full view. */
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
    install_draw_buffers(false);
#endif
    return orb_display_snapshot_bands(object, destination, s_draw_buf, BAND_BYTES, s_guard);
}

bool orb_display_drain(void)
{
    /* UI-thread benchmark seam: FIFO barrier includes actual SPI result drain
     * for all queued jobs, even the old optional snapshot pipeline. */
    if (!s_drain_done) s_drain_done = xSemaphoreCreateBinary();
    if (!s_drain_done) return false;
    job_t barrier = { .brightness = -1 };
    if (xQueueSend(s_jobs, &barrier, portMAX_DELAY) != pdTRUE) return false;
    if (xSemaphoreTake(s_drain_done, portMAX_DELAY) != pdTRUE) return false;
    if (s_flush_pending) {
        xSemaphoreTake(s_flush_done, 0);
        s_flush_pending = false;
        lv_display_flush_ready(s_disp);
    }
#if CONFIG_ORB_DISPLAY_TE_PACING
    /* A benchmark/navigation barrier ends the previous effect's admission.
     * Its next changed scaffold remains immediate feedback, like idle entry. */
    portENTER_CRITICAL(&s_st_lock);
    s_pacing.mode = 0;
    s_pacing.target_us = 0;
    s_pacing.probing = false;
    portEXIT_CRITICAL(&s_st_lock);
    s_gate_pending = false;
#endif
    return true;
}

bool orb_display_accounting_active(void)
{
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    return s_accounting != NULL;
#else
    return false;
#endif
}
