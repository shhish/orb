"""Run bounded production trace capture and histogram code with sanitizers."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    component = Path(__file__).resolve().parents[2]
    source = (component / "orb_display.c").read_text()
    capture = source.split("#if CONFIG_ORB_DISPLAY_SCROLL_TRACE", 1)[1].split(
        "\nvoid orb_display_cpu_snapshot", 1
    )[0]
    capture = "#if CONFIG_ORB_DISPLAY_SCROLL_TRACE" + capture
    fixture = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_display_scroll_trace.h"
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static unsigned s_locks, s_allocations;
static int64_t s_now = 100;
static bool s_fail;
#define portENTER_CRITICAL(lock) do { (void)(lock); assert(!s_locks++); } while (0)
#define portEXIT_CRITICAL(lock) do { (void)(lock); assert(s_locks-- == 1); } while (0)
static int64_t esp_timer_get_time(void) { return s_now; }
static void *heap_caps_calloc(size_t n, size_t size, unsigned caps) {
    assert(!s_locks && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_allocations++; return s_fail ? NULL : calloc(n, size);
}
"""
    fixture += capture
    fixture += r"""
int main(void) {
    orb_display_scroll_trace_t snapshot;
    orb_display_scroll_trace_event_t event;
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
    s_fail = true;
    assert(!orb_display_scroll_trace_begin());
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_COMPLETE, 100, 0, 0);
    orb_display_scroll_trace_snapshot(&snapshot);
    assert(!snapshot.available && !snapshot.active && !snapshot.count && s_allocations == 1);
    s_fail = false;
    assert(orb_display_scroll_trace_begin());
    assert(s_allocations == 2);
    /* An in-flight event timestamped on the other core before begin must not
     * contaminate the new trace's summary or unsigned compact timestamps. */
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_COMPLETE, 99, 0, 0);
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_LOOP_END, 99, 123, 0);
    orb_display_scroll_trace_snapshot(&snapshot);
    assert(!snapshot.count && !snapshot.completions && !snapshot.loops);
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_COMPLETE, 1000, 0, 0);
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_COMPLETE, 17500, 0, 0);
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_COMPLETE, 400000, 0, 0);
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_LOOP_END, 400100, 123, 10);
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_WAIT_END, 400200, 100, 2);
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_WAIT_END, 401200, 1000, 1);
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_WAIT_END, 401300, -1, 1);
    assert(orb_display_scroll_trace_read(0, &event));
    assert(event.kind == ORB_SCROLL_TRACE_COMPLETE && event.at_us == 1000 && !event.a && !event.b);
    assert(!orb_display_scroll_trace_read(7, &event));
    for (unsigned i = 7; i < ORB_SCROLL_TRACE_CAPACITY + 5; i++) {
        orb_display_scroll_trace_record(ORB_SCROLL_TRACE_SCROLL, 401300 + i, 0, i);
    }
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_COMPLETE, 416000, 0, 0);
    s_now = 500000;
    orb_display_scroll_trace_end();
    s_now = 600000;
    orb_display_scroll_trace_end(); /* Reporting/cleanup must preserve original end. */
    orb_display_scroll_trace_snapshot(&snapshot);
    assert(snapshot.available && !snapshot.active && snapshot.count == ORB_SCROLL_TRACE_CAPACITY);
    assert(snapshot.dropped == 6 && snapshot.completions == 4 && snapshot.loops == 1);
    assert(snapshot.intervals[16] == 2 && snapshot.intervals[127] == 1);
    assert(snapshot.handler_us == 123 && snapshot.timer_wait_us == 1000 && snapshot.fairness_wait_us == 100);
    assert(snapshot.started_us == 100 && snapshot.ended_us == 500000);
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_COMPLETE, 500001, 0, 0);
    assert(orb_display_scroll_trace_begin());
    assert(s_allocations == 2); /* Reuse arena, no per-event or repeat allocation. */
    orb_display_scroll_trace_snapshot(&snapshot);
    assert(snapshot.active && !snapshot.count && !snapshot.dropped && !snapshot.completions);
    for (unsigned i = 0; i < ORB_SCROLL_TRACE_INTERVAL_BUCKETS; i++) assert(!snapshot.intervals[i]);
    free(s_scroll_trace_events);
#else
    assert(!orb_display_scroll_trace_begin());
    orb_display_scroll_trace_record(ORB_SCROLL_TRACE_COMPLETE, 1000, 1, 2);
    orb_display_scroll_trace_end();
    orb_display_scroll_trace_snapshot(&snapshot);
    assert(!snapshot.available && !snapshot.active && !snapshot.count && !snapshot.completions);
    assert(!orb_display_scroll_trace_read(0, &event));
    assert(!s_allocations);
#endif
    assert(!s_locks);
    printf("scroll trace (option %d): allocation failure, enable/disable, bounds, intervals and reuse passed\n",
           CONFIG_ORB_DISPLAY_SCROLL_TRACE);
}
"""
    with tempfile.TemporaryDirectory(prefix="orb-scroll-trace-") as temporary:
        directory = Path(temporary)
        path = directory / "trace.c"
        path.write_text(fixture)
        for enabled in (0, 1):
            binary = directory / f"trace-{enabled}"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CC", "cc")),
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-unused-function",
                    "-Wno-unused-variable",
                    "-O1",
                    "-g",
                    "-fsanitize=address,undefined",
                    f"-DCONFIG_ORB_DISPLAY_SCROLL_TRACE={enabled}",
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
