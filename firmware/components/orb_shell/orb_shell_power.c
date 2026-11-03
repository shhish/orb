#include "orb_shell_power.h"
#include "orb_faces_host.h"
#include "orb_time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_feedback.h"
#include "orb_model.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "src/misc/lv_text_private.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static void *calm_alloc(size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(1, size);
#endif
}

/* HIG §§6/7: the privacy surface is retained above shell AND offline apps.
 * Only public glance summaries enter it. No snapshot, allocation or
 * font change occurs in a transition frame. All labels use fixed storage. */
struct orb_shell_calm {
    orb_shell_t *shell;
    lv_obj_t *root;
    orb_app_host_t *faces;
    lv_obj_t *notice, *notice_label;
    lv_point_t notice_start;
    orb_power_controller_t controller;
    orb_shell_snapshot_t status;
    orb_model_snapshot_t *model;
    uint64_t model_revision, painted_ms, now_ms;
    uint32_t tick, font_revision, presented_revision;
    unsigned waiting;
    bool fence_required, enabled, contrast, locked, alert, consume_button[ORB_BUTTON_COUNT];
    uint8_t wake_brightness;
    bool initialized, drawn_visible;
    bool low_notified, critical_notified, notice_captured, notice_back, notice_retiring, notice_open_battery;
    unsigned battery_notice;
    orb_power_snapshot_t previous;
    char notice_text[80];
};

static uint64_t clock_now(orb_shell_calm_t *c)
{
    uint32_t tick = lv_tick_get();
    c->now_ms += (uint32_t)(tick - c->tick);
    c->tick = tick;
    return c->now_ms;
}

static void lock_at(orb_shell_calm_t *c, uint64_t now)
{
    bool was_locked = c->locked;
    c->locked = true;
    c->controller.display_locked = true;
    if (c->controller.state == ORB_POWER_ALWAYS_ON || c->controller.state == ORB_POWER_SLEEP) return;
    /* Reuse the controller's retained desk transition without changing the
     * owner's Quiet preference. Lock and automatic AOD share this latch. */
    bool quiet = c->controller.quiet;
    orb_power_set_quiet(&c->controller, true, now);
    c->controller.quiet = quiet;
    /* A status/IMU update may reevaluate the ordinary Dimmed deadline before
     * the adapter relatches AOD. Do not replay Rest for an already locked face. */
    if (was_locked) c->controller.moment = ORB_POWER_MOMENT_NONE;
}

static orb_power_input_result_t wake_at(orb_shell_calm_t *c, orb_power_wake_t reason, uint64_t now)
{
    if (c->alert && (reason == ORB_POWER_WAKE_TOUCH || reason == ORB_POWER_WAKE_PWR))
        return (orb_power_input_result_t){.consume_input = true};
    orb_power_snapshot_t before = orb_power_get_snapshot(&c->controller);
    orb_power_input_result_t result = orb_power_wake(&c->controller, reason, now);
    if (result.accepted) {
        if (!c->alert || reason == ORB_POWER_WAKE_TOUCH || reason == ORB_POWER_WAKE_PWR) c->locked = false;
        if (reason == ORB_POWER_WAKE_PWR && before.brightness_pct <= 10) {
            orb_power_preferences_t preferences = c->controller.preferences;
            preferences.brightness_pct = c->wake_brightness;
            orb_power_set_preferences(&c->controller, &preferences, now);
            result.consume_input = true;
        }
    }
    return result;
}

static void bare(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
}

#include "orb_shell_glance.inc"

/* HIG §§7/8 and Design 2.0 Lock: reuse the selected public face, keeping
 * Calendar/weather but never session/card text. The controller retains wake
 * fencing, brightness and discrete burn-in shifts; faces owns all geometry. */
static void present(orb_shell_calm_t *c)
{
    if (!c || !c->enabled) return;
    orb_power_snapshot_t power = orb_power_get_snapshot(&c->controller);
    orb_shell_display_active(c->shell, power.state == ORB_POWER_ACTIVE && !power.redact);
    bool visible = power.redact || power.state != ORB_POWER_ACTIVE ||
        power.moment == ORB_POWER_MOMENT_WAKE;
    bool structural = !c->initialized || c->drawn_visible != visible ||
        c->previous.state != power.state || c->previous.moment != power.moment ||
        c->font_revision != (uint32_t)lv_subject_get_int(orb_fonts_subject());
    notice_present(c);
    if (visible && (structural || power.now_ms - c->painted_ms >= 1000)) {
        orb_faces_snapshot_t snapshot = {.now = time(NULL),
            .battery_percent = c->status.battery_pct, .charging = c->status.charging};
        if (!orb_shell_face_snapshot(c->shell, &snapshot)) {
            /* Setup can precede workspace creation. Retain only known public
             * clock/power data until the normal shared summary is available. */
            snprintf(snapshot.clock, sizeof(snapshot.clock), "%s", c->status.clock_time);
            char *period = strchr(snapshot.clock, ' '); /* Faces never show AM/PM. */
            if (period) *period = 0;
            snapshot.usage_percent[0] = snapshot.usage_percent[1] = -1;
        }
        orb_faces_host_update(c->faces, &snapshot);
        c->painted_ms = power.now_ms;
    }
    lv_obj_set_hidden(c->root, !visible);
    if (visible) lv_obj_move_foreground(c->root);
    /* AOD's only motion is the existing discrete minute protection. Wake/rest
     * use the controller's bounded 150/250/400 ms cues, never move face content. */
    lv_obj_set_pos(orb_app_faces_root(orb_app_host_current(c->faces)), power.shift_x, power.shift_y);
    float opacity = power.panel_on ? 1.f : 0.f;
    float progress = power.moment_duration_ms ?
        LV_MIN(1.f, (float)power.moment_elapsed_ms / power.moment_duration_ms) : 1.f;
    if (power.moment == ORB_POWER_MOMENT_SLEEP) opacity *= 1.f - progress;
    else if (power.moment == ORB_POWER_MOMENT_WAKE) {
        /* An authorized wake from a dark panel reveals the retained task
         * through black. Replaying the already-lit public face at p=0 would
         * flash its full ink before the display's safe first frame. */
        if (!power.redact) opacity *= power.wake_from_sleep ? 0.f : 1.f - progress;
        else if (power.wake_from_sleep) opacity *= progress;
    }
    /* Keep the privacy backing opaque throughout redacted/sleep wakes. Only
     * deliberate authorized wake can reveal the retained task beneath it. */
    lv_opa_t backing = power.moment == ORB_POWER_MOMENT_WAKE && !power.redact ?
        (lv_opa_t)lrintf(255 * (1.f - progress)) : LV_OPA_COVER;
    lv_opa_t ink = (lv_opa_t)lrintf(255 * opacity);
    if (lv_obj_get_style_bg_opa(c->root, 0) != backing)
        lv_obj_set_style_bg_opa(c->root, backing, 0);
    if (lv_obj_get_style_opa(orb_app_faces_root(orb_app_host_current(c->faces)), 0) != ink)
        lv_obj_set_style_opa(orb_app_faces_root(orb_app_host_current(c->faces)), ink, 0);
    c->previous = power;
    c->drawn_visible = visible;
    c->font_revision = (uint32_t)lv_subject_get_int(orb_fonts_subject());
    c->initialized = true;
}

static void input_event(lv_event_t *event)
{
    orb_shell_calm_t *c = lv_event_get_user_data(event);
    if (!c->enabled || lv_event_get_code(event) != LV_EVENT_PRESSED) return;
    lv_indev_t *input = lv_event_get_target(event);
    bool synthetic = orb_input_is_synthetic(input);
    /* Alert actions have their own opaque presentation above the retained
     * privacy face. A Stop/Snooze touch is not intent to reveal that task. */
    if (c->alert) {
        if (c->fence_required && c->presented_revision != c->controller.revision) {
            lv_indev_wait_release(input);
            lv_indev_stop_processing(input);
            lv_event_stop_processing(event);
        }
        /* Account after the existing fence decision so bookkeeping cannot
         * change ownership of this alert contact when a moment settles. */
        if (!synthetic) (void)orb_power_note_activity(&c->controller, clock_now(c));
        return;
    }
    /* A devtools command wakes explicitly before injecting. An active
     * synthetic pointer still renews activity, including the second indev on
     * the board; a raw synthetic contact cannot reveal a locked surface. */
    bool active = c->controller.state == ORB_POWER_ACTIVE && !c->controller.redact;
    orb_power_input_result_t result = synthetic && !active ? (orb_power_input_result_t){0} :
        wake_at(c, ORB_POWER_WAKE_TOUCH, clock_now(c));
    if (result.accepted) lv_display_trigger_activity(lv_indev_get_display(input));
    bool waiting_frame = c->fence_required && c->presented_revision != c->controller.revision;
    if (result.consume_input || !result.accepted || waiting_frame) {
        /* LVGL stops this contact before the task's click/hold dispatch. */
        lv_indev_wait_release(input);
        lv_indev_stop_processing(input);
        lv_event_stop_processing(event);
    }
    present(c);
}

orb_shell_calm_t *orb_shell_calm_create(orb_shell_t *shell, lv_obj_t *root)
{
    orb_shell_calm_t *c = calm_alloc(sizeof(*c));
    if (!c) return NULL;
    c->model = calm_alloc(sizeof(*c->model));
    if (!c->model) { free(c); return NULL; }
    c->shell = shell;
    c->status.battery_pct = -1;
    c->now_ms = c->tick = lv_tick_get();
    c->wake_brightness = 70;
    orb_power_init(&c->controller, c->now_ms);
    c->root = lv_obj_create(lv_display_get_layer_top(lv_obj_get_display(root)));
    bare(c->root);
    lv_obj_set_size(c->root, 466, 466);
    lv_obj_set_clickable(c->root, true);
    lv_obj_set_style_bg_color(c->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c->root, LV_OPA_COVER, 0);
    c->faces = orb_faces_host_public_create(c->root);
    if (!c->faces) { lv_obj_delete(c->root); free(c->model); free(c); return NULL; }
    lv_obj_set_hidden(c->root, true);
    return c;
}

void orb_shell_calm_destroy(orb_shell_calm_t *c)
{
    if (!c) return;
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input))
        lv_indev_remove_event_cb_with_user_data(input, input_event, c);
    orb_app_host_destroy(c->faces);
    lv_obj_delete(c->root);
    lv_async_call_cancel(notice_dispose, c);
    if (c->notice) lv_obj_delete(c->notice);
    free(c->model);
    free(c);
}

void orb_shell_calm_status(orb_shell_calm_t *c, const orb_shell_snapshot_t *status)
{
    if (!c || !status) return;
    c->status = *status;
    notice_update(c);
    if (c->enabled) orb_shell_calm_poll(c);
}

void orb_shell_calm_poll(orb_shell_calm_t *c)
{
    if (!c) return;
    notice_present(c);
    if (!c->enabled) return;
    uint64_t now = clock_now(c);
    /* The board creates its developer pointer after the physical one. Attach
     * to every live pointer, including late arrivals, without retaining freed
     * indev descriptors or registering a duplicate callback on each poll. */
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) continue;
        bool attached = false;
        for (uint32_t i = 0; i < lv_indev_get_event_count(input); i++) {
            lv_event_dsc_t *dsc = lv_indev_get_event_dsc(input, i);
            if (lv_event_dsc_get_cb(dsc) == input_event && lv_event_dsc_get_user_data(dsc) == c) {
                attached = true;
                break;
            }
        }
        if (!attached) lv_indev_add_event_cb(input, input_event, LV_EVENT_PRESSED, c);
        /* A drag can cancel LVGL's semantic PRESSING events once navigation
         * owns it. Observe the raw contact so its activity lease lasts until
         * lift. Never let an already-held finger reveal a new lock/sleep. */
        if (c->controller.state == ORB_POWER_ACTIVE && !c->controller.redact &&
            lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED)
            (void)orb_power_wake(&c->controller, ORB_POWER_WAKE_TOUCH, now);
    }
    bool attention = false;
    if (orb_model_snapshot_if_changed(c->model_revision, c->model)) {
        unsigned before = c->waiting;
        c->model_revision = c->model->revision;
        c->waiting = 0;
        for (size_t i = 0; i < c->model->card_count; i++) {
            const orb_card_t *card = &c->model->cards[i];
            if (!card->answered && card->kind != ORB_CARD_MESSAGE) c->waiting++;
        }
        attention = c->waiting > before;
    }
    bool quiet = c->controller.quiet;
    /* The display-lock flag suppresses work's display lease independently of
     * the owner's Quiet preference, which still governs automatic wakes. */
    if (c->alert) c->controller.quiet = false;
    uint32_t inhibitors = c->controller.inhibitors & (ORB_POWER_INHIBIT_PLAYBACK | ORB_POWER_INHIBIT_DMA);
    if (c->model->voice.phase == ORB_VOICE_STARTING || c->model->voice.phase == ORB_VOICE_RECORDING ||
        c->model->voice.phase == ORB_VOICE_TRANSCRIBING || c->model->voice.phase == ORB_VOICE_CONFIRMING ||
        c->model->voice.phase == ORB_VOICE_SENDING || c->model->voice.phase == ORB_VOICE_ERROR)
        inhibitors |= ORB_POWER_INHIBIT_VOICE;
    if (c->status.ota == ORB_SHELL_OTA_DOWNLOADING || c->status.ota == ORB_SHELL_OTA_VERIFYING ||
        c->status.ota == ORB_SHELL_OTA_READY) inhibitors |= ORB_POWER_INHIBIT_OTA;
    const orb_app_manifest_t *app = orb_apps_active();
    /* Time's service owns its deadlines independently of admission/display
     * ticks. Viewing a timer is reading, unlike continuous game interaction;
     * an actual ringing alert still owns its separate keep-awake lease. */
    if ((app && strcmp(app->id, "time") && !orb_apps_paused()) ||
        orb_shell_time_alert_active(c->shell))
        inhibitors |= ORB_POWER_INHIBIT_APP;
    /* Apply new work before any preference/battery/wake idle evaluation.
     * Quiet suppresses display leases while retaining every system blocker. */
    orb_power_set_inhibitors(&c->controller, inhibitors, now);
    orb_power_set_battery(&c->controller, c->status.battery_pct, c->status.usb, c->status.charging, now);
    if (attention) wake_at(c, ORB_POWER_WAKE_ATTENTION, now);
    orb_power_preferences_t preferences = c->controller.preferences;
    preferences.reduce_motion = orb_ui_reduce_motion();
    orb_power_set_preferences(&c->controller, &preferences, now);
    orb_power_tick(&c->controller, now);
    c->controller.quiet = quiet;
    if (c->controller.state == ORB_POWER_ALWAYS_ON && !c->alert) lock_at(c, now);
    present(c);
}

void orb_shell_calm_lock(orb_shell_calm_t *c)
{
    if (!c) return;
    orb_shell_power_enable(c->shell, true);
    lock_at(c, clock_now(c));
    present(c);
}

void orb_shell_calm_alert(orb_shell_calm_t *c, bool presenting)
{
    if (!c || c->alert == presenting) return;
    c->alert = presenting;
    if (!c->enabled) return;
    uint64_t now = clock_now(c);
    if (presenting) {
        /* Alarms/timers bypass Quiet's ordinary attention-wake suppression,
         * but retain automatic-wake redaction and the underlying lock latch.
         * The alert is an explicit lease, not manual viewing intent. */
        bool quiet = c->controller.quiet;
        c->controller.quiet = false;
        c->controller.have_attention = false;
        orb_power_set_inhibitors(&c->controller, c->controller.inhibitors | ORB_POWER_INHIBIT_APP, now);
        (void)wake_at(c, ORB_POWER_WAKE_ATTENTION, now);
        c->controller.quiet = quiet;
    } else if (c->locked) {
        orb_power_set_inhibitors(&c->controller, c->controller.inhibitors & ~ORB_POWER_INHIBIT_APP, now);
        lock_at(c, now);
    }
    present(c);
}

bool orb_shell_calm_button(orb_shell_calm_t *c, const orb_button_event_t *event)
{
    if (event && event->button == ORB_BUTTON_BOOT &&
        (event->gesture == ORB_BUTTON_SHORT || event->gesture == ORB_BUTTON_DOUBLE) &&
        orb_shell_calm_dismiss_notice(c)) return true;
    if (!c || !c->enabled || !event || event->synthetic || event->button >= ORB_BUTTON_COUNT) return false;
    /* BOOT remains Back / hold-to-talk even from a dark presentation. It
     * expresses deliberate intent but cannot answer an approval; only touch
     * and PWR have the wake-contact consumption contract. */
    if (event->button == ORB_BUTTON_BOOT) {
        if (event->gesture == ORB_BUTTON_DOWN && !c->alert) {
            wake_at(c, ORB_POWER_WAKE_PWR, clock_now(c));
            present(c);
        }
        return false;
    }
    if (event->gesture == ORB_BUTTON_DOWN) {
        /* An opaque alert owns this whole press, just as it owns touch.
         * Physical activity must never authorize the retained private task. */
        if (c->alert) {
            (void)orb_power_note_activity(&c->controller, clock_now(c));
            c->consume_button[event->button] = true;
            present(c);
            return true;
        }
        orb_power_input_result_t result = wake_at(c, ORB_POWER_WAKE_PWR, clock_now(c));
        c->consume_button[event->button] = result.consume_input || !result.accepted ||
            (c->fence_required && c->presented_revision != c->controller.revision);
        present(c);
    }
    bool consume = c->consume_button[event->button];
    /* SHORT/DOUBLE may follow UP after the recognizer's double-press window.
     * Keep ownership frozen until the NEXT physical DOWN, not merely UP. */
    return consume;
}

void orb_shell_power_enable(orb_shell_t *shell, bool enabled)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    if (!c) return;
    if (enabled && !c->enabled) {
        orb_power_init(&c->controller, clock_now(c));
        c->locked = false;
        c->presented_revision = 0;
        orb_power_set_battery(&c->controller, c->status.battery_pct, c->status.usb, c->status.charging, c->now_ms);
    }
    c->enabled = enabled;
    if (!enabled) lv_obj_set_hidden(c->root, true);
    else orb_shell_calm_poll(c);
}

void orb_shell_power_advance(orb_shell_t *shell, uint64_t now_ms)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    if (!c || now_ms < c->controller.now_ms) return;
    c->tick = lv_tick_get();
    c->now_ms = now_ms;
    orb_shell_calm_poll(c);
}

static void power_imu(orb_shell_t *shell, float x, float y, float z, uint64_t now_ms, bool minimal)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    if (!c || !c->enabled) return;
    if (!now_ms) now_ms = clock_now(c);
    if (now_ms < c->controller.now_ms) return;
    c->tick = lv_tick_get(); c->now_ms = now_ms;
    uint32_t revision = c->controller.revision;
    orb_power_imu_sample(&c->controller, x, y, z, now_ms);
    if (c->controller.state == ORB_POWER_ACTIVE && !c->alert) c->locked = false;
    if (c->locked && !c->alert) lock_at(c, now_ms);
    /* Sleeping IMU samples prove detector dwell; they must not repeatedly
     * write LVGL styles or move the retained face above the workspace. */
    if (!minimal || c->controller.revision != revision) present(c);
}

void orb_shell_power_imu(orb_shell_t *shell, float x, float y, float z, uint64_t now_ms)
{
    power_imu(shell, x, y, z, now_ms, false);
}

void orb_shell_power_imu_sleep(orb_shell_t *shell, float x, float y, float z, uint64_t now_ms)
{
    power_imu(shell, x, y, z, now_ms, true);
}

orb_power_snapshot_t orb_shell_power_snapshot(const orb_shell_t *shell)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    return orb_power_get_snapshot(c && c->enabled ? &c->controller : NULL);
}

uint32_t orb_shell_power_inhibitors(const orb_shell_t *shell)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    return c && c->enabled ? c->controller.inhibitors : ORB_POWER_INHIBIT_APP;
}

void orb_shell_power_poll(orb_shell_t *shell, const orb_shell_snapshot_t *status)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    if (status) orb_shell_calm_status(c, status);
    else orb_shell_calm_poll(c);
}

orb_power_preferences_t orb_shell_power_preferences(const orb_shell_t *shell)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    return c ? c->controller.preferences : orb_power_default_preferences();
}

void orb_shell_set_power_preferences(orb_shell_t *shell, const orb_power_preferences_t *preferences)
{
    orb_shell_power_enable(shell, true);
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    if (!c) return;
    if (preferences && preferences->brightness_pct > 10) c->wake_brightness = preferences->brightness_pct;
    orb_power_set_preferences(&c->controller, preferences, clock_now(c));
    if (c->locked && !c->alert) lock_at(c, c->controller.now_ms);
    present(c);
}

void orb_shell_set_quiet(orb_shell_t *shell, bool quiet)
{
    orb_shell_power_enable(shell, true);
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    if (!c) return;
    orb_power_set_quiet(&c->controller, quiet, clock_now(c));
    c->locked = quiet;
    present(c);
}

bool orb_shell_quiet(const orb_shell_t *shell)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    return c && c->controller.quiet;
}

void orb_shell_power_set_contrast(orb_shell_t *shell, bool contrast)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    if (!c || c->contrast == contrast) return;
    c->contrast = contrast;
    c->initialized = false;
    present(c);
}

void orb_shell_power_display_ready(orb_shell_t *shell, uint32_t revision, bool ready)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    if (!c) return;
    c->fence_required = true;
    c->presented_revision = ready ? revision : 0;
}

void orb_shell_power_sleep(orb_shell_t *shell)
{
    orb_shell_power_enable(shell, true);
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    if (!c) return;
    /* Explicit display sleep leaves CPU/radio and pending work running. */
    orb_power_request_sleep(&c->controller, clock_now(c));
    present(c);
}

orb_power_input_result_t orb_shell_power_wake(orb_shell_t *shell, orb_power_wake_t reason)
{
    orb_shell_calm_t *c = orb_shell_calm_context(shell);
    if (!c || !c->enabled) return (orb_power_input_result_t){0};
    orb_power_input_result_t result = wake_at(c, reason, clock_now(c));
    if (c->fence_required && c->presented_revision != c->controller.revision)
        result.consume_input = true;
    present(c);
    return result;
}
