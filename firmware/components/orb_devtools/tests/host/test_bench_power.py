"""Run benchmark admission with a deterministic UI owner and power timeline."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

component = Path(__file__).resolve().parents[2]
header = component / "orb_devtools_bench_power.h"
source = (component / "orb_devtools.c").read_text()

# Each owner must settle before freezing shell polling, including the original
# generic fixtures. The host integration test exercises the actual shell/Face.
for function in ("benchmark", "benchmark_game", "benchmark_navigation"):
    start = source.index(f"static bool {function}(")
    body = source[start : source.index("\n}\n", start)]
    assert body.index("orb_devtools_bench_power_ready(") < body.index("s_bench_active = true")

prelude = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define ESP_PLATFORM 1
#define pdMS_TO_TICKS(ms) (ms)
#define ORB_POWER_ACTIVE 0
#define ORB_POWER_ALWAYS_ON 2
#define ORB_POWER_MOMENT_NONE 0
#define ORB_POWER_MOMENT_WAKE 2
#define ESP_LOGW(...) (++s_warnings)
typedef struct { unsigned unused; } orb_shell_t;
typedef struct { unsigned state, moment; bool redact, panel_on; } orb_power_snapshot_t;
static orb_shell_t s_shell;
static orb_power_snapshot_t s_power;
static bool s_locked, s_accepted, s_stuck;
static int64_t s_us;
static unsigned s_checks, s_wakes, s_delays, s_warnings;
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr,"bench power:%d: %s\n",__LINE__,#value); exit(1); } } while(0)
static void lv_lock(void) { CHECK(!s_locked); s_locked = true; }
static void lv_unlock(void) { CHECK(s_locked); s_locked = false; }
static int64_t esp_timer_get_time(void) { return s_us; }
static orb_power_snapshot_t orb_shell_power_snapshot(orb_shell_t *shell) {
    CHECK(shell == &s_shell && s_locked); return s_power;
}
static bool wake(void) { CHECK(!s_locked); ++s_wakes; return s_accepted; }
static void vTaskDelay(unsigned ms) {
    CHECK(!s_locked && ms == 10); ++s_delays; s_us += ms * 1000;
    /* The UI's ordinary guarded wake/presentation is free to complete. */
    if (!s_stuck && s_us >= 400000) s_power = (orb_power_snapshot_t){.panel_on = true};
}
"""
tests = r"""
int main(void) {
    CHECK(!orb_devtools_bench_power_ready(NULL, wake)); CHECK(!s_wakes);
    CHECK(!orb_devtools_bench_power_ready(&s_shell, NULL)); CHECK(!s_wakes);
    CHECK(!orb_devtools_bench_power_ready(&s_shell, wake)); CHECK(!s_delays);
    s_accepted = true;
    s_power = (orb_power_snapshot_t){.panel_on = true};
    CHECK(orb_devtools_bench_power_ready(&s_shell, wake)); CHECK(!s_delays);
    s_power = (orb_power_snapshot_t){.panel_on = true, .moment = ORB_POWER_MOMENT_WAKE};
    CHECK(orb_devtools_bench_power_ready(&s_shell, wake));
    CHECK(s_us == 400000 && s_delays == 40 && !s_locked);
    /* Redaction, a dark panel, or a nonactive policy must never be bypassed. */
    orb_power_snapshot_t rejected[] = {
        {.redact = true, .panel_on = true},
        {.panel_on = false},
        {.state = ORB_POWER_ALWAYS_ON, .panel_on = true},
        {.panel_on = true, .moment = ORB_POWER_MOMENT_WAKE},
    };
    s_stuck = true;
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        s_power = rejected[i]; int64_t before = s_us;
        CHECK(!orb_devtools_bench_power_ready(&s_shell, wake));
        CHECK(s_us - before == 2000000 && !s_locked);
        CHECK(s_power.state == rejected[i].state && s_power.redact == rejected[i].redact &&
              s_power.panel_on == rejected[i].panel_on && s_power.moment == rejected[i].moment);
    }
    CHECK(s_warnings == 4);
    printf("benchmark power: %u checks; normal wake completion, UI ownership, privacy rejection and timeout passed\n", s_checks);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="orb-bench-power-") as temporary:
    directory = Path(temporary)
    for name in (
        "orb_shell.h",
        "esp_log.h",
        "esp_timer.h",
        "freertos/FreeRTOS.h",
        "freertos/task.h",
    ):
        stub = directory / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.touch()
    harness = directory / "bench-power.c"
    harness.write_text(prelude + f'\n#include "{header}"\n' + tests)
    binary = directory / "bench-power"
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
            "-I",
            str(directory),
            str(harness),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
