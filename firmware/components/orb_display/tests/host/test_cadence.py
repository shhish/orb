"""Run production bench cadence counters against virtual time and animation ticks."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    component = Path(__file__).resolve().parents[2]
    source = (component / "orb_display.c").read_text()
    capture = source.split("static orb_display_cadence_t s_cadence;", 1)[1].split(
        "/* Fits a 44 pt", 1
    )[0]
    capture = "static orb_display_cadence_t s_cadence;" + capture
    fixture = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "orb_display_cadence.h"
#define LV_DEF_REFR_PERIOD 16
#define UI_PRIORITY 5
#define UI_CORE 1
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#define configUSE_TRACE_FACILITY 1
#define LV_INV_BUF_SIZE 32
#define LV_MAX(a, b) ((a) > (b) ? (a) : (b))
typedef struct { int32_t x1, y1, x2, y2; } lv_area_t;
typedef struct {
    lv_area_t inv_areas[LV_INV_BUF_SIZE];
    uint8_t inv_area_joined[LV_INV_BUF_SIZE];
    unsigned inv_p;
} lv_display_t;
#if configGENERATE_RUN_TIME_STATS
typedef uint32_t configRUN_TIME_COUNTER_TYPE;
typedef struct { configRUN_TIME_COUNTER_TYPE ulRunTimeCounter; } TaskStatus_t;
static configRUN_TIME_COUNTER_TYPE s_task_runtime;
#define pdFALSE 0
#define eReady 1
#define portGET_RUN_TIME_COUNTER_VALUE() ((configRUN_TIME_COUNTER_TYPE)s_now)
static void *xTaskGetCurrentTaskHandle(void) { return &s_task_runtime; }
static void vTaskGetInfo(void *task, TaskStatus_t *out, int highwater, int state) {
    assert(task == &s_task_runtime && !highwater && state == eReady);
    out->ulRunTimeCounter = s_task_runtime;
}
#endif
static int64_t s_now = 100;
static unsigned s_frequency = 240000000;
typedef struct { uint32_t period, last_run; bool paused; } lv_timer_t;
static lv_timer_t s_animation = {16, 0, false};
static int esp_clk_cpu_freq(void) { return s_frequency; }
static int64_t esp_timer_get_time(void) { return s_now; }
static lv_timer_t *lv_anim_get_timer(void) { return &s_animation; }
static uint32_t lv_tick_elaps(uint32_t then) { return (uint32_t)(s_now / 1000) - then; }
"""
    fixture += capture
    fixture += r"""
int main(void) {
    orb_display_cadence_t out;
    assert(!cadence_loop_begin());
    orb_display_cadence_begin();
    int64_t began = cadence_loop_begin();
    uint32_t previous = cadence_animation_before();
    s_now += 300; int64_t locked = s_now;
    s_now += 5000; s_animation.last_run = 5;
    s_cadence.refresh_us += 2000;
    cadence_loop_handled(began, locked, previous, 0);
    int64_t wait = s_now; s_now += 12000; cadence_wait(wait, 10);
    s_frequency = 80000000;
    orb_display_cadence_set_moving(true);
    began = cadence_loop_begin(); previous = cadence_animation_before();
    s_now += 100; locked = s_now;
    s_now += 6000; s_animation.last_run = 23;
    cadence_loop_handled(began, locked, previous, 2000);
    wait = s_now; s_now += 150; cadence_wait(wait, 0);
    s_now = 50000; cadence_animation_before();
    s_animation.paused = true; cadence_animation_before();
    assert(!s_cadence_animation_at);
    orb_display_cadence_snapshot(&out);
    assert(out.active && out.loops == 2 && out.lock_us == 400);
    assert(out.handler_us == 11000 && out.handler_max_us == 6000);
    assert(out.handler_outside_refresh_us == 9000 && out.refresh_us == 2000);
    assert(out.ui_cpu_available == !!configGENERATE_RUN_TIME_STATS && out.ui_priority == 5 && out.ui_core == 1);
    assert(out.timer_wait_us == 12000 && out.fairness_wait_us == 150);
    assert(out.wait_requested_us == 10100 && out.wait_overrun_us == 2050 && out.wait_max_us == 12000);
    assert(out.frequency_samples == 2 && out.frequency_below_max == 1);
    assert(out.frequency_min_mhz == 80 && out.frequency_max_mhz == 240);
    assert(out.moving_frequency_samples == 1 && out.moving_frequency_below_max == 1);
    assert(out.moving_frequency_min_mhz == 80 && out.moving_frequency_max_mhz == 80);
    assert(out.animation_ticks == 2 && out.animation_gap_max_us == 18100);
    assert(out.animation_late_max_ms == 11 && out.animation_period_ms == 16 && out.indev_period_ms == 16);
    lv_display_t display = {.inv_p = 3, .inv_areas = {{0, 0, 9, 9}, {5, 5, 14, 14}, {20, 0, 24, 4}}};
    cadence_damage(&display);
    assert(s_cadence.damage_frames == 1 && s_cadence.damage_pixels == 200 && s_cadence.damage_max_pixels == 200);
    display.inv_area_joined[1] = 1;
    cadence_damage(&display);
    assert(s_cadence.damage_pixels == 325 && s_cadence.damage_max_pixels == 200);
    display.inv_p = 0; cadence_damage(&display);
    assert(s_cadence.damage_frames == 2);
    /* Compare overlapping rectangles with a pixel oracle, including the full
     * 32-entry buffer and joined rectangles that must be ignored. */
    unsigned random = 7;
    for (unsigned sample = 0; sample < 200; sample++) {
        bool pixel[20][20] = {0};
        display.inv_p = sample % LV_INV_BUF_SIZE + 1;
        for (unsigned i = 0; i < display.inv_p; i++) {
            random = random * 1664525u + 1013904223u;
            lv_area_t *a = &display.inv_areas[i];
            a->x1 = random % 20; a->y1 = (random >> 8) % 20;
            a->x2 = a->x1 + (random >> 16) % (20 - a->x1);
            a->y2 = a->y1 + (random >> 24) % (20 - a->y1);
            display.inv_area_joined[i] = i % 4 == 3;
            if (display.inv_area_joined[i]) continue;
            for (int y = a->y1; y <= a->y2; y++) for (int x = a->x1; x <= a->x2; x++) pixel[y][x] = true;
        }
        unsigned expected = 0;
        for (unsigned y = 0; y < 20; y++) for (unsigned x = 0; x < 20; x++) expected += pixel[y][x];
        uint64_t before = s_cadence.damage_pixels;
        cadence_damage(&display);
        assert(s_cadence.damage_pixels - before == expected);
    }
#if configGENERATE_RUN_TIME_STATS
    s_task_runtime = 24950;
#endif
    orb_display_cadence_end();
    orb_display_cadence_snapshot(&out);
#if configGENERATE_RUN_TIME_STATS
    assert(out.ui_cpu_available && out.ui_cpu_pct == 50.0f);
#endif
    assert(!out.active && !cadence_loop_begin());
    s_now += 100000; cadence_loop_handled(1, 1, 0, 0); cadence_wait(1, 1);
    orb_display_cadence_t frozen; orb_display_cadence_snapshot(&frozen);
    assert(!memcmp(&out, &frozen, sizeof(out)));
    orb_display_cadence_begin(); orb_display_cadence_snapshot(&out);
    assert(out.active && !out.loops && !out.frequency_samples && !out.wait_overrun_us);
    s_animation.paused = false;
    s_animation.last_run = 0;
    s_now = 100000;
    cadence_loop_handled(s_now, s_now - 33000, 0, 0);
    assert(!s_cadence.handler_stalls);
    s_animation.last_run = 1;
    s_now += 33000;
    cadence_loop_handled(s_now, s_now - 33001, 0, 0);
    assert(s_cadence.handler_stalls == 1 && !s_cadence.animation_stalls);
    s_animation.last_run = 2;
    s_now += 33001;
    cadence_loop_handled(s_now, s_now, 1, 0);
    assert(s_cadence.animation_stalls == 1);
    /* Integrate the production cadence end with known exclusive phase times.
     * This also proves the bounded receipt resets with a new benchmark. */
    orb_display_cadence_begin();
    s_now = 200000;
    orb_display_phase_begin(&s_phase_clock, s_now);
    orb_display_phase_timer(&s_phase_clock, s_now, 0x42000020, true);
    s_now += 1000; orb_display_phase_mark(&s_phase_clock, s_now, 2);
    s_now += 15000; orb_display_phase_mark(&s_phase_clock, s_now, 4);
    s_now += 8000; orb_display_phase_mark(&s_phase_clock, s_now, 6);
    s_now += 10000; orb_display_phase_timer(&s_phase_clock, s_now, 0x42000020, false);
    cadence_loop_handled(200000, 200000, s_animation.last_run, 0);
    orb_display_cadence_snapshot(&out);
    assert(out.phase_us[0] == 1000 && out.phase_us[2] == 15000 && out.phase_us[4] == 8000);
    assert(out.phase_us[6] == 10000 && out.phase_max_us[2] == 15000);
    assert(out.handler_max_us == 34000 && out.slow[0].handler_us == 34000);
    assert(out.slow[0].timer_us == 34000 && out.slow[0].timer_callback == 0x42000020);
    assert(out.slow[0].sequence == 1 && !out.slow[1].handler_us);
    orb_display_cadence_begin();
    orb_display_cadence_snapshot(&out);
    assert(!out.phase_us[2] && !out.slow[0].handler_us && !s_phase_clock.active);
#if configGENERATE_RUN_TIME_STATS
    /* Both total and UI task runtime counters may wrap during a sample. */
    s_now = UINT32_MAX - 500;
    s_task_runtime = UINT32_MAX - 100;
    orb_display_cadence_begin();
    s_now += 1000; s_task_runtime += 500;
    orb_display_cadence_end(); orb_display_cadence_snapshot(&out);
    assert(out.ui_cpu_available && out.ui_cpu_pct == 50.0f);
    orb_display_cadence_begin();
    s_now += 1000; s_task_runtime += 2000;
    orb_display_cadence_end(); orb_display_cadence_snapshot(&out);
    assert(!out.ui_cpu_available && !out.ui_cpu_pct);
#endif
    printf("cadence (runtime=%d): lock/handler/wait/animation/frequency/runtime-wrap capture passed\n",
           configGENERATE_RUN_TIME_STATS);
}
"""
    with tempfile.TemporaryDirectory(prefix="orb-cadence-") as temporary:
        directory = Path(temporary)
        path = directory / "cadence.c"
        path.write_text(fixture)
        for runtime in (0, 1):
            binary = directory / f"cadence-{runtime}"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CC", "cc")),
                    "-std=c11",
                    f"-DconfigGENERATE_RUN_TIME_STATS={runtime}",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-O1",
                    "-g",
                    "-fsanitize=address,undefined",
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
