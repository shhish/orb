"""Production sleep admission snapshots LVGL state before running PM recovery."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    firmware = Path(__file__).resolve().parents[4]
    source = (firmware / "main/orb_power_services.c").read_text()
    policy = source[
        source.index("orb_idle_policy_t orb_power_services_policy(") : source.index(
            "int64_t orb_power_services_deadline("
        )
    ]
    fixture = r"""
#include <assert.h>
#include <string.h>
#include "orb_idle_policy.h"
#include "orb_light_sleep.h"
#include "orb_model.h"
#define CONFIG_ORB_DEVTOOLS 1
typedef void orb_shell_t;
typedef struct { bool active; } orb_speech_status_t;
typedef struct { int state; } orb_ota_status_t;
typedef struct { bool alarm_ringing, reminder_due; int timer_state; } orb_time_snapshot_t;
typedef struct { const char *id; } orb_app_manifest_t;
enum { ORB_OTA_DOWNLOADING = 1, ORB_OTA_VERIFYING, ORB_OTA_READY, ORB_TIME_FINISHED };
static bool s_locked, s_voice_known = true, s_speech, s_eligible, s_benchmark;
static bool s_voice_eligible, s_link_eligible;
static unsigned s_phase;
static orb_power_snapshot_t s_power = {.state = ORB_POWER_SLEEP};
static orb_light_sleep_inputs_t s_inputs;
static orb_speech_status_t s_speech_status;
static orb_app_manifest_t s_app = {.id = "time"};
static void orb_voice_speech_status(orb_speech_status_t *out) { *out = s_speech_status; }
bool orb_model_get_voice(orb_voice_state_t *out) { out->phase = s_phase; return s_voice_known; }
static void orb_ota_status(orb_ota_status_t *out) { memset(out, 0, sizeof(*out)); }
static int64_t orb_time_now_us(void) { return 1000; }
static void orb_time_snapshot(int64_t now, orb_time_snapshot_t *out) {
    assert(now == 1000); memset(out, 0, sizeof(*out));
}
static const orb_app_manifest_t *orb_apps_active(void) { return &s_app; }
static bool orb_devtools_benchmark_active(void) { return s_benchmark; }
void orb_light_sleep_status(orb_light_sleep_status_t *out) { memset(out, 0, sizeof(*out)); }
static void orb_board_usb_reattach(void) { assert(!s_locked); }
static void lv_lock(void) { assert(!s_locked); s_locked = true; }
static void lv_unlock(void) { assert(s_locked); s_locked = false; }
static orb_power_snapshot_t orb_shell_power_snapshot(orb_shell_t *shell) {
    assert(shell && s_locked); return s_power;
}
static uint32_t orb_shell_power_inhibitors(orb_shell_t *shell) { assert(shell && s_locked); return 0; }
static bool orb_display_panel_ready(bool awake) { assert(s_locked && !awake); return true; }
bool orb_light_sleep_update(orb_light_sleep_inputs_t inputs) {
    /* Recovery can register callbacks/configure PM. It must not own LVGL. */
    assert(!s_locked); s_inputs = inputs;
    s_eligible = !inputs.voice && !inputs.power.usb;
    return s_eligible;
}
static void orb_voice_set_sleep_eligible(bool eligible) { assert(!s_locked); s_voice_eligible = eligible; }
static void orb_link_set_sleep_eligible(bool eligible) { assert(!s_locked); s_link_eligible = eligible; }
static void orb_power_services_apply(orb_power_snapshot_t power, bool speech) {
    assert(!s_locked && power.state == s_power.state); s_speech = speech;
}
"""
    fixture += policy
    fixture += r"""
int main(void) {
    for (s_phase = ORB_VOICE_IDLE; s_phase <= ORB_VOICE_ERROR; s_phase++) {
        orb_idle_policy_t policy = orb_power_services_policy((orb_shell_t *)1, true, false);
        bool busy = s_phase != ORB_VOICE_IDLE;
        assert(s_inputs.voice == busy && policy.minimal == !busy);
        assert(s_voice_eligible == !busy && s_link_eligible == !busy);
        assert(s_inputs.power_valid && s_inputs.panel_off && !s_locked);
    }
    s_voice_known = false;
    assert(orb_power_services_policy((orb_shell_t *)1, true, false).minimal);
    s_speech_status.active = true;
    assert(!orb_power_services_policy((orb_shell_t *)1, true, false).minimal);
    assert(s_inputs.voice && s_speech);
    s_speech_status.active = false;
    /* Latest sampled VBUS overrides an older shell snapshot in this pass. */
    assert(!orb_power_services_policy((orb_shell_t *)1, true, true).minimal);
    assert(s_inputs.power.usb && !s_voice_eligible && !s_link_eligible);
}
"""
    with tempfile.TemporaryDirectory(prefix="orb-power-services-") as temporary:
        source_path = Path(temporary) / "services.c"
        source_path.write_text(fixture)
        binary = source_path.with_suffix("")
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-I" + str(firmware / "main"),
                "-I" + str(firmware / "components/orb_power/include"),
                "-I" + str(firmware / "components/orb_buttons/include"),
                "-I" + str(firmware / "components/orb_model/include"),
                str(source_path),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
