"""Run production button/console waits and voice leases with deterministic clocks."""

import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    main_dir = Path(__file__).resolve().parents[2]
    components = main_dir.parent / "components"
    source = Path(os.environ.get("ORB_TEST_MAIN", main_dir / "main.c")).read_text()
    buttons = source[
        source.index("static void publish_button_events(") : source.index(
            "static void button_edge_isr("
        )
    ]
    schedule = re.search(r"next_buttons = now \+ .*?poll_buttons\(now\).*?;", source).group(0)
    devtools = Path(
        os.environ.get("ORB_TEST_DEVTOOLS", components / "orb_devtools/orb_devtools.c")
    ).read_text()
    start = devtools.index(
        "        orb_light_sleep_status_t sleep;", devtools.index("static void devtools_task(")
    )
    wait = devtools[start : devtools.index("\n    }", start)]
    services = (main_dir / "orb_power_services.c").read_text()
    radio_lease = re.search(r"voice_active \|= (.*?);", services, re.S).group(1)
    sleep_lease = re.search(r"bool busy = (.*?);", services, re.S).group(1)
    fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "orb_idle_policy.h"
#include "orb_model.h"
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int s_lock;
static orb_buttons_t s_buttons;
static void *s_ui_timer;
static int64_t s_now;
static unsigned s_wakes;
static struct {
    bool consume_pwr, button_activity;
    unsigned injection_count, button_count, edge_count;
    orb_button_event_t injections[ORB_BUTTON_EVENTS], button_events[ORB_BUTTON_EVENTS];
    orb_button_edge_t button_edges[ORB_BUTTON_HISTORY];
    orb_idle_policy_t idle_policy;
} s_box;
typedef struct { uint64_t at_us; unsigned buttons; } orb_board_button_edge_t;
static orb_board_button_edge_t s_edge;
static bool s_pending;
static unsigned orb_board_buttons(void) { return 0; }
static bool orb_board_button_edge(orb_board_button_edge_t *edge) {
    if (!s_pending) return false;
    *edge = s_edge; s_pending = false; return true;
}
static int64_t esp_timer_get_time(void) { return s_now; }
static void lv_lock(void) {}
static void lv_unlock(void) {}
static void lv_timer_ready(void *timer) { assert(timer == s_ui_timer); }
static void orb_display_wake(void) { s_wakes++; }
typedef struct { bool eligible; } orb_light_sleep_status_t;
static bool s_eligible, s_notify;
static unsigned s_wait, s_elapsed;
static void orb_light_sleep_status(orb_light_sleep_status_t *sleep) { sleep->eligible = s_eligible; }
static void ulTaskNotifyTake(int clear, unsigned wait) {
    assert(clear == pdTRUE); s_wait = wait; s_elapsed = s_notify ? 0 : wait; s_notify = false;
}
bool orb_model_get_voice(orb_voice_state_t *voice) { (void)voice; return true; }
"""
    fixture += buttons + "\nstatic void console_wait(void) {\n" + wait + "\n}\n"
    fixture += f"static int64_t scheduled_buttons(int64_t now) {{ int64_t next_buttons; {schedule} return next_buttons; }}\n"
    fixture += f"static bool radio_lease(orb_voice_state_t voice) {{ return {radio_lease}; }}\n"
    fixture += f"static bool sleep_lease(orb_voice_state_t voice) {{ return {sleep_lease}; }}\n"
    fixture += r"""
static uint32_t poll_at(uint64_t ms) { s_now = (int64_t)ms * 1000; return poll_buttons(s_now); }
static uint32_t edge_at(uint64_t ms, unsigned mask) {
    s_edge = (orb_board_button_edge_t){.at_us = ms * 1000, .buttons = mask};
    s_pending = true; return poll_at(ms);
}
int main(void) {
    orb_buttons_init(&s_buttons); s_box.idle_policy.minimal = true;
    s_box.idle_policy.poll_ms = 100;
    assert(poll_at(100) == UINT32_MAX && !s_wakes);
    assert(scheduled_buttons(s_now) == s_now + INT64_C(4294967295000));
    /* No second 10 Hz timer, even after the sensor clock has drifted. */
    assert(poll_at(10001) == UINT32_MAX && !s_wakes);
    s_box.idle_policy.face_down = true; s_box.idle_policy.poll_ms = 1000;
    assert(poll_at(11003) == UINT32_MAX);
    s_box.consume_pwr = true;
    assert(edge_at(12000, 1) == ORB_BUTTON_DEBOUNCE_MS);
    assert(poll_at(12030) == UINT32_MAX);
    assert(s_box.button_count == 1 && s_box.button_events[0].gesture == ORB_BUTTON_DOWN);
    assert(edge_at(12040, 0) == ORB_BUTTON_DEBOUNCE_MS);
    assert(poll_at(12070) == UINT32_MAX);
    assert(s_box.button_count == 2 && s_box.button_events[1].gesture == ORB_BUTTON_UP);
    poll_at(13000); assert(s_box.button_count == 2); /* Whole wake contact consumed. */
    s_box.button_activity = false; s_box.consume_pwr = false;
    assert(edge_at(14000, 2) == ORB_BUTTON_DEBOUNCE_MS);
    assert(poll_at(14030) == ORB_BUTTON_BOOT_HOLD_MS);
    assert(poll_at(14379) == 1);
    assert(poll_at(14380) == UINT32_MAX);
    assert(s_box.button_events[s_box.button_count - 1].gesture == ORB_BUTTON_HOLD);
    assert(edge_at(14400, 0) == ORB_BUTTON_DEBOUNCE_MS);
    assert(poll_at(14430) == UINT32_MAX);
    s_box.idle_policy.minimal = false;
    assert(poll_at(14500) == 50);
    s_eligible = true; console_wait(); assert(s_wait == 5000 && s_elapsed == 5000);
    s_notify = true; console_wait(); assert(s_wait == 5000 && !s_elapsed);
    s_eligible = false; console_wait(); assert(s_wait == 20 && s_elapsed == 20);
    for (unsigned phase = ORB_VOICE_IDLE; phase <= ORB_VOICE_ERROR; phase++) {
        orb_voice_state_t voice = {.phase = phase};
        bool expected = phase != ORB_VOICE_IDLE;
        assert(radio_lease(voice) == expected && sleep_lease(voice) == expected);
    }
    puts("sleep waits: event-only buttons, exact contact deadlines, console notification and pending voice leases passed");
}
"""
    with tempfile.TemporaryDirectory(prefix="orb-sleep-waits-") as temporary:
        path = Path(temporary) / "waits.c"
        path.write_text(fixture)
        binary = path.with_suffix("")
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-I" + str(main_dir),
                "-I" + str(components / "orb_power/include"),
                "-I" + str(components / "orb_buttons/include"),
                "-I" + str(components / "orb_model/include"),
                str(path),
                str(components / "orb_buttons/orb_buttons.c"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
