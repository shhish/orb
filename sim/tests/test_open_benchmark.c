/* Execute production bench_open through real shell/timers. Panel completions
 * and RTOS waits are virtual: these checks cannot establish device latency. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "orb_display.h"
#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_open_source.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static orb_shell_t *s_shell;
static orb_display_response_t s_response;
static orb_display_counters_t s_counters;
static uint32_t s_token, s_frame_token;
static bool s_frame_settled, s_rendering, s_bench_active;
static bool s_fail_alloc, s_fail_drain, s_fail_transfer, s_suppress_render;
static unsigned s_pre_render_ticks, s_allocations;
/* This host run exercises the USB executor; Wi-Fi revocation is covered by
 * devtools host tests, but the extracted production handler retains its gate. */
static struct { bool active, failed; } s_wifi;
static bool wifi_current(void) { return true; }
static int s_done;
typedef int *SemaphoreHandle_t;
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)

static uint32_t tick(void) { return s_now; }
static int64_t esp_timer_get_time(void) { return (int64_t)s_now * 1000; }
static size_t heap_caps_get_minimum_free_size(int caps) { assert(caps == MALLOC_CAP_INTERNAL); return 0; }
static void *heap_caps_calloc(size_t n, size_t size, int caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_fail_alloc) return NULL;
    void *p = calloc(n, size);
    if (p) s_allocations++;
    return p;
}
static void heap_caps_free(void *p) { if (p) { assert(s_allocations); s_allocations--; free(p); } }
static SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &s_done; }
static void xSemaphoreGive(SemaphoreHandle_t sem) { *sem = 1; }
static int xSemaphoreTake(SemaphoreHandle_t sem, unsigned timeout);

/* The native fixture captures the same receipt, while the dedicated FIFO
 * regression covers USB backpressure and short-packet completion. */
static bool command_reply_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vprintf(format, args);
    va_end(args);
    return written >= 0;
}

void orb_display_counters(orb_display_counters_t *out) { *out = s_counters; }
void orb_display_accounting(orb_display_accounting_t *out) { memset(out, 0, sizeof(*out)); }
bool orb_display_accounting_active(void) { return false; }
bool orb_display_drain(void) { return !s_fail_drain; }
uint32_t orb_display_response_begin(int64_t input_us, const lv_area_t *a)
{
    s_response = (orb_display_response_t){.token = ++s_token, .input_us = input_us,
        .x1 = a->x1, .y1 = a->y1, .x2 = a->x2, .y2 = a->y2, .causal = true};
    s_frame_token = s_token;
    s_frame_settled = false;
    return s_token;
}
void orb_display_response_settled(uint32_t token) { assert(token == s_response.token); s_response.settled = true; }
void orb_display_response_snapshot(orb_display_response_t *out) { *out = s_response; }
void orb_display_response_end(uint32_t token) { if (token == s_response.token) s_response.token = 0; }

#include "devtools_open_benchmark.inc"

/* The real display's render observer precedes the benchmark observer; the
 * first flush and final completion carry that frame's immutable token. */
static void panel(lv_event_t *event)
{
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(event)) {
    case LV_EVENT_RENDER_START:
        s_rendering = true;
        s_frame_token = s_response.token;
        s_frame_settled = s_response.settled;
        break;
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *a = lv_event_get_param(event);
        orb_response_complete(&s_response, s_frame_token, s_frame_settled,
                              a->x1, a->y1, a->x2, a->y2, now, true, false);
        if (s_fail_transfer && s_frame_token) { s_counters.transfer_errors++; s_fail_transfer = false; }
        break;
    }
    case LV_EVENT_REFR_READY:
        if (!s_rendering) break;
        s_rendering = false;
        s_counters.frames++;
        orb_response_complete(&s_response, s_frame_token, s_frame_settled,
                              0, 0, 465, 465, now, true, true);
        break;
    default: break;
    }
}

static int xSemaphoreTake(SemaphoreHandle_t sem, unsigned timeout)
{
    unsigned end = s_now + timeout;
    while (!*sem && s_now < end) {
        /* Several benchmark ticks precede the 33 ms refresh, reproducing the
         * second-run race even though all source/history contracts are valid. */
        if (s_open && s_open->measured && !s_open->token && s_response.first_band_us) s_pre_render_ticks++;
        s_now += 5;
        orb_shell_poll(s_shell);
        if (s_suppress_render) lv_timer_pause(lv_display_get_refr_timer(lv_display_get_default()));
        lv_timer_handler();
    }
    if (!*sem) return 0;
    *sem = 0;
    return pdTRUE;
}

static void settle(void)
{
    for (unsigned i = 0; i < 60; i++) {
        s_now += 20;
        orb_shell_poll(s_shell);
        lv_timer_handler();
    }
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    assert(display);
    orb_ui_init(NULL);
    assert(orb_model_init());
    orb_model_load_demo();
    orb_model_set_connection(true, true);
    orb_shell_t *shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    assert(shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .usb = true, .brightness = 70};
    snprintf(status.clock_time, sizeof(status.clock_time), "10:09");
    orb_shell_update(shell, &status);
    lv_display_add_event_cb(display, panel, LV_EVENT_ALL, NULL);
    orb_devtools_set_shell(shell);
    settle();
    const char *names[] = {"apps", "play", "dot", "agents", "back"};
    const orb_shell_surface_t targets[] = {ORB_SHELL_APPS, ORB_SHELL_PLAY, ORB_SHELL_DOT,
                                         ORB_SHELL_AGENTS, ORB_SHELL_APPS};
    for (unsigned reduced = 0; reduced < 2; reduced++) {
        orb_ui_set_reduce_motion(reduced);
        for (unsigned surface = 0; surface < 5; surface++) {
            /* Face matches the live board's initial state. A pre-open target
             * also verifies restoration from an already-open destination. */
            for (unsigned saved = 0; saved < 2; saved++) {
                orb_shell_surface_t origin = saved ? targets[surface] : ORB_SHELL_FACE;
                orb_shell_navigate(shell, origin);
                settle();
                for (unsigned run = 0; run < 20; run++) {
                    assert(benchmark_open(names[surface]));
                    assert(!s_open && !s_allocations && !s_response.token);
                    assert(orb_open_source_settled(shell, origin));
                }
            }
        }
    }
    assert(s_pre_render_ticks > 0);
    assert(!benchmark_open("unknown"));
    s_fail_alloc = true;
    assert(!benchmark_open("apps"));
    s_fail_alloc = false;
    s_bench_active = true;
    assert(!benchmark_open("apps"));
    s_bench_active = false;
    s_fail_drain = true;
    assert(!benchmark_open("apps"));
    s_fail_drain = false;
    s_fail_transfer = true;
    assert(!benchmark_open("apps"));
    s_suppress_render = true;
    assert(!benchmark_open("apps"));
    s_suppress_render = false;
    lv_timer_resume(lv_display_get_refr_timer(display));
    settle();
    assert(!s_open && !s_allocations);
    assert(benchmark_open("apps")); /* Failure cleanup permits recovery. */
    orb_shell_destroy(shell);
    orb_glass_cache_clear();
    printf("open_benchmark: 400 consecutive opens, stale pre-render ticks=%u; failures and recovery passed\n",
           s_pre_render_ticks);
    return 0;
}
