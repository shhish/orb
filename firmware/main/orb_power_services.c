#include "orb_power_services.h"

#include "esp_log.h"
#include "orb_board.h"
#include "orb_model.h"
#include "orb_net.h"
#include "orb_link.h"
#include "orb_ota.h"
#include "orb_display.h"
#include "orb_light_sleep.h"
#include "orb_apps.h"
#include "orb_time.h"
#include "orb_voice.h"
#include <string.h>
#include <sys/time.h>
#if CONFIG_ORB_DEVTOOLS
#include "orb_devtools.h"
#endif

static const char *TAG = "power";

/* The panel decides how eagerly the radio listens. Work that must stay prompt
 * overrides BACKGROUND inside orb_net. The cell's own cutoff is deferred past
 * an OTA write; ordinary NVS commits are atomic. */
void orb_power_services_apply(orb_power_snapshot_t power, bool speech)
{
    static bool last_background, last_ota, last_voice;
    orb_ota_status_t ota = {0};
    orb_ota_status(&ota);
    bool ota_active = ota.state == ORB_OTA_DOWNLOADING || ota.state == ORB_OTA_VERIFYING || ota.state == ORB_OTA_READY;
    orb_voice_state_t voice = {0};
    bool voice_active = speech;
    if (orb_model_get_voice(&voice))
        voice_active |= voice.phase == ORB_VOICE_STARTING || voice.phase == ORB_VOICE_RECORDING ||
            voice.phase == ORB_VOICE_TRANSCRIBING || voice.phase == ORB_VOICE_CONFIRMING ||
            voice.phase == ORB_VOICE_SENDING || voice.phase == ORB_VOICE_ERROR;
    if (ota_active != last_ota) orb_net_set_work_active(ORB_NET_WORK_OTA, ota_active);
    if (voice_active != last_voice) orb_net_set_work_active(ORB_NET_WORK_VOICE, voice_active);
    last_ota = ota_active;
    last_voice = voice_active;
    bool background = !power.panel_on || power.state == ORB_POWER_ALWAYS_ON;
    if (background != last_background) {
        orb_net_set_power_hint(background ? ORB_NET_POWER_BACKGROUND : ORB_NET_POWER_INTERACTIVE);
        last_background = background;
    }
    if (power.shutdown_requested && !ota_active) {
        ESP_LOGW(TAG, "battery exhausted: powering off to protect the cell");
        orb_board_panel_brightness(0);
        (void)orb_board_power_off();
    }
}

orb_idle_policy_t orb_power_services_policy(orb_shell_t *shell, bool power_valid, bool usb)
{
    orb_speech_status_t speech;
    orb_voice_speech_status(&speech);
    orb_voice_state_t voice = {0};
    bool busy = orb_model_get_voice(&voice) && (voice.phase == ORB_VOICE_STARTING ||
        voice.phase == ORB_VOICE_RECORDING || voice.phase == ORB_VOICE_TRANSCRIBING ||
        voice.phase == ORB_VOICE_CONFIRMING || voice.phase == ORB_VOICE_SENDING ||
        voice.phase == ORB_VOICE_ERROR);
    orb_ota_status_t ota = {0};
    orb_ota_status(&ota);
    orb_time_snapshot_t time;
    orb_time_snapshot(orb_time_now_us(), &time);
    const orb_app_manifest_t *app = orb_apps_active();
    bool benchmark = false;
#if CONFIG_ORB_DEVTOOLS
    benchmark = orb_devtools_benchmark_active();
#endif
    lv_lock();
    orb_power_snapshot_t power = orb_shell_power_snapshot(shell);
    uint32_t inhibitors = orb_shell_power_inhibitors(shell);
    power.usb = usb; /* Latest PMU sample revokes sleep before the next UI status slot. */
    orb_light_sleep_inputs_t inputs = {
        .power = power, .inhibitors = inhibitors, .power_valid = power_valid,
        .panel_off = orb_display_panel_ready(false), .voice = busy || speech.active,
        .ota = ota.state == ORB_OTA_DOWNLOADING || ota.state == ORB_OTA_VERIFYING || ota.state == ORB_OTA_READY,
        .alarm = time.alarm_ringing || time.timer_state == ORB_TIME_FINISHED || time.reminder_due,
        .benchmark = benchmark, .game = app && strcmp(app->id, "time"),
    };
    lv_unlock();
    /* PM recovery may configure clocks or register callbacks. Only the shell
     * snapshot needs LVGL ownership; do not retain it across runtime work. */
    bool eligible = orb_light_sleep_update(inputs);
    /* A host gave up on the USB port if it was attached while the device slept. */
    static uint64_t s_usb_entries;
    orb_light_sleep_status_t sleep;
    orb_light_sleep_status(&sleep);
    if (usb && !sleep.usb_override && sleep.entries != s_usb_entries) {
        s_usb_entries = sleep.entries;
        orb_board_usb_reattach();
    }
    orb_idle_policy_t policy = orb_idle_sleep_policy(orb_idle_policy(power, speech.active,
        time.timer_state == ORB_TIME_FINISHED && !time.alarm_ringing), power, eligible);
    orb_voice_set_sleep_eligible(eligible);
    orb_link_set_sleep_eligible(eligible);
    orb_power_services_apply(power, speech.active);
    return policy;
}

int64_t orb_power_services_deadline(int64_t now)
{
    static uint32_t generation;
    static int64_t next_poll;
    struct timeval wall;
    gettimeofday(&wall, NULL);
    int64_t wall_us = (int64_t)wall.tv_sec * 1000000 + wall.tv_usec;
    int64_t deadline = orb_time_next_deadline_us(now, wall_us);
    /* Civil conversion and finish/sound service need not run at the IMU's
     * 10 Hz rate. Query live schedules each slot, then service exact deadlines
     * or the bounded clock-jump reconciliation, whichever comes first. */
    if (!next_poll || now >= next_poll || deadline <= now) {
        orb_time_poll(now, wall.tv_sec);
        orb_time_snapshot_t time;
        orb_time_snapshot(now, &time);
        if (time.generation != generation) orb_display_wake();
        generation = time.generation;
        next_poll = now <= INT64_MAX - 1000000 ? now + 1000000 : INT64_MAX;
        deadline = orb_time_next_deadline_us(now, wall_us);
    }
    return deadline < next_poll ? deadline : next_poll;
}
