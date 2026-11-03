/* Instrumentation must exclude DMA wait and queue overhead from draw time. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "orb_display_accounting.h"
#include "orb_display_response.h"
#include "orb_display_cadence.h"
static orb_display_cadence_t s_cadence;
static int64_t s_cadence_refresh_started;
#define SCROLL_TRACE(kind, at, a, b) ((void)0)
static void refresh_memory(bool started, int64_t now) { (void)started; (void)now; }

static unsigned s_checks;
#define CHECK(cond) do { s_checks++; if (!(cond)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
} } while (0)

typedef enum {
    LV_EVENT_RENDER_START,
    LV_EVENT_REFR_START,
    LV_EVENT_REFR_READY,
    LV_EVENT_FLUSH_WAIT_START,
    LV_EVENT_FLUSH_WAIT_FINISH,
    LV_EVENT_FLUSH_START,
    LV_EVENT_FLUSH_FINISH,
    LV_EVENT_OTHER,
} lv_event_t;
typedef struct { unsigned unused; } lv_display_t;
static lv_display_t s_display;
static struct { uint64_t draw_us, wait_us; } s_st;
static int64_t s_now, s_frame_start, s_draw_start, s_wait_start;
static unsigned s_st_lock, s_lock_depth;
static orb_display_response_t s_response;
static uint32_t s_frame_response_token;
static bool s_frame_response_settled;
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
static orb_display_accounting_t s_accounting_data;
static orb_display_accounting_t *s_accounting = &s_accounting_data;
static int64_t s_update_start, s_refresh_start, s_iteration_start;
static bool s_update_final_queued;
#endif
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
static bool s_double_buffered;
static void select_draw_buffers(void) {}
#endif
#define portENTER_CRITICAL(lock) do { CHECK((lock) == &s_st_lock); \
    CHECK(!s_lock_depth); s_lock_depth++; } while (0)
#define portEXIT_CRITICAL(lock) do { CHECK((lock) == &s_st_lock); \
    CHECK(s_lock_depth == 1); s_lock_depth--; } while (0)

static int64_t esp_timer_get_time(void) { return s_now; }
static lv_event_t lv_event_get_code(lv_event_t *e) { return *e; }
static lv_display_t *lv_event_get_current_target(lv_event_t *e) { (void)e; return &s_display; }
/* The production exact-union helper is exercised by test_cadence.py. This
 * fixture isolates timing accounting but still verifies its display target. */
static void cadence_damage(const lv_display_t *display) { CHECK(display == &s_display); }
#include "orb_render_accounting_impl.inc"

static void event(lv_event_t kind, int64_t now)
{
    s_now = now;
    s_cadence.active = true;
    on_render(&kind);
    CHECK(!s_lock_depth);
}

static void reset(void)
{
    s_st.draw_us = s_st.wait_us = 0;
    s_now = s_frame_start = s_draw_start = s_wait_start = 0;
    s_lock_depth = 0;
}

static void single_buffer(void)
{
    reset();
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
    s_double_buffered = false;
#endif
    event(LV_EVENT_RENDER_START, 100);
    CHECK(s_frame_start == 100);
    event(LV_EVENT_FLUSH_WAIT_START, 101);
    event(LV_EVENT_FLUSH_WAIT_FINISH, 110);
    event(LV_EVENT_FLUSH_START, 130);
    CHECK(s_st.draw_us == 20 && s_st.wait_us == 9);
    event(LV_EVENT_FLUSH_FINISH, 132);
    event(LV_EVENT_FLUSH_WAIT_START, 133);
    event(LV_EVENT_FLUSH_WAIT_FINISH, 140);
    event(LV_EVENT_FLUSH_START, 160);
    event(LV_EVENT_FLUSH_FINISH, 162);
    CHECK(s_st.draw_us == 40 && s_st.wait_us == 16);
    event(LV_EVENT_OTHER, 163);
    CHECK(s_st.draw_us == 40 && s_st.wait_us == 16);
}

#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
static void double_buffer(void)
{
    reset();
    s_double_buffered = true;
    event(LV_EVENT_RENDER_START, 100);
    event(LV_EVENT_FLUSH_WAIT_START, 130);
    CHECK(s_st.draw_us == 30 && !s_st.wait_us);
    event(LV_EVENT_FLUSH_WAIT_FINISH, 140);
    event(LV_EVENT_FLUSH_START, 141);
    event(LV_EVENT_FLUSH_FINISH, 142);
    CHECK(s_st.draw_us == 30 && s_st.wait_us == 10);
    event(LV_EVENT_FLUSH_WAIT_START, 160);
    event(LV_EVENT_FLUSH_WAIT_FINISH, 165);
    event(LV_EVENT_FLUSH_START, 166);
    event(LV_EVENT_FLUSH_FINISH, 167);
    /* Second draw starts after queue completion, not after the previous wait. */
    CHECK(s_st.draw_us == 48 && s_st.wait_us == 15);
    event(LV_EVENT_RENDER_START, 200);
    event(LV_EVENT_FLUSH_WAIT_START, 225);
    event(LV_EVENT_FLUSH_WAIT_FINISH, 230);
    event(LV_EVENT_FLUSH_START, 231);
    event(LV_EVENT_FLUSH_FINISH, 232);
    CHECK(s_st.draw_us == 73 && s_st.wait_us == 20);
}
#endif

static void whole_updates(void)
{
    orb_display_response_t response = { .token=2, .input_us=100,
        .x1=100, .y1=40, .x2=350, .y2=100, .causal=true };
    orb_response_complete(&response, 1, true, 0, 0, 465, 465, 200, true, true);
    CHECK(!response.first_band_us && !response.first_frame_us && !response.settled_frame_us);
    orb_response_complete(&response, 2, false, 0, 0, 465, 20, 201, true, false);
    CHECK(!response.first_band_us);
    orb_response_complete(&response, 2, false, 0, 21, 465, 83, 202, false, false);
    CHECK(!response.first_band_us);
    orb_response_complete(&response, 2, false, 0, 21, 465, 83, 203, true, false);
    CHECK(response.first_band_us == 203 && !response.first_frame_us);
    orb_response_complete(&response, 2, false, 0, 400, 465, 465, 220, true, true);
    CHECK(response.first_frame_us == 220 && !response.settled_frame_us);
    orb_response_complete(&response, 2, true, 0, 400, 465, 465, 250, true, false);
    CHECK(!response.settled_frame_us);
    orb_response_complete(&response, 2, true, 0, 400, 465, 465, 280, true, true);
    CHECK(response.first_band_us == 203 && response.first_frame_us == 220 && response.settled_frame_us == 280);
    CHECK(sizeof(response) <= 64);
    orb_display_accounting_t stats = {0};
    int64_t previous = 0;
    orb_accounting_complete(&stats, &previous, 1000, 15000, true);
    CHECK(stats.completed == 1 && stats.whole_update_us == 14000);
    CHECK(stats.whole_update[14] == 1 && previous == 15000);
    orb_accounting_complete(&stats, &previous, 16000, 49000, true);
    CHECK(stats.completion_interval[34] == 1);
    orb_accounting_complete(&stats, &previous, 50000, 51000, false);
    CHECK(stats.dropped == 1 && stats.completed == 2 && previous == 49000);
    orb_accounting_complete(&stats, &previous, 52000, 300000, true);
    CHECK(stats.whole_update[127] == 1 && stats.completion_interval[127] == 1);
    for (unsigned i = 0; i < 20; i++) orb_accounting_band(&stats,
        (orb_display_band_trace_t){ .draw_start_us=i, .bytes=100+i, .ok=i != 19 });
    CHECK(stats.band_sequence == 20 && stats.bands[19 % ORB_DISPLAY_BAND_TRACE].bytes == 119);
    CHECK(!stats.bands[19 % ORB_DISPLAY_BAND_TRACE].ok);
    /* TE counters add 80 PSRAM bytes; never place phase receipts on UI stacks. */
    CHECK(sizeof(stats) <= 1680);
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    s_iteration_start = 10;
    event(LV_EVENT_REFR_START, 30);
    event(LV_EVENT_RENDER_START, 100);
    CHECK(s_accounting->scheduled == 1 && s_accounting->preparation_us == 90);
    CHECK(s_accounting->scene_prepare_us == 20 && s_update_start == 10);
    s_update_final_queued = true;
    event(LV_EVENT_REFR_READY, 150);
    CHECK(!s_accounting->dropped);
    s_iteration_start = 200;
    event(LV_EVENT_REFR_START, 220);
    event(LV_EVENT_REFR_READY, 230);
    CHECK(s_accounting->scheduled == 1 && !s_accounting->dropped);
    event(LV_EVENT_RENDER_START, 240);
    event(LV_EVENT_REFR_READY, 250);
    CHECK(s_accounting->scheduled == 2 && s_accounting->dropped == 1);
    s_accounting = NULL; /* Optional diagnostics OOM leaves normal events safe. */
    event(LV_EVENT_REFR_START, 300);
    event(LV_EVENT_RENDER_START, 320);
    event(LV_EVENT_REFR_READY, 350);
    s_accounting = &s_accounting_data;
    CHECK(s_accounting->scheduled == 2 && s_accounting->dropped == 1);
#endif
}

int main(void)
{
    whole_updates();
    single_buffer();
#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER
    double_buffer();
#endif
    printf("render accounting (double-buffer option %d): %u checks passed\n",
           CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER, s_checks);
    return 0;
}
