"""Run production pacing/gate/sender boundaries with virtual RTOS and SPI fences."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    component = Path(__file__).resolve().parents[2]
    source = (component / "orb_display.c").read_text()

    def function(signature):
        start = source.index(signature)
        start = source.rfind("\n", 0, start) + 1
        end = source.index("\n}", start) + len("\n}")
        return source[start:end]

    fixture = r"""
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "orb_display_pacing.h"
#include "orb_display_power.h"
#include "orb_display_pipeline.h"
#include "orb_display_response.h"
#include "orb_display_cadence.h"
static orb_display_cadence_t s_cadence;
static int64_t s_cadence_refresh_started;
#define SCROLL_TRACE(kind, at, a, b) ((void)0)
static void refresh_memory(bool started, int64_t now) { (void)started; (void)now; }
#define CONFIG_ORB_DISPLAY_TE_PACING 1
#define CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING 0
#define CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER 0
#define ORB_DISPLAY_LATENCY_BUCKET_US 4000
#define ORB_DISPLAY_LATENCY_BUCKETS 64
#define ORB_DISPLAY_INTERVAL_BUCKETS 128
#define portMAX_DELAY 7
#define pdTRUE 1
#define ESP_OK 0
#define TAG "test"
#define esp_err_to_name(err) ((err) == ESP_OK ? "OK" : "ERROR")
#define ESP_LOGE(tag, format, ...) do { (void)(tag); if (0) fprintf(stderr, format, __VA_ARGS__); } while (0)
#define ESP_LOGW(tag, message) do { (void)(tag); (void)(message); } while (0)
#define pdMS_TO_TICKS(ms) ((ms) * FIXTURE_TICK_HZ / 1000)
typedef int esp_err_t;
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef struct { void (*callback)(void *); void *arg; const char *name; } esp_timer_create_args_t;
typedef struct { esp_timer_create_args_t args; int64_t due; bool active; } fixture_timer_t;
typedef fixture_timer_t *esp_timer_handle_t;
static fixture_timer_t s_sender_timer;
static esp_timer_handle_t s_sender_wake;
static bool s_timer_create_fail, s_timer_start_fail;
static unsigned s_timer_blocks, s_task_delays, s_task_notifications;
static uint64_t s_timer_lateness;
typedef int lv_display_t;
static lv_display_t s_display;
typedef struct { int x1, y1, x2, y2; } lv_area_t;
typedef enum { LV_EVENT_REFR_START, LV_EVENT_REFR_READY, LV_EVENT_RENDER_START, LV_EVENT_FLUSH_WAIT_START,
    LV_EVENT_FLUSH_WAIT_FINISH, LV_EVENT_FLUSH_START, LV_EVENT_FLUSH_FINISH } lv_event_t;
typedef struct {
    const uint8_t *px;
    lv_area_t area;
    int brightness, panel_request;
    bool command_fence;
    bool last, power_owned;
    int64_t frame_start;
    uint32_t response_token;
    bool response_settled;
    int slot;
    int64_t deadline_us, gate_start_us;
    uint8_t pace_mode;
    uint32_t ui_work_us;
    bool overlapped;
} job_t;
static struct {
    uint64_t hold_us, send_us, draw_us, wait_us, bytes, bands, torn, transfer_errors,
        frames, frame_us, failed_frames, pipeline_dropped, queue_wait_us, pipeline_bands,
        dma_wire_us, dma_queue_us, dma_cleanup_us, dma_timed_bands, dma_timed_frames;
    unsigned frame_latency[ORB_DISPLAY_LATENCY_BUCKETS];
    unsigned completion_interval[ORB_DISPLAY_INTERVAL_BUCKETS];
    int last_transfer_error;
} s_st;
static orb_pacing_t s_pacing;
static orb_pipeline_t s_pipeline;
static orb_display_response_t s_response;
static int64_t s_gate_deadline, s_gate_start, s_frame_deadline, s_frame_gate_start;
static int64_t s_frame_start, s_draw_start, s_wait_start, s_now;
static int64_t s_interval_completion, s_last_dma_done_us;
static bool s_last_dma_timed;
static uint8_t s_gate_mode, s_frame_pace_mode;
static uint32_t s_frame_pace_wait, s_frame_response_token, s_last_sender_work, s_frame_admission_work;
static uint64_t s_frame_draw_baseline;
static bool s_snapshot_transition;
static bool s_panel_on = true, s_panel_result, s_board_ready = true;
static int s_panel_done = 5;
static bool orb_board_panel_ready(bool awake) { return awake && s_board_ready; }
static void orb_board_panel_service(void) {}
static uint32_t orb_board_panel_wait_ms(void) { return UINT32_MAX; }
static void orb_display_wake(void) {}
static int orb_board_panel_awake(bool awake) { (void)awake; return ESP_OK; }
static bool s_gate_pending, s_frame_response_settled, s_flush_pending, s_next_last;
static bool s_pace_frame_started, s_pace_final_queued;
static unsigned s_st_lock, s_pipeline_lock, s_locks;
static int s_jobs = 1, s_free_slots = 2, s_flush_done = 3, s_drain_done = 4;
static float s_fixed_us = 60, s_ns_per_byte[2] = {31.5f, 50};
static bool s_te_valid = true, s_moving, s_fail_write, s_fail_finish, s_dma_finished, s_missing_isr;
static bool s_detect_long_transfer, s_timing_unavailable, s_te_follow_time;
static int64_t s_fall = 1000000, s_te_delay, s_write_at;
static int64_t s_dma_start, s_dma_done, s_cleanup_delay, s_queue_delay, s_release_at;
static int64_t s_meets_fall, s_meets_start;
static unsigned s_writes, s_finished, s_ready, s_released, s_power_refs;
static unsigned s_rom_waits;
static uint32_t s_last_rom_wait;
static job_t s_fixture_jobs[4];
static unsigned s_job_count, s_job_index;
static jmp_buf s_sender_done;
#define portENTER_CRITICAL(lock) do { assert((lock) == &s_st_lock || (lock) == &s_pipeline_lock); assert(!s_locks++); } while (0)
#define portEXIT_CRITICAL(lock) do { assert((lock) == &s_st_lock || (lock) == &s_pipeline_lock); assert(s_locks-- == 1); } while (0)
static int64_t esp_timer_get_time(void) { return s_now; }
static bool te_now(int64_t *fall, int64_t *period, int64_t *scan) {
    *fall = s_te_follow_time ? s_now / 16800 * 16800 : s_fall;
    *period = 16800; *scan = 16000; return s_te_valid;
}
static bool lv_draw_sw_blur_motion_active(lv_display_t *disp) { (void)disp; return s_moving; }
static lv_event_t lv_event_get_code(lv_event_t *event) { return *event; }
static lv_display_t *lv_event_get_current_target(lv_event_t *event) { (void)event; return &s_display; }
/* Exact damage union is tested separately; this fixture verifies forwarding. */
static void cadence_damage(const lv_display_t *disp) { assert(disp == &s_display); }
static bool lv_display_flush_is_last(lv_display_t *disp) { (void)disp; return s_next_last; }
static int lv_area_get_size(const lv_area_t *a) { return (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1); }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &s_sender_timer; }
static void xTaskNotifyGive(TaskHandle_t task) {
    assert(task == &s_sender_timer); s_task_notifications++;
}
static int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    assert(args->callback && args->arg == &s_sender_timer && !strcmp(args->name, "lcd_deadline"));
    if (s_timer_create_fail) return -1;
    s_sender_timer = (fixture_timer_t){ .args = *args };
    *out = &s_sender_timer;
    return ESP_OK;
}
static int esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay) {
    assert(timer == &s_sender_timer && delay > 0);
    if (s_timer_start_fail || timer->active) return -1;
    timer->active = true; timer->due = s_now + delay;
    return ESP_OK;
}
static unsigned ulTaskNotifyTake(int clear, int timeout) {
    assert(clear == pdTRUE && timeout == portMAX_DELAY && s_power_refs);
    s_timer_blocks++;
    if (!s_task_notifications) {
        assert(s_sender_timer.active);
        /* Blocking lets the lower-priority UI/draw work run. This emulates
         * the completion notification, never a runnable high-priority poll. */
        s_now = s_sender_timer.due + s_timer_lateness;
        s_sender_timer.active = false;
        s_sender_timer.args.callback(s_sender_timer.args.arg);
    }
    unsigned notifications = s_task_notifications;
    s_task_notifications = 0;
    return notifications;
}
static void vTaskDelay(unsigned ticks) {
    assert(s_power_refs && ticks > 0); /* A zero-tick yield cannot unblock the UI. */
    s_task_delays++; s_now += ticks * (1000000 / FIXTURE_TICK_HZ);
}
static void esp_rom_delay_us(uint32_t us) { s_rom_waits++; s_last_rom_wait = us; s_now += us; }
static void te_hold(int y1, int y2, int64_t wire) {
    assert(y2 > y1 && wire > 0 && s_power_refs); s_now += s_te_delay;
}
static bool te_meets(int64_t now, int64_t wire, int y1, int y2, int64_t fall, int64_t period, int64_t scan) {
    (void)y1; (void)y2; (void)scan;
    assert(fall <= now && now < fall + period);
    s_meets_fall = fall; s_meets_start = now;
    return s_detect_long_transfer && wire > 1000;
}
static int orb_board_panel_write(int x1, int y1, int x2, int y2, const uint8_t *px, size_t bytes) {
    assert(x2 >= x1 && y2 >= y1 && px && bytes && s_power_refs);
    s_write_at = s_now; s_writes++; s_dma_finished = false;
    s_dma_start = s_write_at + s_queue_delay; s_dma_done = s_dma_start + 64;
    bool failed = s_fail_write; s_fail_write = false; return failed ? -1 : ESP_OK;
}
static int orb_board_panel_finish(void) {
    assert(s_power_refs && !s_dma_finished); s_finished++;
    s_now = s_dma_done + s_cleanup_delay;
    s_dma_finished = true; return s_fail_finish || s_missing_isr ? -2 : ESP_OK;
}
static bool orb_board_panel_timing(int64_t *started, int64_t *completed) {
    assert(s_dma_finished && s_power_refs);
    if (s_missing_isr || s_timing_unavailable) return false;
    *started = s_dma_start; *completed = s_dma_done; return true;
}
static int orb_board_panel_set_brightness(int brightness) { (void)brightness; return ESP_OK; }
static int xQueueReceive(int queue, job_t *out, int timeout) {
    assert(queue == s_jobs && timeout == portMAX_DELAY);
    if (s_job_index == s_job_count) longjmp(s_sender_done, 1);
    *out = s_fixture_jobs[s_job_index++];
    return pdTRUE;
}
static int xQueueSend(int queue, const void *item, int timeout) {
    if (queue == s_jobs) {
        assert(timeout == portMAX_DELAY && s_job_count < 4);
        s_fixture_jobs[s_job_count++] = *(const job_t *)item;
        return pdTRUE;
    }
    const unsigned *slot = item;
    assert(queue == s_free_slots && *slot < ORB_PIPELINE_SLOTS && !timeout);
    assert(s_pipeline.slot[*slot] == ORB_SLOT_FREE && s_power_refs); s_ready++; return pdTRUE;
}
static void xSemaphoreGive(int semaphore) {
    if (semaphore == s_drain_done || semaphore == s_panel_done) return;
    assert(semaphore == s_flush_done && s_dma_finished && s_power_refs); s_ready++;
}
static void band_power_release(bool owned) {
    assert(owned && s_power_refs && s_ready == s_released + 1);
    if (s_finished) assert(s_dma_finished && s_now >= s_dma_done + s_cleanup_delay);
    s_release_at = s_now; s_power_refs--; s_released++;
}
static bool band_power_acquire(void) { s_power_refs++; return true; }
static bool snapshot_band(job_t *job) { (void)job; return false; }
"""
    fixture += "\n\n".join(
        function(signature)
        for signature in (
            "static void sender_wake(void *arg)",
            "static void sender_wait_until(int64_t until)",
            "static uint32_t refresh_gate(lv_display_t *disp)",
            "static void on_render(lv_event_t *e)",
            "static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)",
            "static bool send_band(const job_t *j)",
            "static void send_task(void *arg)",
        )
    )
    fixture += r"""
static void event(lv_event_t kind) { s_cadence.active = true; on_render(&kind); assert(!s_locks); }
static void gate_and_forced_refresh(void) {
    s_panel_on = false;
    for (unsigned i = 0; i < 100; i++) assert(refresh_gate(NULL) == UINT32_MAX);
    assert(!s_gate_pending && !s_writes);
    s_panel_on = true; s_board_ready = false;
    assert(refresh_gate(NULL) == UINT32_MAX && !s_gate_pending);
    s_board_ready = true;
    s_now = s_fall;
    assert(refresh_gate(NULL) == 0 && s_gate_pending);
    s_frame_admission_work = 300;
    event(LV_EVENT_RENDER_START);
    assert(!s_gate_pending && !s_frame_deadline); /* immediate causal feedback */
    assert(!s_frame_admission_work); /* no previous refresh's proof cost leaks */
    assert(s_frame_pace_mode == 1 && s_pacing.probing);
    s_now += 4000;
    orb_pacing_complete(&s_pacing, 0, s_frame_gate_start, s_now, 0, s_frame_pace_mode, true, 0);
    assert(!s_pacing.probing);
    s_now += 100;
    unsigned delay = refresh_gate(NULL);
    assert(delay == 7 && !s_gate_pending); /* 8200 us retries after 7 ms */
    s_now += delay * 1000;
    assert(refresh_gate(NULL) == 0 && s_gate_pending && s_gate_deadline);
    assert(s_rom_waits == 1 && s_last_rom_wait == 1200); /* finish exact TE start */
    uint64_t misses = s_pacing.late;
    event(LV_EVENT_REFR_READY); /* layout/clipping consumed all damage */
    assert(!s_gate_pending && s_pacing.late == misses + 1);
    s_now += 100;
    event(LV_EVENT_RENDER_START); /* synchronous capture bypasses admission */
    assert(!s_frame_deadline && !s_frame_gate_start && !s_frame_pace_mode);
    s_te_valid = false;
    assert(refresh_gate(NULL) == 0);
    event(LV_EVENT_RENDER_START);
    assert(!s_frame_deadline && !s_frame_pace_mode);
    s_te_valid = true;
    memset(&s_pacing, 0, sizeof(s_pacing)); s_now = s_fall;
    assert(refresh_gate(NULL) == 0 && s_gate_pending && s_pacing.probing);
    event(LV_EVENT_REFR_READY); /* no layout/draw work after initial admission */
    assert(!s_gate_pending && !s_pacing.probing);
    assert(refresh_gate(NULL) == 0 && s_gate_pending && s_pacing.probing);
    event(LV_EVENT_RENDER_START);
    event(LV_EVENT_REFR_READY); /* round clipping produced no final-band job */
    assert(!s_gate_pending && !s_pacing.probing);
    /* Cached navigation bypasses the live latency/capacity planner, keeps
     * the driver-owned SPI path, and cannot leave a live deadline behind. */
    s_gate_deadline = 2000000; s_gate_start = 1990000; s_gate_mode = 2;
    s_snapshot_transition = true;
    assert(refresh_gate(NULL) == 0 && s_gate_pending);
    assert(!s_gate_deadline && !s_gate_start && !s_gate_mode);
    assert(!s_pacing.mode && !s_pacing.probing && !s_pacing.target_us);
    event(LV_EVENT_RENDER_START);
    assert(!s_frame_deadline && !s_frame_gate_start && !s_frame_pace_mode);
    event(LV_EVENT_REFR_READY);
    s_snapshot_transition = false;
    assert(refresh_gate(NULL) == 0 && s_pacing.probing && s_gate_mode == 1);
    event(LV_EVENT_REFR_READY);
}
static void reset_sender(void) {
    memset(&s_st, 0, sizeof(s_st)); memset(&s_pacing, 0, sizeof(s_pacing));
    memset(&s_pipeline, 0, sizeof(s_pipeline));
    s_pacing.budget_us[0] = 12000;
    s_now = 1000; s_frame_pace_wait = 0; s_te_delay = 0;
    s_interval_completion = 0;
    s_last_dma_done_us = s_dma_start = s_dma_done = s_cleanup_delay = s_queue_delay = s_release_at = 0;
    s_job_count = s_job_index = s_writes = s_finished = s_ready = s_released = s_power_refs = 0;
    s_fail_write = s_fail_finish = s_dma_finished = false;
    s_missing_isr = s_detect_long_transfer = s_timing_unavailable = s_last_dma_timed = false;
    s_te_follow_time = false;
    s_meets_fall = s_meets_start = 0;
    s_sender_wake = NULL;
    s_sender_timer = (fixture_timer_t){0};
    s_timer_create_fail = s_timer_start_fail = false;
    s_timer_blocks = s_task_delays = s_task_notifications = 0;
    s_timer_lateness = 0;
    s_last_sender_work = 0;
    s_frame_draw_baseline = 0;
    s_frame_admission_work = 0;
    s_snapshot_transition = false;
}
static void queue_band(bool last) {
    static const uint8_t pixels[128] = {0};
    s_fixture_jobs[s_job_count++] = (job_t){ .px = pixels, .area = {0, 0, 31, 1}, .last = last,
        .power_owned = true, .frame_start = 500, .slot = -1, .deadline_us = 20000,
        .gate_start_us = 500, .pace_mode = 1 };
    s_power_refs++;
}
static void run_sender(void) { if (!setjmp(s_sender_done)) send_task(NULL); assert(!s_locks && !s_power_refs); }
static void sender_blocking_wait(void) {
    reset_sender();
    const esp_timer_create_args_t args = {
        .callback = sender_wake, .arg = xTaskGetCurrentTaskHandle(), .name = "lcd_deadline",
    };
    assert(esp_timer_create(&args, &s_sender_wake) == ESP_OK);
    s_power_refs = 1;
    unsigned rom = s_rom_waits;
    sender_wait_until(s_now + 8000);
    assert(s_now == 9000 && s_timer_blocks == 1 && !s_task_delays && s_rom_waits == rom);
    sender_wait_until(s_now + 201);
    assert(s_now == 9201 && s_timer_blocks == 2 && s_rom_waits == rom);
    sender_wait_until(s_now + 200);
    assert(s_now == 9401 && s_rom_waits == rom + 1 && s_last_rom_wait == 200);
    sender_wait_until(s_now); sender_wait_until(s_now - 1);
    assert(s_timer_blocks == 2 && s_rom_waits == rom + 1);
    /* Real timer delivery can be late. Do not spin or restart a timer after
     * its deadline has passed, and retain ownership throughout the wait. */
    s_timer_lateness = 1700;
    sender_wait_until(s_now + 5000);
    assert(s_now == 16101 && s_timer_blocks == 3 && s_power_refs == 1);
    /* Production uses 1 kHz ticks. Also exercise a 100 Hz configuration to
     * ensure the fallback remains blocking when a delay is below one tick.
     * Neither configuration may return early from the deadline wait. */
    s_timer_start_fail = true;
    int64_t fallback_start = s_now;
    sender_wait_until(s_now + 500);
    assert(s_now == fallback_start + 1000000 / FIXTURE_TICK_HZ && s_task_delays == 1);
    s_sender_wake = NULL;
    fallback_start = s_now;
    sender_wait_until(s_now + 15000);
    assert(s_now == fallback_start + (FIXTURE_TICK_HZ == 100 ? 20000 : 15000));
    assert(s_task_delays == (FIXTURE_TICK_HZ == 100 ? 3u : 2u));
    s_power_refs = 0;
}
static void sender_ownership(void) {
    reset_sender(); queue_band(true); run_sender();
    assert(s_write_at == 19936 && s_now == 20000);
    assert(s_finished == 1 && s_ready == 1 && s_released == 1);
    assert(s_pacing.on_slot == 1 && !s_pacing.late && s_st.frames == 1);
    assert(!s_frame_pace_wait);
    assert(s_timer_blocks == 1 && !s_task_delays);
    reset_sender(); s_timer_create_fail = true; queue_band(true); run_sender();
    assert(s_now >= 20000 && s_task_delays && !s_timer_blocks);
    assert(s_finished == 1 && s_ready == 1 && s_released == 1);
    reset_sender(); queue_band(false); run_sender();
    assert(s_write_at == 1000 && s_now == 1064 && !s_pacing.on_slot && !s_st.frames);
    reset_sender(); s_fail_write = true; queue_band(true); run_sender();
    assert(s_finished == 1 && s_released == 1 && !s_st.frames && s_st.failed_frames == 1);
    assert(s_st.transfer_errors == 1 && s_pacing.late == 1 && !s_pacing.on_slot);
    reset_sender(); s_fail_finish = true; queue_band(true); run_sender();
    assert(s_finished == 1 && s_released == 1 && s_st.failed_frames == 1 && s_pacing.late == 1);
    reset_sender(); s_fail_write = true; queue_band(false); queue_band(true); run_sender();
    assert(s_finished == 2 && s_released == 2 && s_st.transfer_errors == 1);
    assert(s_st.failed_frames == 1 && !s_st.frames && s_pacing.late == 1 && !s_pacing.on_slot);
    reset_sender(); s_te_delay = 2500; queue_band(true); run_sender();
    assert(s_now == 22500 && s_pacing.late == 1 && s_pacing.max_error_us == 2500);
    assert(s_st.frames == 1 && s_released == 1); /* scan avoidance wins over deadline */
    reset_sender(); queue_band(true);
    s_fixture_jobs[0].slot = 0;
    s_pipeline.slot[0] = ORB_SLOT_QUEUED; /* disabled pipeline still retires its lease */
    run_sender();
    assert(!s_writes && !s_finished && s_pipeline.slot[0] == ORB_SLOT_FREE);
    assert(s_st.failed_frames == 1 && s_st.pipeline_dropped == 1 && s_released == 1);
    reset_sender();
    s_frame_deadline = 20000; s_frame_gate_start = 500; s_frame_pace_mode = 1;
    s_frame_start = 500; s_next_last = true;
    uint8_t pixels[128] = {0}; lv_area_t area = {0, 0, 31, 1};
    flush_cb(NULL, &area, pixels);
    assert(s_flush_pending && s_job_count == 1 && s_power_refs == 1 && !s_ready);
    assert(s_fixture_jobs[0].deadline_us == 20000 && s_fixture_jobs[0].gate_start_us == 500);
    s_frame_deadline = 99999; s_frame_gate_start = 90000; s_frame_pace_mode = 2;
    run_sender(); /* Next-frame mutations cannot change a queued job's target. */
    assert(s_now == 20000 && s_pacing.on_slot == 1 && s_released == 1);
}
static void completion_intervals(void) {
    reset_sender(); queue_band(true); run_sender();
    assert(s_interval_completion == 20000 && !s_st.completion_interval[16]);
    s_job_count = s_job_index = 0; s_now = 30000; queue_band(true);
    s_fixture_jobs[0].deadline_us = 36900;
    run_sender();
    assert(s_interval_completion == 36900 && s_st.completion_interval[16] == 1);
    /* Failed DMA breaks the pair; recovery must not report the failure gap
     * as a successful presentation interval. */
    s_job_count = s_job_index = 0; s_now = 50000; s_fail_write = true;
    queue_band(true); s_fixture_jobs[0].deadline_us = 53800; run_sender();
    assert(!s_interval_completion);
    s_job_count = s_job_index = 0; s_now = 68000; queue_band(true);
    s_fixture_jobs[0].deadline_us = 70700; run_sender();
    unsigned count = 0;
    for (unsigned i = 0; i < ORB_DISPLAY_INTERVAL_BUCKETS; i++) count += s_st.completion_interval[i];
    assert(count == 1 && s_st.frames == 3 && s_st.failed_frames == 1);
}
static void capacity_excludes_scan_wait(void) {
    reset_sender();
    s_pacing.mode = 1; s_pacing.period_us = 16800; s_pacing.probing = true;
    s_te_delay = 20000; queue_band(true); s_fixture_jobs[0].deadline_us = 0;
    run_sender();
    assert(s_st.hold_us == 20000 && s_st.frames == 1);
    assert(s_pacing.budget_us[0] == 1064 && s_pacing.divisor == 1 && !s_pacing.probing);
    /* Both per-band scan holds and the final deadline wait belong to this
     * frame. Overwriting the first hold at final-band submission inflates
     * predicted work, even though measured HOLD/cadence must stay intact. */
    reset_sender();
    s_pacing.mode = 1; s_pacing.period_us = 16800;
    s_te_delay = 2500; queue_band(false); queue_band(true);
    run_sender();
    assert(s_now == 22500 && s_st.hold_us == 21372);
    assert(s_pacing.budget_us[0] == 1128 && s_pacing.late == 1 && s_pacing.max_error_us == 2500);
    /* Circular clipping may abandon an earlier frame before a final band.
     * Its accumulated wait cannot subtract from the next frame's capacity. */
    reset_sender();
    s_pacing.mode = 1; s_pacing.period_us = 16800; s_pacing.probing = true;
    s_te_delay = 2500; queue_band(false); queue_band(true);
    s_fixture_jobs[0].deadline_us = s_fixture_jobs[1].deadline_us = 0;
    s_fixture_jobs[1].frame_start = s_fixture_jobs[1].gate_start_us = 3500;
    run_sender();
    assert(s_now == 6128 && s_st.hold_us == 5000 && s_st.frames == 1);
    assert(s_pacing.budget_us[0] == 628 && !s_pacing.probing && s_pacing.divisor == 1);
}
static void producer_and_sender_capacity(void) {
    /* Final queueing freezes the producer's preparation+draw sample. A later
     * frame may mutate global counters before the FIFO sender consumes it. */
    reset_sender();
    s_frame_deadline = 30000; s_frame_gate_start = 1000; s_frame_pace_mode = 1;
    s_frame_start = 2000; s_next_last = true;
    s_frame_draw_baseline = 5000; s_st.draw_us = 18000; s_now = 18000;
    s_frame_admission_work = 300;
    uint8_t pixels[128] = {0}; lv_area_t area = {0, 0, 31, 1};
    flush_cb(NULL, &area, pixels);
    assert(s_fixture_jobs[0].ui_work_us == 14300);
    s_frame_start = 90000; s_st.draw_us = 100000; s_frame_admission_work = 900;
    s_pacing.mode = 1; s_pacing.period_us = 16800; s_pacing.probing = true;
    s_fixture_jobs[0].deadline_us = 0;
    s_fixture_jobs[0].overlapped = true;
    run_sender();
    assert(s_pacing.divisor == 1 && !s_pacing.probing && s_pacing.budget_us[0] > 16800);
    assert(s_pacing.capacity_budget_us[0] == 14800);
    /* Wire/setup/cleanup is summed across this frame's bands. It remains a
     * capacity limit even when DMA completion timestamps precede cleanup. */
    reset_sender();
    s_pacing.mode = 1; s_pacing.period_us = 16800; s_pacing.probing = true;
    s_cleanup_delay = 10000;
    queue_band(false); queue_band(true);
    s_fixture_jobs[0].deadline_us = s_fixture_jobs[1].deadline_us = 0;
    s_fixture_jobs[1].ui_work_us = 4000;
    s_fixture_jobs[1].overlapped = true;
    run_sender();
    assert(s_last_sender_work == 10064 && s_pacing.divisor == 2);
    /* A preceding abandoned refresh must not contribute sender service to
     * the next frame's independent capacity sample. */
    reset_sender();
    s_pacing.mode = 1; s_pacing.period_us = 16800; s_pacing.probing = true;
    s_cleanup_delay = 10000;
    queue_band(false); queue_band(true);
    s_fixture_jobs[0].deadline_us = s_fixture_jobs[1].deadline_us = 0;
    s_fixture_jobs[1].frame_start = s_fixture_jobs[1].gate_start_us = 11064;
    s_fixture_jobs[1].ui_work_us = 4000;
    s_fixture_jobs[1].overlapped = true;
    run_sender();
    assert(s_pacing.divisor == 1);
    /* A full-band serial frame cannot use max(producer,sender) admission:
     * both lanes fit T separately here, while their combined elapsed work
     * exceeds T. Preserve the serial latency-based conservative fallback. */
    reset_sender();
    s_pacing.mode = 1; s_pacing.period_us = 16800; s_pacing.probing = true;
    s_now = 8500; s_queue_delay = 8000;
    queue_band(false); queue_band(true);
    s_fixture_jobs[0].deadline_us = s_fixture_jobs[1].deadline_us = 0;
    s_fixture_jobs[1].ui_work_us = 8000;
    assert(!s_fixture_jobs[1].overlapped);
    run_sender();
    assert(s_last_sender_work == 8064 && s_pacing.divisor == 2);
    assert(s_pacing.budget_us[0] == 24628 && s_pacing.capacity_budget_us[0] == 24628);
}
static void interrupt_completion_excludes_cleanup(void) {
    reset_sender(); s_cleanup_delay = 10000; s_detect_long_transfer = true;
    queue_band(true); run_sender();
    assert(s_now == 30000 && s_release_at == 30000); /* Ownership waits for cleanup. */
    assert(s_last_dma_done_us == 20000 && s_interval_completion == 20000);
    assert(s_st.frames == 1 && s_st.frame_us == 19500 && !s_st.torn);
    assert(s_pacing.on_slot == 1 && !s_pacing.late && !s_pacing.max_error_us);
    assert(s_st.dma_wire_us == 64 && !s_st.dma_queue_us && s_st.dma_cleanup_us == 10000);
    assert(s_st.dma_timed_bands == 1 && s_st.dma_timed_frames == 1);
    assert(s_st.send_us == 10064); /* Task/fence cost remains separately visible. */
    s_job_count = s_job_index = 0; queue_band(true);
    s_fixture_jobs[0].deadline_us = 36900; run_sender();
    assert(s_now == 46900 && s_release_at == 46900);
    assert(s_interval_completion == 36900 && s_st.completion_interval[16] == 1);
    assert(s_pacing.on_slot == 2 && !s_pacing.late && !s_st.torn);
    reset_sender(); s_te_follow_time = true; s_cleanup_delay = 2 * 16800 + 5000;
    s_detect_long_transfer = true; queue_band(true); run_sender();
    /* te_now observes two later scan cycles after slow task cleanup. The
     * retrospective intersection must use the cycle containing DMA start. */
    assert(s_release_at == 58600 && s_last_dma_done_us == 20000);
    assert(s_meets_start == 19936 && s_meets_fall == 16800);
    assert(s_st.frames == 1 && !s_st.torn && s_pacing.on_slot == 1 && !s_pacing.late);
    reset_sender(); s_queue_delay = 250; queue_band(true); run_sender();
    assert(s_st.dma_queue_us == 250 && s_st.dma_wire_us == 64 && !s_st.dma_cleanup_us);
    reset_sender(); s_missing_isr = true; queue_band(true); run_sender();
    assert(!s_st.frames && !s_st.completion_interval[16] && !s_interval_completion);
    assert(s_st.failed_frames == 1 && s_st.transfer_errors == 1 && !s_pacing.on_slot);
    assert(s_ready == 1 && s_released == 1); /* Failure still releases after the fence. */
    reset_sender(); s_timing_unavailable = true; queue_band(true); run_sender();
    assert(s_st.frames == 1 && !s_st.failed_frames); /* Result fence is authoritative. */
    assert(!s_st.dma_timed_frames && !s_st.dma_timed_bands && !s_last_dma_timed);
    assert(!s_pacing.on_slot && s_pacing.late == 1); /* No verified ISR phase claim. */
}
int main(void) {
    gate_and_forced_refresh(); sender_blocking_wait(); sender_ownership(); completion_intervals(); capacity_excludes_scan_wait();
    producer_and_sender_capacity(); interrupt_completion_excludes_cleanup();
    printf("pacing driver (%uHz): pending/forced refresh, final wait, SPI failure and PM/buffer ownership passed\n",
           FIXTURE_TICK_HZ);
}
"""
    with tempfile.TemporaryDirectory(prefix="orb-pacing-driver-") as temporary:
        directory = Path(temporary)
        path = directory / "pacing_driver.c"
        path.write_text(fixture)
        for tick_hz in (1000, 100):
            binary = directory / f"pacing_driver_{tick_hz}"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CC", "cc")),
                    "-std=c11",
                    f"-DFIXTURE_TICK_HZ={tick_hz}",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-unused-parameter",
                    "-O1",
                    "-g",
                    "-fsanitize=address,undefined",
                    f"-I{component}",
                    f"-I{component / 'include'}",
                    str(path),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
