"""Production input consumer: mode switches cannot reuse gyro bias or gestures."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[3]
    source = (root / "main/orb_input_service.c").read_text()
    source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
    fixture = (
        r"""
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "orb_senses.h"
#include "orb_idle_policy.h"
#define CONFIG_ORB_DEVTOOLS 0
#define ESP_OK 0
typedef struct { float x, y, z; } orb_accel_t;
typedef struct { orb_accel_t gravity; int64_t at_us; orb_senses_result_t senses; bool motion; uint32_t next_poll_ms; } orb_input_motion_t;
typedef struct {
    int64_t now_us, motion_sample_us;
    float accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z;
    bool motion_valid, gyro_valid;
} orb_app_input_t;
static uint32_t s_mode_generation = 1;
static int64_t s_now;
static bool s_valid = true, s_screen;
static orb_motion_sample_t s_sample = {.az = 1, .gx = 20, .gy = 20, .gz = 20};
static orb_app_input_t s_input;
typedef void orb_shell_t;
static unsigned s_imu_samples, s_sleep_imu_samples;
static unsigned s_app_calls, s_touch_reads;
static bool s_locked;
static void lv_lock(void) { assert(!s_locked); s_locked = true; }
static void lv_unlock(void) { assert(s_locked); s_locked = false; }
static orb_power_snapshot_t orb_shell_power_snapshot(orb_shell_t *shell) {
    assert(shell && s_locked); return (orb_power_snapshot_t){0};
}
static void orb_shell_power_imu(orb_shell_t *shell, float x, float y, float z, uint64_t now) {
    assert(shell && s_locked && now == 0); (void)x; (void)y; (void)z; s_imu_samples++;
}
static void orb_shell_power_imu_sleep(orb_shell_t *shell, float x, float y, float z, uint64_t now) {
    s_sleep_imu_samples++; orb_shell_power_imu(shell, x, y, z, now);
}
static void orb_display_wake(void) { assert(s_locked); }
static int orb_board_motion_configure(bool gyro, bool taps) { (void)gyro; (void)taps; return 0; }
static uint32_t orb_board_motion_generation(void) { return s_mode_generation; }
static int orb_board_motion(orb_motion_sample_t *out) { *out = s_sample; return s_valid ? 0 : -1; }
static int64_t esp_timer_get_time(void) { return s_now; }
static bool orb_display_touch_pressed(void) { s_touch_reads++; return false; }
static void orb_shell_apps_service(void) { s_app_calls++; }
static void orb_shell_apps_publish(void) { s_app_calls++; }
static void orb_apps_poll_input(const orb_app_input_t *input, bool held, bool screen) {
    assert(!held); s_input = *input; s_screen = screen; s_app_calls++;
}
"""
        + source
        + r"""
static orb_input_motion_t sample(orb_idle_policy_t policy) {
    s_now += 20000; return orb_input_motion(policy, true, (orb_shell_t *)1);
}
int main(void) {
    orb_idle_policy_t active = {.gyro = true, .taps = true};
    for (unsigned i = 0; i < 100; i++) sample(active);
    assert(s_imu_samples == 40); /* 20 Hz despite independent UI cadence. */
    assert(s_senses.bias_valid && s_input.gyro_valid && s_input.motion_valid && s_screen);
    assert(s_senses.bias_x == 20 && s_senses.bias_z == 20);
    orb_idle_policy_t low = {0};
    s_mode_generation++;
    s_sample.gx = s_sample.gy = s_sample.gz = 0;
    s_sample.hardware_double_tap = true;
    for (unsigned i = 0; i < 100; i++) {
        orb_input_motion_t result = sample(low);
        assert(!result.senses.detents && !result.senses.double_tap);
        assert(!s_input.gyro_valid && !s_input.motion_valid && !s_screen);
    }
    assert(s_senses.bias_valid && s_senses.bias_z == 0);
    s_mode_generation++;
    s_sample.gx = s_sample.gy = s_sample.gz = 20;
    orb_input_motion_t result = sample(active);
    assert(!s_senses.bias_valid && !s_input.gyro_valid);
    assert(!result.senses.detents && !result.senses.double_tap);
    s_valid = false; result = sample(active);
    assert(!result.motion && !s_input.motion_valid && !s_input.gyro_valid);
    s_valid = true;
    for (unsigned i = 0; i < 100; i++) sample(active);
    assert(s_input.gyro_valid && s_senses.bias_z == 20);
    unsigned app_calls = s_app_calls, touch_reads = s_touch_reads, imu_samples = s_imu_samples;
    orb_senses_t before = s_senses;
    orb_idle_policy_t asleep = {.poll_ms = 100, .minimal = true};
    for (unsigned i = 0; i < 10; i++) {
        s_now += 100000;
        result = orb_input_motion(asleep, false, (orb_shell_t *)1);
        assert(result.motion && result.next_poll_ms == 100);
        assert(!result.senses.double_tap && !result.senses.detents);
    }
    assert(s_imu_samples == imu_samples + 10);
    assert(s_sleep_imu_samples == 10);
    assert(s_app_calls == app_calls && s_touch_reads == touch_reads);
    assert(!memcmp(&before, &s_senses, sizeof(before)));
    puts("input modes: bias/gesture reset, no low-mode game input, settled re-enable passed");
}
"""
    )
    with tempfile.TemporaryDirectory(prefix="orb-input-modes-") as temporary:
        path = Path(temporary) / "input.c"
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
                f"-I{root / 'main'}",
                f"-I{root / 'components/orb_board/include'}",
                f"-I{root / 'components/orb_power/include'}",
                f"-I{root / 'components/orb_buttons/include'}",
                str(path),
                str(root / "components/orb_board/orb_senses.c"),
                "-lm",
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
