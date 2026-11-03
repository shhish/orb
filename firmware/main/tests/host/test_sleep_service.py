"""Exercise production sleep UI admission and deadline-service ordering."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    main_dir = Path(__file__).resolve().parents[2]
    source = (main_dir / "main.c").read_text()
    start = source.index("static void ui_tick(lv_timer_t *timer)")
    stop = source.index("    static int64_t last_gravity;", start)
    prefix = source[start:stop] + "    s_normal_ticks++;\n}\n"
    service = (main_dir / "orb_power_services.c").read_text()
    start = service.index("int64_t orb_power_services_deadline(int64_t now)")
    stop = service.index("\n}", start) + 2
    fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
typedef void lv_timer_t;
typedef struct { bool attention; } orb_shell_snapshot_t;
typedef struct { unsigned state; } orb_power_snapshot_t;
typedef struct { uint32_t generation; } orb_time_snapshot_t;
#define ORB_POWER_SLEEP 3
#define ORB_POWER_ACTIVE 0
#define portENTER_CRITICAL(lock) do { (void)(lock); assert(!s_locked); s_locked = true; } while (0)
#define portEXIT_CRITICAL(lock) do { (void)(lock); assert(s_locked); s_locked = false; } while (0)
static int s_lock;
static void *s_shell = (void *)1;
static bool s_locked, s_sleep = true;
static unsigned s_power_state = ORB_POWER_SLEEP;
static unsigned s_calm_polls, s_normal_ticks, s_main_wakes, s_display_wakes;
typedef struct { bool view_dirty, button_activity; unsigned button_count; orb_shell_snapshot_t view; } mailbox_t;
static mailbox_t s_box;
static bool orb_display_sleep_poll(void) { return s_sleep; }
static void orb_shell_power_poll(void *shell, const orb_shell_snapshot_t *status) {
    assert(shell == s_shell && !s_locked); s_calm_polls++;
    if (status && status->attention) s_power_state = ORB_POWER_ACTIVE;
}
static orb_power_snapshot_t orb_shell_power_snapshot(void *shell) {
    assert(shell == s_shell); return (orb_power_snapshot_t){.state = s_power_state};
}
static void orb_display_set_sleep_poll(bool eligible, void (*poll)(void)) {
    assert(!eligible && !poll); s_sleep = eligible;
}
static void wake_main(void) { s_main_wakes++; }
static unsigned s_stage, s_full_polls, s_deadline_queries;
static int64_t s_timer_due = 1434500, s_reminder_due = INT64_MAX, s_last_poll, s_wall_us;
static uint32_t s_generation = 1;
static bool s_timer_rang, s_reminder_rang;
static int fake_gettimeofday(struct timeval *wall, void *zone) {
    assert(!zone && !s_stage); s_stage = 1;
    wall->tv_sec = s_wall_us / 1000000; wall->tv_usec = s_wall_us % 1000000;
    return 0;
}
#define gettimeofday fake_gettimeofday
static void orb_time_poll(int64_t now, time_t wall) {
    assert(s_stage == 1 && wall == s_wall_us / 1000000); s_stage = 2;
    s_last_poll = now; s_full_polls++;
    if (!s_timer_rang && now >= s_timer_due) { s_timer_rang = true; s_generation++; }
    if (!s_reminder_rang && now >= s_reminder_due) { s_reminder_rang = true; s_generation++; }
}
static void orb_time_snapshot(int64_t now, orb_time_snapshot_t *out) {
    assert(s_stage == 2 && now == s_last_poll); s_stage = 3;
    out->generation = s_generation;
}
static void orb_display_wake(void) { assert(s_stage == 3); s_display_wakes++; }
static int64_t orb_time_next_deadline_us(int64_t now, int64_t wall_us) {
    assert((s_stage == 1 || s_stage == 3) && now >= s_last_poll && wall_us == s_wall_us);
    s_deadline_queries++;
    int64_t timer = s_timer_rang ? INT64_MAX : s_timer_due;
    int64_t reminder = s_reminder_rang ? INT64_MAX : s_reminder_due;
    return timer < reminder ? timer : reminder;
}
"""
    fixture += prefix + "\n" + service[start:stop]
    fixture += r"""
static void reset_ui(void) {
    s_box = (mailbox_t){0}; s_sleep = true; s_power_state = ORB_POWER_SLEEP;
    s_calm_polls = s_normal_ticks = s_main_wakes = 0;
}
static int64_t service_at(int64_t now) {
    assert(!s_stage);
    s_wall_us = INT64_C(1793484000750000) + now;
    int64_t next = orb_power_services_deadline(now);
    assert(s_stage == 1 || s_stage == 3); s_stage = 0;
    return next;
}
int main(void) {
    reset_ui(); s_box.view_dirty = true;
    ui_tick(NULL);
    assert(s_calm_polls == 1 && !s_normal_ticks && s_sleep && s_box.view_dirty);
    /* The electrical level can be released before main runs. Its queued
     * physical DOWN/UP still must reach the ordinary button/consumption path. */
    reset_ui(); s_box.button_count = 2; s_box.button_activity = true;
    ui_tick(NULL);
    assert(s_normal_ticks == 1 && !s_sleep && s_main_wakes == 1);
    reset_ui(); s_power_state = ORB_POWER_ACTIVE;
    ui_tick(NULL);
    assert(s_normal_ticks == 1 && !s_sleep && s_main_wakes == 1);
    reset_ui(); s_box.view_dirty = s_box.view.attention = true;
    ui_tick(NULL);
    assert(s_normal_ticks == 1 && !s_sleep && s_box.view_dirty);
    assert(service_at(1000000) == s_timer_due && s_display_wakes == 1 && s_full_polls == 1);
    for (int64_t now = 1100000; now <= 1400000; now += 100000) {
        assert(service_at(now) == s_timer_due);
        assert(s_full_polls == 1 && s_display_wakes == 1);
    }
    assert(service_at(s_timer_due) == s_timer_due + 1000000);
    assert(s_timer_rang && s_display_wakes == 2 && s_full_polls == 2);
    /* A newly installed schedule is queried each IMU slot, so a deadline
     * earlier than the cached civil-service poll still preempts that wait. */
    s_reminder_due = 1700000;
    assert(service_at(1500000) == s_reminder_due && s_full_polls == 2);
    assert(service_at(1600000) == s_reminder_due && s_full_polls == 2);
    assert(service_at(s_reminder_due) == 2700000);
    assert(s_reminder_rang && s_display_wakes == 3 && s_full_polls == 3);
    for (int64_t now = 1800000; now < 2700000; now += 100000)
        assert(service_at(now) == 2700000 && s_full_polls == 3);
    assert(service_at(2700000) == 3700000 && s_full_polls == 4 && s_display_wakes == 3);
    assert(s_deadline_queries >= 19);
    puts("sleep service: mailbox/contact admission,1Hz civil polling, exact timer/reminder deadlines and fractional wall time passed");
}
"""
    with tempfile.TemporaryDirectory(prefix="orb-sleep-service-") as temporary:
        path = Path(temporary) / "service.c"
        path.write_text(fixture)
        binary = path.with_suffix("")
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-fsanitize=address,undefined",
                str(path),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
