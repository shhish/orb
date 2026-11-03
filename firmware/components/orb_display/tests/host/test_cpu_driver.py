"""Compile the production idle snapshot with both counter widths and stats off."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    component = Path(__file__).resolve().parents[2]
    source = (component / "orb_display.c").read_text()
    function = source.split("void orb_display_cpu_snapshot(", 1)[1]
    function = "void orb_display_cpu_snapshot(" + function.split("\n#define UI_CORE", 1)[0]
    fixture = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "orb_display_cpu.h"
#define configGENERATE_RUN_TIME_STATS TEST_STATS
#define configUSE_TRACE_FACILITY TEST_STATS
#define INCLUDE_xTaskGetIdleTaskHandle TEST_STATS
#define portNUM_PROCESSORS 2
#define pdFALSE 0
#define eReady 1
#if TEST_BITS == 32
typedef uint32_t configRUN_TIME_COUNTER_TYPE;
#else
typedef uint64_t configRUN_TIME_COUNTER_TYPE;
#endif
#define portGET_RUN_TIME_COUNTER_VALUE() ((configRUN_TIME_COUNTER_TYPE)12345678)
typedef struct { configRUN_TIME_COUNTER_TYPE ulRunTimeCounter; } TaskStatus_t;
static int64_t esp_timer_get_time(void) { return 7654321; }
#if TEST_STATS
static unsigned s_calls;
static unsigned xTaskGetIdleTaskHandleForCore(unsigned core) { return core + 1; }
static void vTaskGetInfo(unsigned handle, TaskStatus_t *out, int stack, int state)
{
    assert(!stack && state == eReady);
    assert(handle >= 1 && handle <= 2);
    out->ulRunTimeCounter = handle * 1000;
    s_calls++;
}
#endif
"""
    fixture += (
        function
        + r"""
int main(void)
{
    orb_display_cpu_snapshot_t snapshot;
    memset(&snapshot, 0xA5, sizeof(snapshot));
    orb_display_cpu_snapshot(&snapshot);
    assert(snapshot.at_us == 7654321);
#if TEST_STATS
    assert(snapshot.valid && s_calls == 2);
    assert(snapshot.runtime == 12345678 && snapshot.counter_bits == TEST_BITS);
    assert(snapshot.cores == 2 && snapshot.idle[0] == 1000 && snapshot.idle[1] == 2000);
#else
    assert(!snapshot.valid && !snapshot.cores && !snapshot.counter_bits);
    assert(!snapshot.runtime && !snapshot.idle[0] && !snapshot.idle[1]);
#endif
    printf("CPU snapshot: stats=%d width=%d passed\n", TEST_STATS, TEST_BITS);
}
"""
    )
    with tempfile.TemporaryDirectory(prefix="orb-cpu-snapshot-") as temporary:
        directory = Path(temporary)
        path = directory / "cpu.c"
        path.write_text(fixture)
        binary = directory / "cpu"
        for stats, bits in ((0, 32), (1, 32), (1, 64)):
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CC", "cc")),
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-DTEST_STATS={stats}",
                    f"-DTEST_BITS={bits}",
                    "-I",
                    str(component / "include"),
                    str(path),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
