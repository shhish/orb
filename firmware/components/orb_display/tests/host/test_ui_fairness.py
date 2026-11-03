"""Exercise the production UI wait and lock handoff with virtual precise timers.

The 100/1000 Hz scheduler cases prove no mandatory tick is charged to a
healthy timer path. Wall-clock USB, timer dispatch, DMA and touch require board
measurements; this model does not claim real device FPS or console latency.
"""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[5]
    source = (root / "firmware/components/orb_display/orb_display.c").read_text()
    helper = source.split("static void ui_wake(void *arg)", 1)[1]
    helper = "static void ui_wake(void *arg)" + helper.split("\nstatic void ui_task", 1)[0]
    block = source.split("    for (;;) {\n#if CONFIG_ORB_DISPLAY_SCROLL_TRACE", 1)[1]
    block = (
        "#if CONFIG_ORB_DISPLAY_SCROLL_TRACE"
        + block.split("\n    }\n}\n\nesp_err_t orb_display_start", 1)[0]
    )
    start = source.index("void orb_display_wake_service(void)")
    stop = source.index("\n}", start) + 2
    service = source[start:stop]
    fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include "orb_display_slow.h"
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef void *esp_timer_handle_t;
static unsigned s_tick_ms, s_next, s_now, s_requested, s_max_latency;
static unsigned s_commands, s_delays, s_precise, s_pipeline_lock, s_deadline, s_work;
static bool s_held, s_notified, s_fail_timer, s_panel_on = true;
static void *s_power_timer;
static atomic_bool s_external_wake, s_service_wake;
static unsigned s_service_notifications;
static void orb_display_wake(void) { s_service_notifications++; atomic_store(&s_external_wake, true); }
static bool s_sleep_eligible;
static bool s_calm_wake;
static unsigned s_calm_polls, s_touch_services, s_timer_calls;
static bool orb_display_sleep_poll(void) { return s_sleep_eligible; }
static void orb_display_touch_service(void) { s_touch_services++; }
static void calm_poll(void) {
    s_requested = s_now; s_calm_polls++;
    if (s_calm_wake) { s_sleep_eligible = false; s_panel_on = true; }
}
static void (*s_sleep_poll_callback)(void) = calm_poll;
static void lv_timer_ready(void *timer) { (void)timer; }
static struct { bool active; uint64_t refresh_us; } s_cadence;
static orb_display_phase_clock_t s_phase_clock;
static void cadence_timer(void (*callback)(void *), bool entering) { (void)callback; (void)entering; }
static void lv_timer_set_observer_cb(void (*observer)(void (*)(void *), bool)) { (void)observer; }
static int64_t esp_timer_get_time(void) { return s_now; }
static int64_t cadence_loop_begin(void) { return 0; }
static uint32_t cadence_animation_before(void) { return 0; }
static void cadence_loop_handled(int64_t a, int64_t b, uint32_t c, uint64_t d) { (void)a; (void)b; (void)c; (void)d; }
static void cadence_wait(int64_t a, uint32_t b) { (void)a; (void)b; }
static struct { bool redraw; } s_pipeline;
static esp_timer_handle_t wake = (void *)1;
#define pdMS_TO_TICKS(ms) ((ms) / s_tick_ms)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define ESP_OK 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define SCROLL_TRACE(kind, at, a, b) ((void)0)
static void lv_lock(void) { assert(!s_held); s_held = true; }
static void lv_unlock(void) { assert(s_held); s_held = false; }
static uint32_t lv_timer_handler(void) {
    assert(s_held);
    s_timer_calls++;
    s_requested = s_now;
    s_now += s_work;
    return s_next;
}
static void *lv_screen_active(void) { return &s_pipeline; }
static void lv_obj_invalidate(void *obj) { assert(s_held && obj == &s_pipeline); }
static void try_double_buffer(void) { assert(s_held); }
static void handoff(unsigned elapsed_us) {
    assert(!s_held && elapsed_us > 0 && elapsed_us <=
           (s_sleep_eligible && !s_panel_on ? 1000000u : s_panel_on ? 10000u : 100000u));
    /* A waiting console request can acquire unlocked LVGL during a genuine
     * blocked interval; unlike taskYIELD this admits lower priority tasks. */
    unsigned latency = s_now - s_requested;
    if (latency > s_max_latency) s_max_latency = latency;
    s_commands++;
    s_now += elapsed_us;
}
static int esp_timer_start_once(esp_timer_handle_t timer, uint32_t us) {
    assert(timer == wake && !s_held && us > 0 && us <= (s_panel_on ? 10000u : 100000u) && !s_notified);
    if (s_fail_timer) return -1;
    s_deadline = us;
    return ESP_OK;
}
static void esp_timer_stop(esp_timer_handle_t timer) { assert(timer == wake); }
static void xTaskNotifyGive(TaskHandle_t task) {
    assert(task == wake && !s_held && !s_notified); s_notified = true;
}
static unsigned ulTaskNotifyTake(int clear, unsigned timeout);
static void vTaskDelay(TickType_t ticks) {
    assert(ticks > 0 && s_fail_timer);
    s_delays++;
    handoff(ticks * s_tick_ms * 1000);
}
"""
    fixture += helper + "\n" + service
    fixture += r"""
static unsigned ulTaskNotifyTake(int clear, unsigned timeout) {
    assert(clear == pdTRUE && timeout == portMAX_DELAY && !s_held);
    s_precise++;
    if (s_sleep_eligible && !s_panel_on) {
        assert(!s_deadline);
        handoff(1000000);
        return 1;
    }
    handoff(s_deadline);
    ui_wake(wake);
    assert(s_notified); s_notified = false;
    return 1;
}
static void iteration(void) {
"""
    fixture += block
    fixture += r"""
}
int main(void) {
    unsigned deadlines[] = {0, 1, 5, 9, 10, 20, UINT32_MAX};
    for (s_tick_ms = 1; s_tick_ms <= 10; s_tick_ms += 9) {
        for (unsigned i = 0; i < sizeof(deadlines) / sizeof(deadlines[0]); i++) {
            s_next = deadlines[i]; s_work = 35000;
            for (unsigned frame = 0; frame < 1000; frame++) {
                unsigned before = s_now;
                s_pipeline.redraw = frame % 2;
                iteration();
                unsigned expected = s_next ? (s_next > 10 ? 10000 : s_next * 1000) : 100;
                assert(s_now - before == s_work + expected);
                assert(!s_held && !s_pipeline.redraw && s_delays == (s_tick_ms == 1 ? 0 : 1));
            }
        }
        /* A 7ms handler with a real 9ms deadline fits a 16ms period, including
         * at 100Hz. The old unconditional tick would make this at least17ms. */
        s_work = 7000; s_next = 9;
        for (unsigned frame = 0; frame < 1000; frame++) {
            unsigned before = s_now; iteration(); assert(s_now - before == 16000);
        }
        s_fail_timer = true; s_next = 0; iteration();
        s_fail_timer = false;
    }
    assert(s_precise == 16000 && s_delays == 2 && s_commands == 16002);
    assert(s_max_latency == 35000);
    s_panel_on = false; s_next = 0;
    unsigned before = s_now; iteration(); assert(s_now - before == s_work + 100000);
    s_next = 1; before = s_now; iteration(); assert(s_now - before == s_work + 100000);
    unsigned timer_calls = s_timer_calls;
    s_sleep_eligible = true; s_deadline = 0;
    before = s_now; iteration(); assert(s_now - before == 1000000);
    assert(s_calm_polls == 1 && s_touch_services == 1 && s_timer_calls == timer_calls);
    orb_display_wake_service(); assert(s_service_notifications == 1);
    before = s_now; iteration(); assert(s_now - before == s_work + 1000000);
    assert(s_calm_polls == 2 && s_touch_services == 2 && s_timer_calls == timer_calls + 1);
    assert(!atomic_load(&s_service_wake) && s_sleep_eligible && !s_panel_on);
    before = s_now; iteration(); assert(s_now - before == 1000000);
    assert(s_calm_polls == 3 && s_touch_services == 3 && s_timer_calls == timer_calls + 1);
    s_calm_wake = true; before = s_now;
    iteration(); assert(s_now - before == s_work + 1000);
    assert(s_calm_polls == 4 && s_touch_services == 4 && s_timer_calls == timer_calls + 2);
    assert(s_panel_on && !s_sleep_eligible);
    s_panel_on = false;
    s_sleep_eligible = false;
    before = s_now; iteration(); assert(s_now - before == s_work + 100000);
    assert(s_timer_calls == timer_calls + 3);
    printf("UI fairness: %u precise handoffs, %u fallback ticks, virtual command lock wait <=%uus; 100/1000Hz ticks\n",
           s_precise, s_delays, s_max_latency);
}
"""
    with tempfile.TemporaryDirectory(prefix="orb-ui-fairness-") as temporary:
        directory = Path(temporary)
        path = directory / "fairness.c"
        path.write_text(fixture)
        binary = directory / "fairness"
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-O1",
                "-g",
                "-fsanitize=address,undefined",
                f"-I{root / 'firmware/components/orb_display/include'}",
                str(path),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
