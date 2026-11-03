/* Whole-OS Alive evidence, linked only into the deterministic parity runner.
 * Uses production presenters and process-local parity models; no board ports. */
#include "parity.h"
#include "sim_display.h"
#include "orb_faces.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_kit.h"
#include "orb_kit_effects.h"
#include "orb_kit_motion.h"
#include "orb_sound.h"
#include "orb_time.h"
#include "orb_ui.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint32_t s_alive_pixels, s_alive_frames, s_alive_peak, s_alive_refresh, s_alive_last_damage;
static orb_faces_t *s_alive_face;

static void alive_frames(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
    case LV_EVENT_RENDER_START: s_alive_refresh = 0; break;
    case LV_EVENT_FLUSH_START: {
        uint32_t pixels = lv_area_get_size(lv_event_get_param(event));
        s_alive_pixels += pixels; s_alive_refresh += pixels; break;
    }
    case LV_EVENT_REFR_READY:
        if (s_alive_refresh) { ++s_alive_frames; s_alive_peak = LV_MAX(s_alive_peak, s_alive_refresh); s_alive_last_damage = parity_now; }
        s_alive_refresh = 0; break;
    default: break;
    }
}

static void alive_fixture(const char *id)
{
    for (unsigned i = 0; i < parity_shell_count; i++) if (!strcmp(id, parity_shell_scenarios[i].id)) {
        parity_shell_scenarios[i].setup(id); return;
    }
    for (unsigned i = 0; i < parity_app_count; i++) if (!strcmp(id, parity_app_scenarios[i].id)) {
        parity_app_scenarios[i].setup(id); return;
    }
    parity_check(false, "known real-screen capture fixture");
}

/* Stop at compositor completion, before its fresh-arrival vocabulary settles.
 * open_metrics.active intentionally includes arrival, so transition_end_us is
 * the fence used here. Captures are not physical throughput measurements. */
static void alive_transition(void)
{
    for (unsigned i = 0; i < 500; i++) {
        orb_shell_open_metrics_t metrics = {0};
        orb_shell_open_metrics(parity_shell, &metrics);
        if (metrics.transition_end_us && metrics.content_ready && !metrics.retaining_snapshot) return;
        parity_advance(4);
    }
    parity_check(false, "navigation transition completed for arrival capture");
}

static void alive_fresh(orb_shell_surface_t surface)
{
    parity_navigate(surface); parity_advance(4); alive_transition();
}

static void alive_tap(const char *caption)
{
    parity_check(parity_tap_text(caption), caption);
    alive_transition();
}

static void alive_face(const char *sequence)
{
    s_alive_face = orb_faces_create(lv_screen_active(), NULL);
    parity_check(s_alive_face != NULL, "create real Face for wake/roll");
    parity_set_root(orb_faces_root(s_alive_face));
    orb_faces_snapshot_t data = {.now = 1793182140, .usage_percent = {62, 38},
        .battery_percent = 80, .home_on = 4, .home_total = 12, .working = 2,
        .sessions = 3, .online = true, .weather_override = true,
        .weather = {.revision = 1, .configured = true, .available = true,
            .unit = ORB_TEMPERATURE_F, .temperature = 72, .high = 78, .low = 62,
            .condition = ORB_WEATHER_PARTLY_CLOUDY, .is_day = true, .updated_at = 1793182140}};
    strcpy(data.clock, "10:09"); strcpy(data.weekday, "WED"); strcpy(data.day, "28");
    strcpy(data.weather.label, "Partly cloudy");
    orb_faces_update(s_alive_face, &data); parity_advance(1400);
    if (!strcmp(sequence, "face-wake")) {
        orb_faces_set_active(s_alive_face, false);
        data.usage_percent[0] = 78; data.usage_percent[1] = 51; data.battery_percent = 76;
        orb_faces_update(s_alive_face, &data); parity_advance(1200);
        orb_faces_set_active(s_alive_face, true);
    } else if (!strcmp(sequence, "face-minute")) {
        strcpy(data.clock, "10:10"); data.now += 60; orb_faces_update(s_alive_face, &data);
    } else {
        data.weather.revision++; data.weather.temperature = 74;
        data.home_on = 5; data.working = 3;
        orb_faces_update(s_alive_face, &data);
    }
}

static void alive_agent_done(void)
{
    alive_fixture("session.default");
    orb_agent_stream_t stream = {.session = "parity-claude", .seq = 1, .at = 1793196301,
        .state = ORB_SESSION_DONE, .activity = "Tests passed", .steps_count = 2,
        .steps = {{.id = "read", .kind = ORB_STEP_TOOL, .label = "Read navigation", .state = ORB_STEP_DONE},
                  {.id = "build", .kind = ORB_STEP_TOOL, .label = "Build with four jobs", .state = ORB_STEP_DONE}},
        .assistant = {.id = "answer", .text = "All checks completed.", .final = true}};
    parity_check(orb_model_apply_stream(&stream), "real agent step completion");
}

static bool alive_prepare(const char *sequence)
{
    if (!strncmp(sequence, "face-", 5) || !strcmp(sequence, "roll-weather")) {
        alive_face(sequence); return false;
    }
    if (!strcmp(sequence, "settings-brightness")) {
        alive_fixture("settings.display"); alive_tap("Brightness");
    } else if (!strcmp(sequence, "volume")) {
        alive_fixture("settings.sound"); alive_tap("Volume");
    } else if (!strcmp(sequence, "usage")) {
        alive_fixture("agents.default"); alive_fresh(ORB_SHELL_USAGE);
    } else if (!strcmp(sequence, "usage-follow")) {
        alive_fixture("usage.default"); parity_advance(1200);
        orb_usage_snapshot_t usage = {.count = 2, .windows = {
            {.provider = ORB_AGENT_CLAUDE, .label = "5 hours", .used_percent = 79, .source = ORB_USAGE_USAGE},
            {.provider = ORB_AGENT_CLAUDE, .label = "week", .used_percent = 55, .source = ORB_USAGE_USAGE}}};
        parity_check(orb_model_set_usage(&usage), "changed real Usage values");
    } else if (!strcmp(sequence, "timer-open")) {
        parity_shell_create(true); parity_check(orb_time_timer_set(210), "timer duration fixture");
        alive_fresh(ORB_SHELL_TIME);
    } else if (!strcmp(sequence, "timer-action")) {
        alive_fixture("timer-paused"); parity_advance(1200);
        orb_time_timer_reset();
    } else if (!strcmp(sequence, "cc-toggle")) {
        alive_fixture("control.default"); parity_advance(1200);
        parity_tap(133, 132);
    } else if (!strcmp(sequence, "cc-follow")) {
        alive_fixture("control.default"); parity_advance(1200);
        parity_status.brightness = 31; orb_shell_update(parity_shell, &parity_status);
    } else if (!strcmp(sequence, "charging-follow")) {
        alive_fixture("charging.default"); parity_advance(1200);
        parity_status.battery_pct = 66; orb_shell_update(parity_shell, &parity_status);
    } else if (!strcmp(sequence, "volume-hud-follow")) {
        alive_fixture("volume-hud.default");
        orb_sound_set_volume(77); orb_shell_show_volume(parity_shell);
    } else if (!strcmp(sequence, "roll-battery")) {
        alive_fixture("settings.battery"); parity_advance(1200);
        parity_status.battery_pct = 79; parity_status.battery_detail.battery_pct = 79;
        parity_status.battery_detail.temperature_c = 26;
        orb_shell_update(parity_shell, &parity_status);
    } else if (!strcmp(sequence, "apps-arrival")) {
        parity_shell_create(true); alive_fresh(ORB_SHELL_APPS);
    } else if (!strcmp(sequence, "compass-arrival")) {
        parity_shell_create(true); alive_fresh(ORB_SHELL_COMPASS);
    } else if (!strcmp(sequence, "receipt")) {
        alive_fixture("receipt.pending");
        orb_answer_t answer = {.card = "parity-card", .option = "deny"};
        parity_check(orb_model_complete_answer("parity-card", &answer, true, NULL), "delivered answer outcome");
    } else if (!strcmp(sequence, "agent-step-check")) {
        alive_agent_done();
    } else if (!strcmp(sequence, "update-ready")) {
        alive_fixture("updates.mandatory.verifying");
        parity_status.ota = ORB_SHELL_OTA_READY; orb_shell_update(parity_shell, &parity_status);
    } else if (!strcmp(sequence, "ota-follow")) {
        alive_fixture("updates.mandatory.downloading"); parity_advance(1200);
        parity_status.progress = 78; orb_shell_update(parity_shell, &parity_status);
    } else if (!strcmp(sequence, "empty-state")) {
        parity_shell_create(false); alive_fresh(ORB_SHELL_NOTIFICATIONS);
    } else if (!strcmp(sequence, "breathe-running")) {
        alive_fixture("home-scenes"); parity_check(orb_home_scene("evening"), "real named scene wait");
    } else if (!strcmp(sequence, "breathe-turning-off")) {
        alive_fixture("home-room"); parity_check(orb_home_room_off("living"), "real named All off wait");
    } else if (!strcmp(sequence, "breathe-dot")) {
        alive_fixture("dot.ask");
        const orb_message_t message = {.target = "dot", .text = "What is on tomorrow?"};
        orb_model_complete_message_receipt(&message, ORB_SEND_DELIVERED, "Delivered", NULL);
    } else if (!strcmp(sequence, "breathe-checking")) {
        alive_fixture("updates.ordinary");
        parity_check(parity_tap_text("Check now"), "real check-for-updates wait");
    } else {
        static const struct { const char *sequence, *fixture; } waits[] = {
            {"breathe-loading", "home-rooms-loading"},
            {"breathe-transcribing", "voice.transcribing"},
            {"breathe-buffering", "spoken.buffering"},
            {"breathe-captions", "spoken.empty"},
            {"breathe-saving", "settings.sound.saving"},
            {"breathe-temp-saving", "settings.temperature.pending"},
            {"time-pending-still", "timer-pending"},
            {"breathe-weather-loading", "weather.loading"},
            {"breathe-control-loading", "control.home.loading"},
            {"breathe-boot-starting", "system.starting"},
            {"breathe-setup-code", "system.setup.loading"},
            {"breathe-pairing-code", "system.pairing.loading"},
            {"breathe-sources-saving", "settings.sources.pending"},
            {"activity-placeholder-still", "session.waiting"},
            {"breathe-syncing", "agents.loading"},
            {"breathe-idle-loading", "idle.loading"},
            {"breathe-starting", "voice.starting.no-text"},
            {"breathe-sending", "voice.sending"},
            {"breathe-queued", "send.queued"},
            {"breathe-time", "alarm-no-clock"},
            {"breathe-verifying", "updates.mandatory.verifying"},
            {"breathe-installing", "updates.mandatory.ready"},
        };
        for (unsigned i = 0; i < sizeof(waits) / sizeof(waits[0]); i++)
            if (!strcmp(sequence, waits[i].sequence)) { alive_fixture(waits[i].fixture); return true; }
        parity_check(false, "known Alive sequence");
    }
    return !strncmp(sequence, "breathe-", 8) || !strcmp(sequence, "update-ready");
}

static void alive_directory(const char *directory)
{
    DIR *existing = opendir(directory);
    if (existing) {
        struct dirent *entry;
        while ((entry = readdir(existing)))
            parity_check(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."), "capture directory must be empty");
        closedir(existing);
    } else parity_check(errno == ENOENT && mkdir(directory, 0755) == 0, "create capture directory");
}

int alive_os_capture(const char *sequence, const char *mode, const char *directory)
{
    alive_directory(directory);
    bool waiting = alive_prepare(sequence);
    lv_refr_now(parity_display);
    lv_display_add_event_cb(parity_display, alive_frames, LV_EVENT_ALL, NULL);
    char path[1024]; snprintf(path, sizeof(path), "%s/frames.csv", directory);
    FILE *metrics = fopen(path, "w"); parity_check(metrics != NULL, "open Alive metrics");
    fprintf(metrics, "frame,virtual_ms,flushed_pixels,rendered_frames,max_frame_pixels,motion_clock\n");
    uint32_t start = parity_now, last_damage = 0, peak = 0;
    uint64_t total_pixels = 0; unsigned total_frames = 0;
    unsigned samples = waiting ? 240 : 180;
    for (unsigned sample = 0; sample <= samples + (waiting ? 1 : 0); sample++) {
        uint32_t elapsed = sample <= samples ? sample * 1000 / 60 : 32000;
        s_alive_pixels = s_alive_frames = s_alive_peak = 0;
        parity_advance(start + elapsed - parity_now);
        if (s_alive_pixels) last_damage = s_alive_last_damage - start;
        total_pixels += s_alive_pixels; total_frames += s_alive_frames; peak = LV_MAX(peak, s_alive_peak);
        snprintf(path, sizeof(path), "%s/%04u.png", directory, sample);
        parity_check(sim_display_save_png(path), "save Alive virtual sample");
        fprintf(metrics, "%u,%u,%u,%u,%u,%d\n", sample, elapsed, s_alive_pixels,
            s_alive_frames, s_alive_peak, orb_kit_motion_timer_running());
    }
    parity_check(fclose(metrics) == 0, "close Alive metrics");
    snprintf(path, sizeof(path), "%s/summary.csv", directory);
    metrics = fopen(path, "w"); parity_check(metrics != NULL, "open Alive summary");
    fprintf(metrics, "sequence,profile,rendered_frames,mean_pixels_per_frame,max_pixels_per_frame,last_damage_ms,motion_clock\n");
    fprintf(metrics, "%s,%s,%u,%.2f,%u,%u,%d\n", sequence, mode, total_frames,
        total_frames ? (double)total_pixels / total_frames : 0., peak, last_damage, orb_kit_motion_timer_running());
    parity_check(fclose(metrics) == 0, "close Alive summary");
    printf("alive %-24s %-14s frames=%u mean_px=%.2f max_px=%u last_damage_ms=%u clock=%d\n",
        sequence, mode, total_frames, total_frames ? (double)total_pixels / total_frames : 0.,
        peak, last_damage, orb_kit_motion_timer_running());
    parity_check(!orb_kit_motion_timer_running(), "Alive capture must finish with a stopped motion clock");
    bool placeholder = !strcmp(sequence, "activity-placeholder-still") || !strcmp(sequence, "time-pending-still");
    if (strcmp(mode, "reduce-motion") && !placeholder) {
        parity_check(total_frames > 0, "real Alive cause must produce intermediate frames");
        if (!waiting && strcmp(sequence, "volume-hud-follow"))
            parity_check(last_damage <= 450, "finite Alive effect exceeded its arrival deadline");
    }
    return 0;
}
