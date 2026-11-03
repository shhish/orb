"""Production calm contact/lease paths with the real deterministic controller."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    firmware = Path(__file__).resolve().parents[3]
    source = Path(
        os.environ.get("ORB_CALM_SOURCE", firmware / "components/orb_shell/orb_shell_power.c")
    ).read_text()

    def section(start, end):
        return source[source.index(start) : source.index(end)]

    fixture = r"""
#include "orb_power.h"
#include "orb_buttons.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef void orb_shell_t;
typedef void lv_indev_t;
typedef void lv_event_dsc_t;
typedef struct { const char *id; } orb_app_manifest_t;
enum { LV_INDEV_TYPE_POINTER, LV_EVENT_PRESSED, LV_INDEV_STATE_PRESSED };
enum { ORB_VOICE_IDLE, ORB_VOICE_STARTING, ORB_VOICE_RECORDING, ORB_VOICE_TRANSCRIBING,
    ORB_VOICE_CONFIRMING, ORB_VOICE_SENDING, ORB_VOICE_ERROR };
enum { ORB_SHELL_OTA_IDLE, ORB_SHELL_OTA_DOWNLOADING, ORB_SHELL_OTA_VERIFYING, ORB_SHELL_OTA_READY };
enum { ORB_CARD_APPROVAL, ORB_CARD_MESSAGE };
typedef struct { bool answered; int kind; } orb_card_t;
typedef struct {
    uint64_t revision;
    unsigned card_count;
    orb_card_t cards[1];
    struct { int phase; } voice;
} orb_model_snapshot_t;
typedef struct { int ota, battery_pct; bool usb, charging; } orb_shell_snapshot_t;
typedef struct {
    orb_shell_t *shell;
    orb_power_controller_t controller;
    orb_model_snapshot_t *model;
    orb_shell_snapshot_t status;
    uint64_t model_revision, now_ms;
    uint32_t tick, presented_revision;
    unsigned waiting;
    bool fence_required, enabled, locked, alert, consume_button[ORB_BUTTON_COUNT];
    uint8_t wake_brightness;
} orb_shell_calm_t;
static orb_shell_calm_t s_calm;
static orb_model_snapshot_t s_model;
static orb_app_manifest_t s_app = {.id = "game"};
static uint32_t s_tick;
static unsigned s_presents;
static bool s_changed, s_app_active, s_time_alert;
static uint32_t lv_tick_get(void) { return s_tick; }
static lv_indev_t *lv_indev_get_next(lv_indev_t *p) { (void)p; return NULL; }
static int lv_indev_get_type(lv_indev_t *p) { (void)p; return LV_INDEV_TYPE_POINTER; }
static unsigned lv_indev_get_event_count(lv_indev_t *p) { (void)p; return 0; }
static lv_event_dsc_t *lv_indev_get_event_dsc(lv_indev_t *p, unsigned n) { (void)p; (void)n; return NULL; }
static void input_event(void) { assert(0); }
static void (*lv_event_dsc_get_cb(lv_event_dsc_t *p))(void) { (void)p; return input_event; }
static void *lv_event_dsc_get_user_data(lv_event_dsc_t *p) { (void)p; return &s_calm; }
static void lv_indev_add_event_cb(lv_indev_t *p, void (*cb)(void), int event, void *data)
{ (void)p; (void)cb; (void)event; (void)data; assert(0); }
static int lv_indev_get_state(lv_indev_t *p) { (void)p; return 0; }
static void present(orb_shell_calm_t *c) { assert(c == &s_calm); s_presents++; }
static void notice_present(orb_shell_calm_t *c) { (void)c; }
static void notice_update(orb_shell_calm_t *c) { (void)c; }
static bool orb_shell_calm_dismiss_notice(orb_shell_calm_t *c) { (void)c; return false; }
static bool orb_ui_reduce_motion(void) { return false; }
static bool orb_model_snapshot_if_changed(uint64_t revision, orb_model_snapshot_t *model)
{ (void)revision; assert(model == &s_model); bool changed = s_changed; s_changed = false; return changed; }
static const orb_app_manifest_t *orb_apps_active(void) { return s_app_active ? &s_app : NULL; }
static bool orb_apps_paused(void) { return false; }
static bool orb_shell_time_alert_active(orb_shell_t *s) { (void)s; return s_time_alert; }
static orb_shell_calm_t *orb_shell_calm_context(orb_shell_t *s) { (void)s; return &s_calm; }
void orb_shell_calm_poll(orb_shell_calm_t *c);
"""
    fixture += section("static uint64_t clock_now(", "static void bare(")
    fixture += section("void orb_shell_calm_status(", "void orb_shell_calm_lock(")
    fixture += section("void orb_shell_calm_alert(", "void orb_shell_power_enable(")
    fixture += section("static void power_imu(", "orb_power_snapshot_t orb_shell_power_snapshot(")
    fixture += section(
        "void orb_shell_power_poll(", "orb_power_preferences_t orb_shell_power_preferences("
    )
    fixture += r"""
static void setup(bool locked) {
    s_model = (orb_model_snapshot_t){0};
    s_calm = (orb_shell_calm_t){.enabled = true, .model = &s_model, .wake_brightness = 70,
        .status = {.battery_pct = 80}};
    s_tick = 0; s_presents = 0; s_changed = s_app_active = s_time_alert = false;
    orb_power_init(&s_calm.controller, 0);
    assert(orb_power_wake(&s_calm.controller, ORB_POWER_WAKE_PWR, 0).accepted);
    if (locked) lock_at(&s_calm, 0);
}
static bool button(orb_button_gesture_t gesture, bool synthetic) {
    orb_button_event_t event = {.button = ORB_BUTTON_PWR, .gesture = gesture, .synthetic = synthetic};
    return orb_shell_calm_button(&s_calm, &event);
}
static void alert_contacts(void) {
    setup(true);
    s_calm.controller.ignored_pickups = 3;
    s_calm.controller.pickup_suppressed_until_ms = 300000;
    s_tick = 1000;
    orb_shell_calm_alert(&s_calm, true);
    assert(s_calm.alert && s_calm.locked && s_calm.controller.redact);
    assert(!button(ORB_BUTTON_DOWN, true));
    assert(s_calm.controller.ignored_pickups == 3 && s_calm.locked && s_calm.controller.redact);
    assert(!wake_at(&s_calm, ORB_POWER_WAKE_TOUCH, 1000).accepted);
    assert(s_calm.controller.ignored_pickups == 3 && s_calm.locked && s_calm.controller.redact);
    assert(button(ORB_BUTTON_DOWN, false));
    assert(s_calm.controller.ignored_pickups == 0 && s_calm.controller.activity_ms == 1000);
    assert(s_calm.controller.redact && s_calm.locked);
    /* Dismissal before UP must not change ownership of the old press. */
    orb_shell_calm_alert(&s_calm, false);
    assert(s_calm.locked && s_calm.controller.redact);
    assert(button(ORB_BUTTON_UP, false));
    assert(button(ORB_BUTTON_SHORT, false));
    assert(button(ORB_BUTTON_DOUBLE, false));
    assert(button(ORB_BUTTON_HOLD, false));
    assert(s_calm.controller.redact && s_calm.locked);
    assert(button(ORB_BUTTON_DOWN, false));
    assert(!s_calm.controller.redact && !s_calm.locked);
    assert(button(ORB_BUTTON_UP, false) && button(ORB_BUTTON_SHORT, false));
}
static void pending_voice(void) {
    const int phases[] = {ORB_VOICE_STARTING, ORB_VOICE_RECORDING, ORB_VOICE_TRANSCRIBING,
        ORB_VOICE_CONFIRMING, ORB_VOICE_SENDING, ORB_VOICE_ERROR};
    for (unsigned i = 0; i < sizeof(phases)/sizeof(phases[0]); i++) {
        setup(false); s_model.voice.phase = phases[i]; s_changed = true;
        s_tick = 120000; orb_shell_calm_poll(&s_calm);
        assert(s_calm.controller.inhibitors == ORB_POWER_INHIBIT_VOICE);
        assert(s_calm.controller.state == ORB_POWER_ACTIVE && !s_calm.controller.redact);
        s_tick += 200000; orb_shell_calm_poll(&s_calm);
        assert(s_calm.controller.state == ORB_POWER_ACTIVE);
        s_model.voice.phase = ORB_VOICE_IDLE;
        orb_shell_calm_poll(&s_calm);
        s_tick += 19999; orb_shell_calm_poll(&s_calm);
        assert(s_calm.controller.state == ORB_POWER_ACTIVE);
        s_tick++; orb_shell_calm_poll(&s_calm);
        assert(s_calm.controller.state == ORB_POWER_DIMMED && s_calm.controller.redact);
    }
}
static void locked_attention(void) {
    setup(true);
    s_changed = true; s_model.card_count = 1;
    s_tick = 1000; orb_shell_calm_poll(&s_calm);
    assert(s_calm.controller.state == ORB_POWER_ACTIVE && s_calm.controller.redact);
    assert(!s_calm.locked && !s_calm.controller.display_locked);
    /* Explicit Quiet still suppresses the same automatic attention. */
    setup(true);
    assert(orb_power_set_quiet(&s_calm.controller, true, 0));
    s_changed = true; s_model.card_count = 1;
    s_tick = 1000; orb_shell_calm_poll(&s_calm);
    assert(s_calm.controller.state == ORB_POWER_ALWAYS_ON && s_calm.controller.redact);
    assert(s_calm.locked && s_calm.controller.quiet);
}
static void locked_work(void) {
    setup(true);
    s_model.voice.phase = ORB_VOICE_RECORDING;
    s_calm.status.ota = ORB_SHELL_OTA_DOWNLOADING;
    s_app_active = true;
    s_calm.controller.inhibitors = ORB_POWER_INHIBIT_DMA | ORB_POWER_INHIBIT_PLAYBACK;
    orb_shell_calm_poll(&s_calm);
    for (s_tick = 100; s_tick < 120250; s_tick += 100) {
        orb_shell_power_imu_sleep(s_calm.shell, 0, 0, 1, 0);
        assert(s_calm.controller.activity_ms == 0);
    }
    orb_shell_calm_poll(&s_calm);
    unsigned all = ORB_POWER_INHIBIT_VOICE | ORB_POWER_INHIBIT_PLAYBACK | ORB_POWER_INHIBIT_OTA |
        ORB_POWER_INHIBIT_APP | ORB_POWER_INHIBIT_DMA;
    assert(s_calm.controller.inhibitors == all && s_calm.locked && s_calm.controller.redact);
    assert(s_calm.controller.state == ORB_POWER_SLEEP && s_calm.controller.inhibitors == all);
    orb_shell_calm_alert(&s_calm, true);
    orb_shell_calm_alert(&s_calm, false);
    assert((s_calm.controller.inhibitors & ~ORB_POWER_INHIBIT_APP) == (all & ~ORB_POWER_INHIBIT_APP));
}
static void first_observed_lease(void) {
    for (unsigned lease = 0; lease < 4; lease++) {
        setup(false);
        if (lease == 0) s_model.voice.phase = ORB_VOICE_RECORDING;
        if (lease == 1) s_calm.status.ota = ORB_SHELL_OTA_VERIFYING;
        if (lease == 2) s_app_active = true;
        if (lease == 3) s_time_alert = true;
        s_changed = true; s_model.card_count = 1;
        s_tick = 120000;
        orb_shell_snapshot_t status = s_calm.status;
        orb_shell_calm_status(&s_calm, &status);
        orb_shell_calm_poll(&s_calm);
        assert(s_calm.controller.state == ORB_POWER_ACTIVE);
        assert(!s_calm.controller.redact && s_calm.controller.inhibitors);
    }
}
static void single_status_poll(void) {
    setup(false);
    s_tick = 1000;
    orb_shell_power_poll(s_calm.shell, &s_calm.status);
    assert(s_presents == 1);
    orb_shell_power_poll(s_calm.shell, NULL);
    assert(s_presents == 2);
}
int main(void) {
    alert_contacts(); pending_voice(); locked_attention(); locked_work(); first_observed_lease();
    single_status_poll();
    puts("calm power: opaque-alert contacts, locked system leases, pending voice and acquisition ordering passed");
}
"""
    with tempfile.TemporaryDirectory(prefix="orb-calm-safety-") as temporary:
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
                f"-I{firmware / 'components/orb_buttons/include'}",
                str(path),
                str(
                    os.environ.get(
                        "ORB_POLICY_SOURCE", firmware / "components/orb_power/orb_power.c"
                    )
                ),
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
