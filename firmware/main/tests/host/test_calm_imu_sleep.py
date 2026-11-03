"""Production calm IMU path: unchanged Sleep never invokes presentation."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    firmware = Path(__file__).resolve().parents[3]
    source = (firmware / "components/orb_shell/orb_shell_power.c").read_text()
    clock = source[
        source.index("static uint64_t clock_now(") : source.index(
            "static orb_power_input_result_t wake_at("
        )
    ]
    imu = source[
        source.index("static void power_imu(") : source.index(
            "orb_power_snapshot_t orb_shell_power_snapshot("
        )
    ]
    fixture = (
        r"""
#include "orb_power.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
typedef void orb_shell_t;
typedef struct {
    orb_power_controller_t controller;
    uint32_t tick;
    uint64_t now_ms;
    bool enabled, locked, alert;
} orb_shell_calm_t;
static orb_shell_calm_t s_calm;
static uint32_t s_tick;
static unsigned s_present;
static uint32_t lv_tick_get(void) { return s_tick; }
static orb_shell_calm_t *orb_shell_calm_context(orb_shell_t *shell) { return shell ? &s_calm : NULL; }
static void present(orb_shell_calm_t *calm) { assert(calm == &s_calm); s_present++; }
"""
        + clock
        + imu
        + r"""
static void setup(void) {
    s_calm = (orb_shell_calm_t){.enabled = true, .locked = true, .tick = 1250, .now_ms = 1250};
    orb_power_init(&s_calm.controller, 0);
    assert(orb_power_request_sleep(&s_calm.controller, 1000));
    assert(orb_power_tick(&s_calm.controller, 1250));
    assert(s_calm.controller.state == ORB_POWER_SLEEP);
    s_tick = 1250; s_present = 0;
}
static void sample(float x, float z, unsigned dt) {
    s_tick += dt;
    orb_shell_power_imu_sleep((orb_shell_t *)1, x, 0, z, 0);
}
int main(void) {
    setup();
    uint32_t revision = s_calm.controller.revision;
    for (unsigned i = 0; i < 20; i++) sample(0, 1, 100);
    assert(s_calm.controller.motion.armed && s_calm.controller.revision == revision);
    assert(s_calm.locked && s_present == 0); /* lock_at is idempotent in Sleep. */
    for (unsigned i = 0; i < 10 && !s_present; i++)
        sample(sinf(50 * 0.01745329251994f), cosf(50 * 0.01745329251994f), 100);
    assert(s_present == 1 && s_calm.controller.state == ORB_POWER_ACTIVE);
    assert(s_calm.controller.redact && !s_calm.locked);
    s_tick += 100;
    orb_shell_power_imu((orb_shell_t *)1, 0, 0, 1, 0);
    assert(s_present == 2); /* Ordinary input keeps its previous presentation path. */

    setup();
    for (unsigned i = 0; i < 4; i++) sample(0, -1, 100);
    assert(s_calm.controller.motion.face_down && s_present == 1);
    sample(0, -1, 1000);
    sample(NAN, NAN, 1000);
    assert(s_calm.controller.motion.face_down && s_present == 1);
    sample(0, 1, 1000);
    sample(0, 1, 100); sample(0, 1, 100);
    assert(s_calm.controller.motion.face_down && s_present == 1);
    sample(0, 1, 100);
    assert(!s_calm.controller.motion.face_down && s_present == 2);
    assert(s_calm.controller.state == ORB_POWER_SLEEP && s_calm.locked);
    puts("calm sleep IMU: steady samples skip presentation; wake/face-down changes present exactly once");
}
"""
    )
    with tempfile.TemporaryDirectory(prefix="orb-calm-imu-") as temporary:
        path = Path(temporary) / "calm.c"
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
                f"-I{firmware / 'components/orb_power/include'}",
                str(path),
                str(firmware / "components/orb_power/orb_power.c"),
                str(firmware / "components/orb_power/orb_power_imu.c"),
                "-lm",
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
