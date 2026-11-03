"""Execute the real queued wake transaction with deterministic timeout races."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

component = Path(__file__).resolve().parents[2]
source = (component / "orb_devtools.c").read_text()
start = source.index("static SemaphoreHandle_t s_wake_done;")
end = source.index("static void glide(", start)
actual = source[start:end]
prelude = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define LV_RESULT_OK 0
#define ORB_SHELL_FACE 0
#define ORB_SHELL_HOME 3
#define ORB_SHELL_AGENTS 4
typedef int SemaphoreHandle_t;
typedef int orb_shell_surface_t;
static int s_shell_storage;
static void *s_shell = &s_shell_storage;
static bool s_locked, s_on_ui, s_pending, s_guard_accept, s_queue_fail, s_timeout, s_late;
static void (*s_callback)(void *);
static unsigned s_service_wakes;
static unsigned s_tokens, s_wakes, s_navigations, s_cancels, s_checks, s_surface;
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr,"wake:%d: %s\n",__LINE__,#value); exit(1); } } while(0)
static void orb_display_wake_service(void) {
    CHECK(s_locked && s_pending); ++s_service_wakes;
}
static void dispatch_ui(void) {
    CHECK(!s_on_ui && !s_locked && s_pending);
    s_pending = false; s_on_ui = s_locked = true;
    s_callback(NULL);
    s_on_ui = s_locked = false;
}
static void lv_lock(void) {
    CHECK(!s_locked);
    if (s_late && s_pending) { s_late = false; dispatch_ui(); }
    s_locked = true;
}
static void lv_unlock(void) { CHECK(s_locked); s_locked = false; }
static int lv_async_call(void (*callback)(void *), void *context) {
    CHECK(s_locked && !s_on_ui && !context && !s_pending);
    if (s_queue_fail) return 1;
    s_callback = callback; s_pending = true; return LV_RESULT_OK;
}
static void lv_async_call_cancel(void (*callback)(void *), void *context) {
    CHECK(s_locked && callback == s_callback && !context);
    s_pending = false; ++s_cancels;
}
static int xSemaphoreTake(int semaphore, unsigned wait) {
    CHECK(semaphore == 1);
    if (wait) {
        CHECK(!s_locked && !s_on_ui && wait == 5000);
        if (s_timeout) return 0;
        if (s_pending) dispatch_ui();
    }
    if (!s_tokens) return 0;
    --s_tokens; return pdTRUE;
}
static void xSemaphoreGive(int semaphore) {
    CHECK(semaphore == 1 && s_on_ui && s_locked && !s_tokens); ++s_tokens;
}
static bool orb_devtools_input_wake(void *shell) {
    CHECK(shell == s_shell && s_on_ui && s_locked);
    ++s_wakes; return s_guard_accept;
}
static bool orb_devtools_input_goto(void *shell, int surface) {
    if (!orb_devtools_input_wake(shell)) return false;
    ++s_navigations; s_surface = (unsigned)surface; return true;
}
"""
tests = r"""
int main(void) {
    /* No initialized gate and allocation/queue failure must leave no callback. */
    CHECK(!wake_display()); CHECK(!s_pending && !s_wakes);
    s_wake_done = 1; s_queue_fail = true;
    CHECK(!wake_display()); CHECK(!s_pending && !s_tokens && !s_wakes && !s_service_wakes);
    s_queue_fail = false; s_guard_accept = true;
    CHECK(wake_display()); CHECK(s_wakes == 1 && !s_pending && !s_tokens && s_service_wakes == 1);
    s_guard_accept = false;
    CHECK(!wake_display()); CHECK(s_wakes == 2 && !s_navigations);
    CHECK(!request_wake(true, ORB_SHELL_HOME)); CHECK(!s_navigations);
    s_guard_accept = true;
    CHECK(request_wake(true, ORB_SHELL_HOME)); CHECK(s_navigations == 1 && s_surface == ORB_SHELL_HOME);
    /* Timeout before execution cancels queued work with no owner-state call. */
    unsigned before = s_wakes;
    s_timeout = true;
    CHECK(!wake_display());
    CHECK(!s_pending && !s_tokens && s_wakes == before && s_cancels == 1);
    /* A callback that wins the lock after wait expiry still cannot acknowledge
     * the timed-out command or leave a semaphore token for the next request. */
    s_late = true;
    CHECK(!request_wake(true, ORB_SHELL_AGENTS));
    CHECK(s_wakes == before + 1 && !s_pending && !s_tokens && s_cancels == 2);
    s_timeout = false; s_guard_accept = false;
    CHECK(!wake_display()); CHECK(!s_tokens && !s_pending);
    s_guard_accept = true;
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(wake_display()); CHECK(!s_tokens && !s_pending && !s_locked);
    }
    printf("devtools wake: %u actual-source checks; UI ownership, guard rejection, queue failure, timeout/cancel, late result and repeated cleanup passed\n", s_checks);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="orb-devtools-wake-") as temporary:
    directory = Path(temporary)
    harness = directory / "wake.c"
    harness.write_text(prelude + actual + tests)
    binary = directory / "wake"
    subprocess.run(
        [
            *shlex.split(os.environ.get("CC", "clang")),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-O1",
            "-g",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            str(harness),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
