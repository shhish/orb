/* Deterministic shell previews using the same banded renderer as Orb.
 * All names/codes and session data are synthetic, never captured credentials. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_shell.h"
#include "orb_shell_services.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "orb_time.h"
#include "sim_display.h"

static uint32_t s_now;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static uint32_t tick(void) { return s_now; }

static const struct { const char *name; orb_shell_surface_t surface; } s_surfaces[] = {
    {"home-app", ORB_SHELL_HOME}, {"home-face", ORB_SHELL_FACE},
    {"home-dial-rotary", ORB_SHELL_HOME},
    {"home-controls", ORB_SHELL_CONTROL_CENTER}, {"home-stack", ORB_SHELL_STACK},
    {"home-doorbell", ORB_SHELL_CARD},
    {"home-controls-unavailable", ORB_SHELL_CONTROL_CENTER},
    {"home-controls-waiting", ORB_SHELL_CONTROL_CENTER},
    {"home", ORB_SHELL_FACE}, {"face", ORB_SHELL_FACE}, {"demo", ORB_SHELL_FACE},
    {"face-owner", ORB_SHELL_FACE}, {"face-owner-waiting", ORB_SHELL_FACE},
    {"face-owner-offline", ORB_SHELL_FACE},
    {"apps", ORB_SHELL_APPS}, {"apps-next", ORB_SHELL_APPS},
    {"agents", ORB_SHELL_AGENTS}, {"agents-next", ORB_SHELL_AGENTS},
    {"agents-idle", ORB_SHELL_SESSIONS}, {"agents-idle-row", ORB_SHELL_SESSIONS},
    {"idle-loading", ORB_SHELL_IDLE_SESSIONS}, {"idle-page", ORB_SHELL_IDLE_SESSIONS},
    {"idle-more", ORB_SHELL_IDLE_SESSIONS}, {"idle-end", ORB_SHELL_IDLE_SESSIONS},
    {"idle-empty", ORB_SHELL_IDLE_SESSIONS}, {"idle-error", ORB_SHELL_IDLE_SESSIONS},
    {"idle-more-pending", ORB_SHELL_IDLE_SESSIONS}, {"idle-more-error", ORB_SHELL_IDLE_SESSIONS},
    {"idle-detail", ORB_SHELL_IDLE_SESSIONS}, {"idle-reply", ORB_SHELL_IDLE_SESSIONS},
    {"session-outside-focus", ORB_SHELL_SESSION},
    {"sessions-settings", ORB_SHELL_SESSION_SETTINGS}, {"sessions-codex", ORB_SHELL_SESSION_SETTINGS},
    {"sessions-dot", ORB_SHELL_SESSION_SETTINGS}, {"sessions-recent", ORB_SHELL_SESSION_SETTINGS},
    {"sessions-pending", ORB_SHELL_SESSION_SETTINGS}, {"sessions-error", ORB_SHELL_SESSION_SETTINGS},
    {"sessions-off", ORB_SHELL_SESSION_SETTINGS},
    {"sessions-touched", ORB_SHELL_SESSION_SETTINGS},
    {"sessions-recent-off", ORB_SHELL_SESSION_SETTINGS}, {"sessions-recent-five", ORB_SHELL_SESSION_SETTINGS},
    {"sessions-recent-thirty", ORB_SHELL_SESSION_SETTINGS},
    {"session", ORB_SHELL_SESSION}, {"session-summary", ORB_SHELL_SESSION}, {"stack", ORB_SHELL_STACK},
    {"notifications", ORB_SHELL_NOTIFICATIONS},
    {"notifications-received", ORB_SHELL_NOTIFICATIONS}, {"notifications-cleared", ORB_SHELL_NOTIFICATIONS},
    {"notifications-undo", ORB_SHELL_NOTIFICATIONS},
    {"agents-empty", ORB_SHELL_AGENTS}, {"sessions-empty", ORB_SHELL_SESSIONS}, {"dot-empty", ORB_SHELL_DOT},
    {"stack-empty", ORB_SHELL_STACK}, {"notifications-empty", ORB_SHELL_NOTIFICATIONS},
    {"stack-day", ORB_SHELL_STACK}, {"stack-date", ORB_SHELL_STACK},
    {"stack-timer", ORB_SHELL_STACK}, {"stack-stopwatch", ORB_SHELL_STACK}, {"stack-usage", ORB_SHELL_STACK},
    {"voice-idle-session", ORB_SHELL_VOICE},
    {"control-center", ORB_SHELL_CONTROL_CENTER}, {"usage", ORB_SHELL_USAGE}, {"charging", ORB_SHELL_CHARGING},
    {"control-silent", ORB_SHELL_CONTROL_CENTER}, {"control-motion", ORB_SHELL_CONTROL_CENTER},
    {"settings", ORB_SHELL_SETTINGS}, {"settings-next", ORB_SHELL_SETTINGS},
    {"display", ORB_SHELL_DISPLAY},
    {"units", ORB_SHELL_UNITS}, {"temperature-settings", ORB_SHELL_TEMPERATURE},
    {"accessibility", ORB_SHELL_ACCESSIBILITY}, {"access-text-size", ORB_SHELL_ACCESSIBILITY},
    {"access-text-selected", ORB_SHELL_ACCESSIBILITY}, {"access-list-motion", ORB_SHELL_ACCESSIBILITY},
    {"access-list-transparency", ORB_SHELL_ACCESSIBILITY}, {"access-list-contrast", ORB_SHELL_ACCESSIBILITY},
    {"sound", ORB_SHELL_SOUND}, {"sound-volume", ORB_SHELL_SOUND}, {"sound-quiet", ORB_SHELL_SOUND},
    {"sound-start", ORB_SHELL_SOUND}, {"sound-end", ORB_SHELL_SOUND},
    {"access-bold", ORB_SHELL_ACCESSIBILITY}, {"access-motion", ORB_SHELL_ACCESSIBILITY},
    {"access-transparency", ORB_SHELL_ACCESSIBILITY}, {"access-contrast", ORB_SHELL_ACCESSIBILITY},
    {"about", ORB_SHELL_ABOUT}, {"about-licenses", ORB_SHELL_ABOUT},
    {"about-device-id", ORB_SHELL_ABOUT}, {"about-disclosure", ORB_SHELL_ABOUT}, {"about-licenses-end", ORB_SHELL_ABOUT}, {"about-licenses-middle", ORB_SHELL_ABOUT},
    {"dot", ORB_SHELL_DOT}, {"voice", ORB_SHELL_VOICE},
    {"voice-starting", ORB_SHELL_VOICE}, {"voice-recording", ORB_SHELL_VOICE},
    {"voice-transcribing", ORB_SHELL_VOICE}, {"voice-confirming", ORB_SHELL_VOICE},
    {"voice-sending", ORB_SHELL_VOICE}, {"voice-error", ORB_SHELL_VOICE},
    {"voice-no-speech", ORB_SHELL_VOICE}, {"voice-network", ORB_SHELL_VOICE},
    {"voice-unsupported", ORB_SHELL_VOICE},
    {"message-delivered", ORB_SHELL_VOICE}, {"message-timeout", ORB_SHELL_VOICE},
    {"play", ORB_SHELL_PLAY}, {"play-registered", ORB_SHELL_PLAY}, {"updates", ORB_SHELL_UPDATES},
};

static bool scenario(orb_shell_snapshot_t *m, const char *name)
{
    *m = (orb_shell_snapshot_t){
        .net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .usb = true, .brightness = 70,
    };
    m->demo = !strcmp(name, "demo");
    snprintf(m->device_name, sizeof(m->device_name), "Orb");
    snprintf(m->device_id, sizeof(m->device_id), "orb-0123456789ab");
    snprintf(m->hub_name, sizeof(m->hub_name), "Mac mini");
    snprintf(m->ssid, sizeof(m->ssid), "Home Wi-Fi");
    snprintf(m->clock_time, sizeof(m->clock_time), "10:09");
    if (!strcmp(name, "home-stack")) snprintf(m->clock_time, sizeof(m->clock_time), "23:10");
    snprintf(m->clock_date, sizeof(m->clock_date), "Tuesday, October 27");
    if (!strcmp(name, "charging")) m->charging = true;
    if (!strcmp(name, "stack-empty")) m->clock_date[0] = '\0';
    snprintf(m->firmware_version, sizeof(m->firmware_version), "0.4.0 · w2-shell");
    if (!strcmp(name, "face-owner-offline")) m->link = ORB_SHELL_LINK_OFFLINE;
    for (size_t i = 0; i < sizeof(s_surfaces) / sizeof(s_surfaces[0]); i++) {
        if (!strcmp(name, s_surfaces[i].name)) return true;
    }
    if (!strcmp(name, "approval") || !strcmp(name, "choice") ||
        !strcmp(name, "message") || !strcmp(name, "attention") ||
        !strcmp(name, "approval-actions") || !strncmp(name, "choice-options", 14) ||
        !strncmp(name, "answer-", 7) || !strcmp(name, "card-cancelled")) return true;
    if (!strncmp(name, "connection", 10) || !strncmp(name, "forget-wifi-confirm", 19) ||
        !strncmp(name, "unpair-confirm", 14) || !strncmp(name, "update-check-", 13)) return true;
    if (!strcmp(name, "setup") || !strcmp(name, "setup-retry") || !strcmp(name, "setup-more")) {
        m->net = !strcmp(name, "setup-retry") ? ORB_SHELL_NET_FAILED : ORB_SHELL_NET_PROVISIONING;
        m->link = ORB_SHELL_LINK_OFFLINE;
        snprintf(m->service_name, sizeof(m->service_name), "Orb-89AB");
        snprintf(m->pop, sizeof(m->pop), "12345678");
    } else if (!strcmp(name, "pairing") || !strcmp(name, "pairing-more")) {
        m->link = ORB_SHELL_LINK_PAIRING;
        snprintf(m->pairing_code, sizeof(m->pairing_code), "002310");
    } else if (!strcmp(name, "connecting")) {
        m->net = ORB_SHELL_NET_CONNECTING;
        m->link = ORB_SHELL_LINK_OFFLINE;
    } else if (!strcmp(name, "offline")) {
        m->link = ORB_SHELL_LINK_SEARCHING;
    } else if (!strcmp(name, "identify")) {
        m->identify = true;
    } else if (!strcmp(name, "service-error")) {
        snprintf(m->service_error, sizeof(m->service_error), "Connection unavailable");
    } else {
        snprintf(m->version, sizeof(m->version), "0.4.1");
        snprintf(m->notes, sizeof(m->notes), "Improvements for your Orb");
        if (!strcmp(name, "update")) m->ota = ORB_SHELL_OTA_OFFERED;
        else if (!strcmp(name, "downloading")) { m->ota = ORB_SHELL_OTA_DOWNLOADING; m->progress = 42; }
        else if (!strcmp(name, "verifying")) m->ota = ORB_SHELL_OTA_VERIFYING;
        else if (!strcmp(name, "ready")) m->ota = ORB_SHELL_OTA_READY;
        else if (!strcmp(name, "update-error")) {
            m->ota = ORB_SHELL_OTA_FAILED;
            snprintf(m->error, sizeof(m->error), "Connection interrupted");
        } else return false;
    }
    return true;
}

static void settle(orb_shell_t *shell)
{
    /* HIG §6: capture after a complete spring, not half-way through a launch. */
    for (unsigned i = 0; i < 48; i++) {
        s_now += 16;
        orb_shell_poll(shell);
        lv_timer_handler();
        lv_refr_now(s_display);
    }
}

static void select_scene(orb_shell_t *shell, const char *name)
{
    orb_model_snapshot_t model;
    if (!orb_model_snapshot(&model)) return;
    if (!strcmp(name, "voice-idle-session")) {
        orb_model_cancel_card("choice-next");
        orb_shell_select_session(shell, "codex-shell");
        orb_shell_navigate(shell, ORB_SHELL_VOICE); return;
    }
    if (!strcmp(name, "home-doorbell")) {
        orb_card_t card = {.id="home-doorbell-preview", .kind=ORB_CARD_ATTENTION,
            .title="Front Door", .body="Doorbell rang at 10:09", .priority=ORB_PRIORITY_HIGH,
            .options_count=1, .options={{.id="dismiss", .label="Dismiss", .role=ORB_OPTION_CANCEL}}};
        if (!orb_model_upsert_card(&card)) return;
        orb_shell_poll(shell); orb_shell_select_card(shell, card.id); return;
    }
    if (!strncmp(name, "home-controls", 13)) {
        if (!strcmp(name, "home-controls-unavailable")) {
            orb_home_snapshot_t *home = calloc(1, sizeof(*home));
            if (!home || !orb_home_snapshot(home)) { free(home); return; }
            for (size_t i = 0; i < home->accessory_count; i++) home->accessories[i].reachable = false;
            bool ok = orb_home_snapshot_chunk_available(home->rooms, home->room_count,
                home->accessories, home->accessory_count, home->scenes, home->scene_count, false, true, false);
            free(home); if (!ok) return;
        } else if (!strcmp(name, "home-controls-waiting")) orb_home_snapshot_discard();
        orb_shell_navigate(shell, ORB_SHELL_CONTROL_CENTER);
        return;
    }
    if (!strcmp(name, "session") || !strcmp(name, "session-summary") || !strcmp(name, "session-outside-focus")) {
        if (model.session_count) orb_shell_select_session(shell, model.sessions[0].id);
        return;
    }
    if (!strncmp(name, "answer-", 7) || !strcmp(name, "card-cancelled")) {
        orb_shell_select_card(shell, "choice-next");
        return;
    }
    const char *kinds[] = {"approval", "choice", "message", "attention"};
    for (unsigned kind = 0; kind < 4; kind++) {
        if (strcmp(name, kinds[kind]) &&
            !(kind == ORB_CARD_APPROVAL && !strcmp(name, "approval-actions")) &&
            !(kind == ORB_CARD_CHOICE && !strncmp(name, "choice-options", 14))) continue;
        for (size_t i = 0; i < model.card_count; i++) {
            if (model.cards[i].kind == (orb_card_kind_t)kind) {
                orb_shell_select_card(shell, model.cards[i].id);
                return;
            }
        }
        return;
    }
    for (size_t i = 0; i < sizeof(s_surfaces) / sizeof(s_surfaces[0]); i++) {
        if (!strcmp(name, s_surfaces[i].name)) {
            orb_shell_navigate(shell, s_surfaces[i].surface);
            return;
        }
    }
}

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    *data = s_input;
}

/* LVGL's one-line DOTS presentation may temporarily expose the shortened
 * display string. Resolve that visible prefix to the requested fixture text. */
static bool display_label_matches(const char *text, const char *value)
{
    if (!strcmp(text, value)) return true;
    const char *end = strstr(text, "...");
    if (!end) end = strstr(text, "…");
    size_t prefix = end ? (size_t)(end - text) : 0;
    return prefix >= 3 && strlen(value) > prefix && !strncmp(text, value, prefix);
}

static lv_obj_t *find_label(lv_obj_t *root, const char *value)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (orb_kit_rim_text_get(root) && !strcasecmp(orb_kit_rim_text_get(root), value)) return root;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *text = lv_label_get_text(root);
        size_t n = strlen(text), v = strlen(value);
        if (display_label_matches(text, value) || (n > v && text[n - v - 1] == ' ' && !strcmp(text + n - v, value))) return root;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, i), value);
        if (found) return found;
    }
    return NULL;
}

/* Captions can match a page title as well as an action (e.g. Choose).
 * A header is not a button; prefer a matching label with its own hit target. */
static lv_obj_t *find_control(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    /* The kit exposes app identity through its glyph, with a curved caption. */
    static const struct { const char *name, *glyph; } apps[] = {
        {"Agents", ORB_ICON_PROMPT}, {"Dot", ORB_ICON_CHAT}, {"Home", ORB_ICON_HOUSE},
        {"Time", ORB_ICON_TIMER}, {"Play", ORB_ICON_GAME_CONTROLLER},
        {"Updates", ORB_ICON_UPLOAD}, {"Settings", ORB_ICON_GEAR},
    };
    if (lv_obj_get_width(root) == ORB_KIT_DISC && lv_obj_get_height(root) == ORB_KIT_DISC &&
        lv_obj_is_clickable(root)) {
        for (unsigned i = 0; i < sizeof(apps) / sizeof(*apps); ++i)
            if (!strcmp(caption, apps[i].name) && find_label(root, apps[i].glyph)) return root;
    }
    /* Design 2.0 Face Apps is the kit56 mini, an ordinary LVGL object. */
    if (!strcmp(caption, "Apps") && lv_obj_is_clickable(root) &&
        lv_obj_get_width(root) == 56 && lv_obj_get_height(root) == 56) return root;
    if (lv_obj_check_type(root, &lv_label_class) && find_label(root, caption) == root) {
        if (lv_obj_is_clickable(root)) return root;
        for (lv_obj_t *obj = lv_obj_get_parent(root); obj; obj = lv_obj_get_parent(obj))
            if (lv_obj_check_type(obj, &lv_button_class) ||
                (lv_obj_is_clickable(obj) && !lv_obj_is_scrollable(obj) &&
                 (orb_glass_get_amount(obj) > 0 ||
                  ((intptr_t)lv_obj_get_user_data(obj) > 0 && (intptr_t)lv_obj_get_user_data(obj) < 2048)))) return obj;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_control(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}

static bool tap(orb_shell_t *shell, const char *caption)
{
    lv_obj_t *button = find_control(orb_shell_root(shell), caption);
    if (!button && !strcmp(caption, "Reply"))
        button = find_control(orb_shell_root(shell), orb_shell_surface(shell) == ORB_SHELL_SESSION ? ORB_ICON_MIC : "Reply by voice");
    if (!button) { fprintf(stderr, "flow: missing control '%s'\n", caption); return false; }
    lv_area_t box;
    lv_obj_get_coords(button, &box);
    lv_obj_t *row = button;
    orb_round_list_cache_stats_t cache;
    while (lv_obj_get_parent(row) && !orb_round_list_cache_stats(lv_obj_get_parent(row), &cache))
        row = lv_obj_get_parent(row);
    bool round_row = lv_obj_get_parent(row) != NULL;
    if (round_row && !orb_round_list_child_bounds(row, button, &box)) {
        fprintf(stderr, "flow: control '%s' has no presented row bounds\n", caption);
        return false;
    }
    s_input.point = (lv_point_t){(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
    if (round_row && !orb_round_list_hit_test(row, &s_input.point)) {
        fprintf(stderr, "flow: control '%s' is outside its visible viewport\n", caption);
        return false;
    }
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer);
    s_now += 32;
    lv_timer_handler();
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    settle(shell);
    return true;
}

static bool shot(orb_shell_t *shell, const char *directory, const char *name)
{
    char path[1024];
    int len = snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    if (len < 0 || (size_t)len >= sizeof(path)) return false;
    settle(shell);
    return sim_display_save_png(path);
}

/* Switches float beside cached row ink. Match their actual presentation to
 * the named row rather than assuming a private child index or source pose. */
static lv_obj_t *inline_switch(orb_shell_t *shell, const char *caption)
{
    lv_obj_t *row = find_control(orb_shell_root(shell), caption);
    lv_area_t bounds;
    if (!row || !orb_round_list_row_bounds(row, &bounds)) return NULL;
    lv_obj_t *list = lv_obj_get_parent(row);
    for (unsigned i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *control = lv_obj_get_child(list, i);
        if (!lv_obj_is_floating(control) || !lv_obj_is_clickable(control) || !lv_obj_is_visible(control)) continue;
        lv_area_t box; lv_obj_get_coords(control, &box);
        lv_point_t centre = {(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
        if (centre.x >= bounds.x1 && centre.x <= bounds.x2 &&
            centre.y >= bounds.y1 && centre.y <= bounds.y2) return control;
    }
    return NULL;
}

static bool reveal(orb_shell_t *shell, const char *caption);

/* The same bounded model mutations as the link/voice services, with synthetic
 * text. No microphone, network service or owner session is touched. */
static bool model_scene(orb_shell_t *shell, const char *name)
{
    if (!strncmp(name, "connection", 10) || !strncmp(name, "forget-wifi-confirm", 19) ||
        !strncmp(name, "unpair-confirm", 14)) {
        orb_shell_navigate(shell, ORB_SHELL_SETTINGS);
        settle(shell);
        if (!reveal(shell, "Mac") || !tap(shell, "Mac")) return false;
        if (strcmp(name, "connection")) {
            if (!tap(shell, "Maintenance")) return false;
            if (!strncmp(name, "forget-wifi-confirm", 19) && !tap(shell, "Forget Wi-Fi")) return false;
            if (!strncmp(name, "unpair-confirm", 14) && !tap(shell, "Unpair Mac")) return false;
        }
        if (strstr(name, "-more")) {
            orb_shell_twist(shell, 8); settle(shell);
        }
        return true;
    }
    if (!strcmp(name, "setup-more") || !strcmp(name, "pairing-more")) {
        orb_shell_twist(shell, 8); settle(shell);
        return true;
    }
    if (!strncmp(name, "update-check-", 13)) {
        orb_shell_navigate(shell, ORB_SHELL_UPDATES);
        settle(shell);
        if (!tap(shell, "Check now")) return false;
        if (!strcmp(name, "update-check-result")) {
            orb_shell_snapshot_t result;
            if (!scenario(&result, "face")) return false;
            result.update_check_revision = 1;
            snprintf(result.update_check_result, sizeof(result.update_check_result), "Orb is up to date.");
            orb_shell_update(shell, &result);
            settle(shell);
        }
        return true;
    }
    if (!strncmp(name, "agents-idle", 11)) {
        if (!orb_model_set_summary(12, (uint32_t[3]){0, 0, 0})) return false;
    } else if (!strncmp(name, "idle-", 5)) {
        if (strcmp(name, "idle-loading")) {
            orb_idle_request_t request;
            if (!orb_model_take_idle_request(&request)) return false;
            orb_session_t idle[2] = {
                {.id = "idle-review", .agent = ORB_AGENT_CLAUDE, .title = "Review tests", .project = "Orb", .state = ORB_SESSION_IDLE, .can_message = true},
                {.id = "idle-notes", .agent = ORB_AGENT_CODEX, .title = "Release notes", .project = "Recipe", .state = ORB_SESSION_DONE},
            };
            if (!orb_model_complete_idle(&request, idle, !strcmp(name, "idle-empty") ? 0 : 2,
                    !strcmp(name, "idle-end") || !strcmp(name, "idle-empty") ? NULL : "next-page",
                    !strcmp(name, "idle-error") ? "timeout" : NULL)) return false;
            if (!strcmp(name, "idle-detail") || !strcmp(name, "idle-reply")) {
                settle(shell);
                if (!tap(shell, "Review tests")) return false;
                if (!strcmp(name, "idle-reply")) {
                    if (!tap(shell, "Reply")) return false;
                    orb_voice_state_t voice = {.phase = ORB_VOICE_CONFIRMING, .stream = 8};
                    strcpy(voice.target, "idle-review"); strcpy(voice.text, "Please continue the review.");
                    if (!orb_model_set_voice(&voice)) return false;
                }
            }
        }
    } else if (!strcmp(name, "session-outside-focus")) {
        if (!orb_model_remove_session("claude-firmware")) return false;
    } else if (!strncmp(name, "sessions-", 9)) {
        orb_model_snapshot_t model;
        if (!orb_model_snapshot(&model)) return false;
        if (!strncmp(name, "sessions-recent-", 16)) {
            model.prefs.idle_minutes = !strcmp(name, "sessions-recent-off") ? 0 :
                                       !strcmp(name, "sessions-recent-five") ? 5 : 30;
            if (!orb_model_set_prefs(&model.prefs)) return false;
        } else if (!strcmp(name, "sessions-off")) {
            model.prefs.sources[ORB_AGENT_CLAUDE] = false;
            model.prefs.idle_minutes = 0;
            if (!orb_model_set_prefs(&model.prefs)) return false;
        } else if (!strcmp(name, "sessions-pending") || !strcmp(name, "sessions-error")) {
            if (!tap(shell, "Claude")) return false;
            if (!strcmp(name, "sessions-error")) orb_model_complete_prefs(NULL, "timeout");
        }
    } else if (!strncmp(name, "voice-", 6)) {
        orb_voice_state_t voice = {.stream = 7, .level = 68};
        snprintf(voice.target, sizeof(voice.target), "codex-shell");
        if (!strcmp(name, "voice-starting")) voice.phase = ORB_VOICE_STARTING;
        else if (!strcmp(name, "voice-recording")) voice.phase = ORB_VOICE_RECORDING;
        else if (!strcmp(name, "voice-transcribing")) voice.phase = ORB_VOICE_TRANSCRIBING;
        else if (!strcmp(name, "voice-confirming")) voice.phase = ORB_VOICE_CONFIRMING;
        else if (!strcmp(name, "voice-sending")) voice.phase = ORB_VOICE_SENDING;
        else if (!strcmp(name, "voice-error") || !strcmp(name, "voice-no-speech") ||
                 !strcmp(name, "voice-network") || !strcmp(name, "voice-unsupported")) voice.phase = ORB_VOICE_ERROR;
        else return false;
        if (voice.phase >= ORB_VOICE_CONFIRMING)
            snprintf(voice.text, sizeof(voice.text),
                "Please review the round shell at the largest text size. Check the Agents list, "
                "then run the simulator tests and show me the results before changing anything else. "
                "Keep the approval buttons visible while I read the complete command.");
        if (voice.phase == ORB_VOICE_ERROR)
            snprintf(voice.error, sizeof(voice.error), "Delivery could not be confirmed");
        if (!strcmp(name, "voice-no-speech") || !strcmp(name, "voice-network") || !strcmp(name, "voice-unsupported")) {
            voice.text[0] = '\0';
            snprintf(voice.error_code, sizeof(voice.error_code), "%s", !strcmp(name, "voice-no-speech") ? "no_speech" :
                     !strcmp(name, "voice-network") ? "disconnected" : "unsupported");
            snprintf(voice.error, sizeof(voice.error), "%s", !strcmp(name, "voice-no-speech") ? "Hold and speak" :
                     !strcmp(name, "voice-network") ? "Connection lost. Try again." : "Voice unavailable on the hub.");
        }
        if (!orb_model_set_voice(&voice)) return false;
    } else if (!strncmp(name, "answer-", 7) || !strcmp(name, "card-cancelled")) {
        if (!strcmp(name, "card-cancelled")) {
            if (!orb_model_cancel_card("choice-next")) return false;
        } else {
            if (!reveal(shell, "Device link") || !tap(shell, "Device link")) return false;
            orb_answer_t answer;
            if (!orb_model_peek_answer(&answer) || strcmp(answer.card, "choice-next") ||
                strcmp(answer.option, "link") || orb_model_pending_answers() != 1) {
                fprintf(stderr, "answer fixture: Device link did not queue the exact selected answer\n");
                return false;
            }
            if (!find_label(orb_shell_root(shell), "Sending…")) return false;
            if (!orb_model_cancel_card(answer.card)) return false;
            if (strcmp(name, "answer-queued")) {
                orb_answer_t recorded = answer;
                if (!strcmp(name, "answer-other"))
                    snprintf(recorded.option, sizeof(recorded.option), "stack");
                bool accepted = strcmp(name, "answer-rejected") != 0;
                bool delivered = strcmp(name, "answer-undelivered") != 0;
                if (!orb_model_complete_answer(answer.card, accepted ? &recorded : NULL,
                        delivered, accepted ? NULL : "expired")) return false;
            }
            settle(shell);
            const char *status = !strcmp(name, "answer-queued") ? "Sending…" :
                !strcmp(name, "answer-rejected") ? "Not sent" :
                !strcmp(name, "answer-undelivered") ? "Sent" : "Delivered";
            unsigned pending = !strcmp(name, "answer-queued") ? 1 : 0;
            if (!find_label(orb_shell_root(shell), status) || orb_model_pending_answers() != pending ||
                find_control(orb_shell_root(shell), "Device link")) {
                fprintf(stderr, "answer fixture: expected %s receipt with %u pending answers; surface=%u status=%d pending=%zu control=%d\n", status, pending, (unsigned)orb_shell_surface(shell), find_label(orb_shell_root(shell), status) != NULL, orb_model_pending_answers(), find_control(orb_shell_root(shell), "Device link") != NULL);
                return false;
            }
        }
    } else if (!strncmp(name, "message-", 8) && strcmp(name, "message-dot")) {
        orb_message_t message = {0};
        snprintf(message.target, sizeof(message.target), "codex-shell");
        snprintf(message.text, sizeof(message.text), "Please review the simulator test results.");
        if (!orb_model_voice_focus(message.target, NULL)) return false;
        bool accepted = !strcmp(name, "message-delivered");
        orb_model_complete_message(&message, accepted, accepted ? NULL : "timeout");
    }
    settle(shell);
    return true;
}

static lv_obj_t *label_in_tree(lv_obj_t *root, const char *caption)
{
    if (lv_obj_check_type(root, &lv_label_class) &&
        (display_label_matches(lv_label_get_text(root), caption) ||
         (!strcmp(caption, "About") && !strncmp(lv_label_get_text(root), "About · ", 9)) ||
         (!strcmp(caption, "Recent activity") && strstr(lv_label_get_text(root), "Show")))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = label_in_tree(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}

static bool reveal(orb_shell_t *shell, const char *caption)
{
    lv_obj_t *text = label_in_tree(orb_shell_root(shell), caption);
    if (!text) { fprintf(stderr, "flow: missing row '%s'\n", caption); return false; }
    lv_obj_t *row = lv_obj_get_parent(text), *list = lv_obj_get_parent(row);
    /* A question's kit rows share a reading viewport with its prose. The
     * immediate row parent is content, so reveal through the actual scroller. */
    while (list && !lv_obj_is_scrollable(list)) list = lv_obj_get_parent(list);
    if (!list) { fprintf(stderr, "flow: row '%s' has no scroll viewport\n", caption); return false; }
    lv_area_t row_box, viewport;
    lv_obj_get_coords(row, &row_box); lv_obj_get_coords(list, &viewport);
    int scroll = lv_obj_get_scroll_y(list) +
        (row_box.y1 + row_box.y2 - viewport.y1 - viewport.y2) / 2;
    lv_obj_scroll_to_y(list, LV_MAX(0, scroll), LV_ANIM_OFF);
    settle(shell);
    return true;
}

static bool verify_back(const char *scene)
{
    /* Design 1.0: BOOT is the always-available Back alternative. Content
     * rows may use a chevron symbol (Undo Clear), so reject only floating
     * navigation controls rather than an inline content glyph. */
    lv_obj_t *control = find_control(lv_screen_active(), ORB_ICON_CHEVRON_LEFT);
    if (control && lv_obj_get_width(control) <= 72) {
        fprintf(stderr, "scene %s: redundant on-screen Back control\n", scene);
        return false;
    }
    return true;
}

static bool settings_review(orb_shell_t *shell, const char *directory)
{
    static const char *names[] = {"Display", "Sounds", "Accessibility", "Mac", "About", "Sessions", "Updates",
                                  "Units", "Controls", "Compass", "Battery"};
    orb_shell_navigate(shell, ORB_SHELL_SETTINGS);
    settle(shell);
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!reveal(shell, names[i])) return false;
        char path[1024];
        int length = snprintf(path, sizeof(path), "%s/Settings-%s.png", directory, i == 4 ? "About" : names[i]);
        if (length < 0 || (size_t)length >= sizeof(path) || !sim_display_save_png(path)) return false;
    }
    return true;
}

static bool flow(orb_shell_t *shell, const char *directory)
{
#define FLOW_REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "flow line %d: %s (surface=%u)\n", __LINE__, #condition, \
                (unsigned)orb_shell_surface(shell)); \
        return false; \
    } \
} while (0)
    orb_model_snapshot_t model;
    FLOW_REQUIRE(orb_model_snapshot(&model) && model.session_count);
    if (!shot(shell, directory, "flow-01-face")) return false;
    FLOW_REQUIRE(tap(shell, "Apps") && orb_shell_surface(shell) == ORB_SHELL_APPS);
    if (!shot(shell, directory, "flow-02-apps")) return false;
    FLOW_REQUIRE(tap(shell, "Agents") && orb_shell_surface(shell) == ORB_SHELL_AGENTS);
    if (!shot(shell, directory, "flow-03-agents")) return false;
    orb_shell_select_session(shell, model.sessions[0].id); settle(shell);
    FLOW_REQUIRE(orb_shell_surface(shell) == ORB_SHELL_SESSION);
    if (!shot(shell, directory, "flow-04-session")) return false;
    orb_shell_select_card(shell, "approval-build"); settle(shell);
    FLOW_REQUIRE(orb_shell_surface(shell) == ORB_SHELL_CARD);
    FLOW_REQUIRE(find_control(orb_shell_root(shell), "Allow") &&
                 find_control(orb_shell_root(shell), "Deny"));
    if (!shot(shell, directory, "flow-05-approval")) return false;
    FLOW_REQUIRE(tap(shell, "Allow") && orb_model_pending_answers());
    FLOW_REQUIRE(find_label(orb_shell_root(shell), "Sending…") &&
                 !find_label(orb_shell_root(shell), "Delivered"));
    if (!shot(shell, directory, "flow-06-queued")) return false;
    orb_answer_t answer;
    FLOW_REQUIRE(orb_model_take_answer(&answer) && answer.option[0] && !answer.text[0]);
    /* Design 2.0 retires the Face attention pill. The Notifications list
     * supplies the pending-decision route; still activate its actual row. */
    orb_model_load_demo();
    orb_model_set_connection(true, true);
    if (!orb_model_snapshot(&model)) return false;
    for (size_t i = 0; i < model.card_count; i++) {
        if (strcmp(model.cards[i].id, "approval-build")) continue;
        if (!orb_model_cancel_card(model.cards[i].id)) return false;
        snprintf(model.cards[i].id, sizeof(model.cards[i].id), "approval-shortcut");
        if (!orb_model_upsert_card(&model.cards[i])) return false;
    }
    if (!orb_model_cancel_card("choice-next")) return false;
    orb_shell_navigate(shell, ORB_SHELL_FACE);
    settle(shell);
    if (!shot(shell, directory, "shortcut-01-face")) return false;
    orb_shell_navigate(shell, ORB_SHELL_NOTIFICATIONS); settle(shell);
    const char *approval_title = NULL;
    for (size_t i = 0; i < model.card_count; i++)
        if (!strcmp(model.cards[i].id, "approval-shortcut")) approval_title = model.cards[i].title;
    /* An approval row opens the full command/decision review directly. The
     * informational notification detail's Open session action is not this route. */
    FLOW_REQUIRE(approval_title && tap(shell, approval_title) &&
                 orb_shell_surface(shell) == ORB_SHELL_CARD);
    FLOW_REQUIRE(find_control(orb_shell_root(shell), "Allow") &&
                 find_control(orb_shell_root(shell), "Deny"));
    if (!shot(shell, directory, "shortcut-02-notification")) return false;
    printf("shell flow: Face → Apps → Agents → session → approval → queued Allow; Notifications approval review\n");
#undef FLOW_REQUIRE
    return true;
}

/* The delight recorder deliberately does not call settle between samples.
 * Each PNG is the framebuffer after real input/model publication at the named
 * virtual time. This preserves the press and intermediate motion evidence. */
static void advance(orb_shell_t *shell, uint32_t elapsed)
{
    while (elapsed) {
        uint32_t step = LV_MIN(elapsed, 16);
        s_now += step;
        orb_shell_apps_service();
        orb_apps_poll((int64_t)s_now * 1000, false, true, 0, 0, 1, true);
        orb_shell_apps_publish();
        orb_shell_poll(shell);
        lv_timer_handler();
        lv_refr_now(s_display);
        elapsed -= step;
    }
}

static bool motion_shot(const char *directory, const char *name, uint32_t ms)
{
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s-%03u.png", directory, name, (unsigned)ms);
    if (length < 0 || (size_t)length >= sizeof(path)) return false;
    lv_refr_now(s_display);
    return sim_display_save_png(path);
}

static bool visible_white_label(orb_shell_t *shell, const char *caption);

static bool motion_frames(orb_shell_t *shell, const char *directory, const char *name)
{
    static const uint32_t times[] = {0, 16, 48, 96, 150, 176, 320, 600, 800};
    uint32_t previous = 0;
    for (unsigned i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        /* A held control is no longer a tap at500ms. Record the immediate
         * response, then lift inside the same budget as a real owner tap. */
        if ((!strcmp(name, "apps-press") || !strcmp(name, "icon-press")) && times[i] > 176) break;
        advance(shell, times[i] - previous);
        if (!motion_shot(directory, name, times[i])) return false;
        if (!strcmp(name, "count-roll") && !visible_white_label(shell, "10:09")) return false;
        if (!strcmp(name, "scroll-release") && !verify_back("agents")) return false;
        previous = times[i];
    }
    return true;
}

static lv_obj_t *arc_ink(lv_obj_t *root)
{
    if (lv_obj_check_type(root, &lv_arclabel_class)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = arc_ink(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}

/* A retained text object can remain in the tree while an animated overlay has
 * erased its framebuffer pixels. Verify the resting frame, not just the model
 * or object hierarchy, after these transitions. */
static bool visible_white_label(orb_shell_t *shell, const char *caption)
{
    lv_obj_t *label = find_label(orb_shell_root(shell), caption);
    if (!label) return false;
    lv_area_t box;
    lv_obj_get_coords(label, &box);
    uint8_t *rgb = malloc(SIM_D * SIM_D * 3);
    if (!rgb) return false;
    sim_display_rgb(rgb);
    unsigned count = 0;
    bool rim = orb_kit_rim_text_get(label) != NULL;
    lv_obj_t *ink = rim ? arc_ink(label) : label;
    if (!ink) { free(rgb); return false; }
    lv_color32_t colour = lv_color_to_32(lv_obj_get_style_text_color(ink, 0), LV_OPA_COVER);
    for (int y = LV_MAX(0, box.y1); y <= LV_MIN(SIM_D - 1, box.y2); y++) {
        for (int x = LV_MAX(0, box.x1); x <= LV_MIN(SIM_D - 1, box.x2); x++) {
            const uint8_t *p = &rgb[(y * SIM_D + x) * 3];
            if (rim) {
                unsigned peak = LV_MAX(p[0], LV_MAX(p[1], p[2]));
                /* The rim wrapper contains only its prepared title slices.
                 * Require the actual title colour, excluding unrelated white
                 * content that could otherwise satisfy a bounding-box test. */
                count += peak > 96 && abs((int)p[0] - colour.red) < 32 &&
                    abs((int)p[1] - colour.green) < 32 && abs((int)p[2] - colour.blue) < 32;
            } else count += p[0] > 210 && p[1] > 210 && p[2] > 210;
        }
    }
    free(rgb);
    if (count < 40) fprintf(stderr, "delight: '%s' exists but lost its rendered pixels (%u matching ink)\n", caption, count);
    return count >= 40;
}

static bool press_control(orb_shell_t *shell, const char *caption)
{
    lv_obj_t *control = find_control(orb_shell_root(shell), caption);
    if (!control) {
        fprintf(stderr, "delight: missing control '%s'\n", caption);
        return false;
    }
    lv_area_t box;
    lv_obj_get_coords(control, &box);
    s_input.point = (lv_point_t){(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer);
    advance(shell, 16);
    return true;
}

static void release_pointer(orb_shell_t *shell)
{
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    advance(shell, 16);
}

static void reset_demo(orb_shell_t *shell, orb_shell_surface_t surface)
{
    orb_model_load_demo();
    orb_model_set_connection(true, true);
    orb_shell_navigate(shell, surface);
    settle(shell);
}

typedef enum {
    NAV_SLIDE_LEFT, NAV_SLIDE_RIGHT, NAV_STACK_UP, NAV_SHADE_DOWN, NAV_BACK,
    NAV_CONTROL, NAV_IRIS, NAV_IRIS_BACK, NAV_ROTARY, NAV_ROTARY_REVERSE,
    NAV_CANCEL, NAV_REGRAB, NAV_COMPASS, NAV_COMPASS_CANCEL,
} nav_capture_kind_t;

typedef struct {
    const char *name;
    nav_capture_kind_t kind;
    orb_shell_surface_t source, destination;
    int x1, y1, x2, y2;
} nav_capture_t;

static void nav_pointer(int x, int y, bool down)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
}

static bool home_rotary_check(bool ok, const char *why)
{
    if (!ok) fprintf(stderr, "Home shell rotary: %s\n", why);
    return ok;
}

static void home_rim_preview(orb_shell_t *shell)
{
    nav_pointer(449, 233, true);
    advance(shell, 32);
    for (unsigned i = 1; i <= 6; ++i) {
        float theta = (float)i * .13f;
        nav_pointer(233 + (int)lroundf(216 * cosf(theta)),
                    233 + (int)lroundf(216 * sinf(theta)), true);
        advance(shell, 33);
    }
}

static bool home_rotary_review(orb_shell_t *shell)
{
    /* Republish only synthetic data without demo's immediate success path,
     * so shell input must produce exactly one real-shaped queued request. */
    orb_home_snapshot_t *model = calloc(1, sizeof(*model));
    bool ok = model && orb_home_snapshot(model) && orb_home_snapshot_chunk(
        model->rooms, model->room_count, model->accessories, model->accessory_count,
        model->scenes, model->scene_count, true, false);
    free(model);
    if (!home_rotary_check(ok, "install asynchronous Home fixture")) return false;
    settle(shell);
    if (!tap(shell, "Living Room") || !tap(shell, "Ceiling Lamp")) return false;
    orb_home_request_t request, extra;
    if (!home_rotary_check(orb_shell_twist(shell, 2) && orb_home_take_request(&request) &&
        !strcmp(request.id, "sofa-lamp") && request.values.present == ORB_HOME_BRIGHTNESS &&
        request.values.brightness == 72 && !orb_home_take_request(&extra),
        "twist adjusts the selected dial and commits once")) return false;
    orb_home_complete(&request, &request.values, NULL);
    settle(shell);

    home_rim_preview(shell);
    if (!home_rotary_check(!orb_home_take_request(&request), "rim previews without writing before lift")) return false;
    nav_pointer(s_input.point.x, s_input.point.y, false);
    advance(shell, 32);
    if (!home_rotary_check(orb_home_take_request(&request) &&
        request.values.present == ORB_HOME_BRIGHTNESS && request.values.brightness > 72 &&
        !orb_home_take_request(&extra), "rim lift commits the preview exactly once")) return false;
    orb_home_complete(&request, &request.values, NULL);
    settle(shell);
    if (!home_rotary_check(!orb_home_take_request(&extra), "rim release has no continuing fling writes")) return false;

    home_rim_preview(shell);
    orb_shell_display_active(shell, false);
    nav_pointer(s_input.point.x, s_input.point.y, false);
    advance(shell, 32);
    orb_shell_display_active(shell, true);
    settle(shell);
    if (!home_rotary_check(!orb_home_take_request(&request), "display suspension cancels the rim preview")) return false;

    orb_input_mark_synthetic(s_pointer);
    home_rim_preview(shell);
    nav_pointer(s_input.point.x, s_input.point.y, false);
    advance(shell, 32);
    lv_indev_set_user_data(s_pointer, NULL);
    settle(shell);
    if (!home_rotary_check(!orb_home_take_request(&request), "synthetic rim input cannot queue a Home write")) return false;

    orb_shell_button(shell, &(orb_button_event_t){.button = ORB_BUTTON_BOOT,
        .gesture = ORB_BUTTON_SHORT, .synthetic = true});
    settle(shell);
    if (!tap(shell, "Front Door")) return false;
    if (!home_rotary_check(!orb_shell_twist(shell, 2) && !orb_home_take_request(&request),
        "lock detail never takes a dial owner or queues a twist write")) return false;
    printf("Home shell rotary: twist, rim commit, cancellation, synthetic guard and lock safety PASS\n");
    return true;
}

/* Navigation evidence uses actual pointer ownership. No bench seam or direct
 * navigation call supplies the moving frames: the destination must follow
 * the same input as a finger. HIG §6: retain reversals and cancellation. */
static bool nav_capture(orb_shell_t *shell, const char *directory, const nav_capture_t *scene)
{
    nav_pointer(233, 233, false);
    reset_demo(shell, ORB_SHELL_FACE);
    if (scene->source != ORB_SHELL_FACE) {
        orb_shell_navigate(shell, scene->source);
        settle(shell);
    }
    if (scene->kind == NAV_IRIS_BACK) {
        /* Face neighbours have a canonical Face parent. Dot retains ordinary
         * Apps launch provenance, so this still records an actual reverse iris. */
        const char *caption = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? "Dot" : ORB_ICON_CHAT;
        if (!tap(shell, caption)) return false;
        settle(shell);
    }
    int x1 = scene->x1, y1 = scene->y1, x2 = scene->x2, y2 = scene->y2;
    if (scene->kind == NAV_IRIS) {
        const char *caption = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? "Agents" : ORB_ICON_PROMPT;
        lv_obj_t *control = find_control(orb_shell_root(shell), caption);
        if (!control) { fprintf(stderr, "nav: missing Agents launch control\n"); return false; }
        lv_area_t bounds;
        lv_obj_get_coords(control, &bounds);
        x1 = x2 = (bounds.x1 + bounds.x2) / 2;
        y1 = y2 = (bounds.y1 + bounds.y2) / 2;
    }
    for (unsigned frame = 0; frame < 30; frame++) {
        if (frame) advance(shell, 33);
        int x = x1, y = y1;
        bool down = frame < 11;
        if (scene->kind == NAV_IRIS) down = frame < 3;
        else if (scene->kind == NAV_COMPASS || scene->kind == NAV_COMPASS_CANCEL) {
            down = frame < 21;
            if (frame >= 14) {
                unsigned move = LV_MIN(frame - 14, 6u);
                x = x1 + (x2 - x1) * (int)move / 6;
                y = y1 + (y2 - y1) * (int)move / 6;
            }
        } else if (scene->kind == NAV_ROTARY || scene->kind == NAV_ROTARY_REVERSE) {
            float theta = LV_MIN(frame, 10u) * .13f;
            if (scene->kind == NAV_ROTARY_REVERSE) theta = -theta;
            x = 233 + (int)lrintf(216 * cosf(theta));
            y = 233 + (int)lrintf(216 * sinf(theta));
        } else if (scene->kind == NAV_CANCEL) {
            unsigned distance = frame < 6 ? frame : frame < 11 ? 10 - frame : 0;
            x = x1 + (x2 - x1) * (int)distance / 5;
            y = y1 + (y2 - y1) * (int)distance / 5;
        } else if (scene->kind == NAV_REGRAB) {
            if (frame < 8) x = x1 - (int)frame * 25;
            else if (frame < 10) { x = x1 - 175; down = false; }
            else if (frame < 18) { x = x1 - 175 + (int)(frame - 10) * 28; down = true; }
            else { x = x1 + 21; down = false; }
        } else {
            unsigned move = LV_MIN(frame, 10u);
            x = x1 + (x2 - x1) * (int)move / 10;
            y = y1 + (y2 - y1) * (int)move / 10;
        }
        nav_pointer(x, y, down);
        /* Process the recogniser without adding an unrecorded time step. */
        orb_shell_poll(shell);
        lv_timer_handler();
        if (!motion_shot(directory, scene->name, frame * 33)) return false;
    }
    nav_pointer(s_input.point.x, s_input.point.y, false);
    advance(shell, 600);
    bool ok = orb_shell_surface(shell) == scene->destination;
    orb_answer_t answer;
    if (orb_model_peek_answer(&answer)) {
        fprintf(stderr, "nav: %s unexpectedly submitted an answer\n", scene->name);
        ok = false;
    }
    if (!ok) fprintf(stderr, "nav: %s ended on surface %d, expected %d\n", scene->name,
                     orb_shell_surface(shell), scene->destination);
    printf("nav: %s frames=30 interval_ms=33 destination=%d %s\n", scene->name,
           orb_shell_surface(shell), ok ? "PASS" : "FAIL");
    return ok;
}

static bool navigation(orb_shell_t *shell, const char *directory)
{
    static const nav_capture_t scenes[] = {
        {"slide-left", NAV_SLIDE_LEFT, ORB_SHELL_FACE, ORB_SHELL_HOME, 233, 240, 32, 240},
        {"slide-right", NAV_SLIDE_RIGHT, ORB_SHELL_FACE, ORB_SHELL_AGENTS, 233, 240, 434, 240},
        {"place-end", NAV_SLIDE_LEFT, ORB_SHELL_AGENTS, ORB_SHELL_AGENTS, 233, 240, 32, 240},
        {"stack-up", NAV_STACK_UP, ORB_SHELL_FACE, ORB_SHELL_STACK, 233, 240, 233, 30},
        {"shade-down", NAV_SHADE_DOWN, ORB_SHELL_FACE, ORB_SHELL_NOTIFICATIONS, 233, 220, 233, 430},
        {"back", NAV_BACK, ORB_SHELL_AGENTS, ORB_SHELL_FACE, 17, 233, 306, 233},
        {"control-center", NAV_CONTROL, ORB_SHELL_FACE, ORB_SHELL_CONTROL_CENTER, 449, 233, 160, 233},
        {"iris", NAV_IRIS, ORB_SHELL_APPS, ORB_SHELL_AGENTS, 0, 0, 0, 0},
        {"iris-back", NAV_IRIS_BACK, ORB_SHELL_APPS, ORB_SHELL_APPS, 17, 233, 306, 233},
        {"rotary", NAV_ROTARY, ORB_SHELL_SETTINGS, ORB_SHELL_SETTINGS, 449, 233, 449, 233},
        {"rotary-reverse", NAV_ROTARY_REVERSE, ORB_SHELL_SETTINGS, ORB_SHELL_SETTINGS, 449, 233, 449, 233},
        {"cancel", NAV_CANCEL, ORB_SHELL_FACE, ORB_SHELL_FACE, 233, 240, 168, 240},
        /* Once Home owns the live content, a centre regrab stays content and
         * the released Face transition completes; edge regrabs remain nav. */
        {"regrab-reversal", NAV_REGRAB, ORB_SHELL_FACE, ORB_SHELL_HOME, 233, 240, 233, 240},
        /* Legacy capture names remain stable; holding Face now opens its
         * gallery. Moving/releasing that held contact never launches an app. */
        {"compass", NAV_COMPASS, ORB_SHELL_FACE, ORB_SHELL_FACE, 233, 240, 233, 115},
        {"compass-agents", NAV_COMPASS, ORB_SHELL_FACE, ORB_SHELL_FACE, 233, 240, 384, 220},
        {"compass-home", NAV_COMPASS, ORB_SHELL_FACE, ORB_SHELL_FACE, 233, 240, 82, 220},
        {"compass-play", NAV_COMPASS, ORB_SHELL_FACE, ORB_SHELL_FACE, 233, 240, 233, 364},
        {"compass-cancel", NAV_COMPASS_CANCEL, ORB_SHELL_FACE, ORB_SHELL_FACE, 233, 240, 233, 233},
    };
    orb_home_load_demo();
    bool ok = true;
    for (unsigned i = 0; i < sizeof(scenes) / sizeof(scenes[0]); i++)
        ok = nav_capture(shell, directory, &scenes[i]) && ok;
    return ok;
}

static bool reward_evidence(orb_shell_t *shell, const char *directory, bool allow)
{
    reset_demo(shell, ORB_SHELL_FACE);
    orb_model_snapshot_t model;
    if (!orb_model_snapshot(&model)) return false;
    orb_card_t approval = model.cards[0];
    const char *id = allow ? "approval-evidence-allow" : "approval-evidence-deny";
    if (!orb_model_cancel_card(approval.id)) return false;
    snprintf(approval.id, sizeof(approval.id), "%s", id);
    if (!orb_model_upsert_card(&approval)) return false;
    orb_shell_select_card(shell, id);
    settle(shell);
    const char *name = allow ? "approve" : "deny";
    if (!motion_shot(directory, name, 0) ||
        !press_control(shell, allow ? "Allow" : "Deny")) return false;
    if (!motion_shot(directory, allow ? "approve-press" : "deny-press", 16)) return false;
    release_pointer(shell);
    if (!motion_frames(shell, directory, allow ? "approve-queued" : "deny-queued")) return false;
    orb_answer_t answer;
    if (!orb_model_peek_answer(&answer) || strcmp(answer.card, id) ||
        strcmp(answer.option, allow ? "allow" : "deny")) {
        fprintf(stderr, "delight: decision input did not queue the exact selected option\n");
        return false;
    }
    /* This is a synthetic authoritative hub result, not a local success claim. */
    if (!orb_model_cancel_card(answer.card) ||
        !orb_model_complete_answer(answer.card, &answer, true, NULL)) return false;
    advance(shell, 16);
    if (!motion_frames(shell, directory, allow ? "approve-delivered" : "deny-delivered")) return false;
    return orb_model_pending_answers() == 0;
}

static bool ux_outcome_failure(orb_shell_t *shell, const char *directory, bool rejected)
{
    reset_demo(shell, ORB_SHELL_FACE);
    orb_model_snapshot_t model;
    if (!orb_model_snapshot(&model)) return false;
    orb_card_t approval = model.cards[0];
    snprintf(approval.id, sizeof(approval.id), "%s", rejected ? "approval-reject" : "approval-undelivered");
    if (!orb_model_upsert_card(&approval)) return false;
    orb_shell_select_card(shell, approval.id); settle(shell);
    if (!press_control(shell, "Allow")) return false;
    release_pointer(shell);
    orb_answer_t answer;
    if (!orb_model_peek_answer(&answer) || strcmp(answer.card, approval.id)) return false;
    if (!orb_model_cancel_card(answer.card) ||
        !orb_model_complete_answer(answer.card, rejected ? NULL : &answer, false, rejected ? "expired" : NULL)) return false;
    advance(shell, 16);
    return motion_frames(shell, directory, rejected ? "approve-rejected" : "approve-undelivered");
}

static bool launcher_caption_evidence(orb_shell_t *shell, const char *directory)
{
    if (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3) return true;
    reset_demo(shell, ORB_SHELL_APPS);
    const char *icons[] = {ORB_ICON_PROMPT, ORB_ICON_CHAT, ORB_ICON_TIMER, ORB_ICON_GEAR,
                          ORB_ICON_UPLOAD, ORB_ICON_GAME_CONTROLLER, ORB_ICON_HOUSE};
    const char *names[] = {"Agents", "Dot", "Time", "Settings", "Updates", "Play", "Home"};
    const orb_shell_surface_t destinations[] = {ORB_SHELL_AGENTS, ORB_SHELL_DOT, ORB_SHELL_TIME,
        ORB_SHELL_SETTINGS, ORB_SHELL_UPDATES, ORB_SHELL_PLAY, ORB_SHELL_HOME};
    /* Returning to Apps preserves its last settled focus. The preceding
     * reverse-iris evidence can select Dot, so cancellation must restore the
     * actual starting caption rather than assume a fresh launcher instance. */
    unsigned previous = 0;
    while (previous < 7 && !find_label(orb_shell_root(shell), names[previous])) previous++;
    if (previous == 7) {
        fprintf(stderr, "launcher: missing initial focused app caption\n");
        return false;
    }
    for (unsigned i = 0; i < 7; i++) {
        if (!press_control(shell, icons[i]) || !find_label(orb_shell_root(shell), names[i])) return false;
        char scene[64]; snprintf(scene, sizeof(scene), "launcher-%s-touch", names[i]);
        if (!motion_shot(directory, scene, 0)) return false;
        /* Move beyond the tap budget: a launcher cancellation must retain
         * its previous focus instead of launching the touched app. */
        s_input.point.x = s_input.point.x < 233 ? 445 : 21;
        lv_indev_read(s_pointer); advance(shell, 16);
        release_pointer(shell); settle(shell);
        if (orb_shell_surface(shell) != ORB_SHELL_APPS || !find_label(orb_shell_root(shell), names[previous])) {
            fprintf(stderr, "launcher: %s cancellation surface=%d expected Apps with %s caption\n",
                    names[i], orb_shell_surface(shell), names[previous]);
            motion_shot(directory, "launcher-cancel-failure", 0);
            return false;
        }
        snprintf(scene, sizeof(scene), "launcher-%s-cancel", names[i]);
        if (!motion_shot(directory, scene, 0)) return false;
    }
    if (!tap(shell, names[previous]) || orb_shell_surface(shell) != destinations[previous]) return false;
    return true;
}

static bool delight(orb_shell_t *shell, const char *directory)
{
    if (!motion_shot(directory, "face-rest", 0) || !press_control(shell, "Apps")) return false;
    if (!motion_frames(shell, directory, "apps-press")) return false;
    release_pointer(shell);
    if (!motion_frames(shell, directory, "apps-open") ||
        orb_shell_surface(shell) != ORB_SHELL_APPS) return false;
    /* Dot preserves Apps provenance, keeping this launch/reverse recording
     * sensitive to the iris even though Agents now returns directly to Face.
     * Default uses its icon; AX3 exposes the same app in a round list. */
    if (!press_control(shell, orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? "Dot" : ORB_ICON_CHAT)) return false;
    if (!motion_frames(shell, directory, "icon-press")) return false;
    release_pointer(shell);
    if (!motion_frames(shell, directory, "app-launch") ||
        orb_shell_surface(shell) != ORB_SHELL_DOT || !verify_back("dot")) return false;
    orb_shell_button(shell, &(orb_button_event_t){.button = ORB_BUTTON_BOOT,
                     .gesture = ORB_BUTTON_SHORT, .synthetic = true});
    if (!motion_frames(shell, directory, "app-reverse") ||
        orb_shell_surface(shell) != ORB_SHELL_APPS || !verify_back("apps")) return false;

    orb_shell_navigate(shell, ORB_SHELL_SESSIONS);
    settle(shell);
    /* Centre drags remain list content even at the top. Begin below it to
     * record visible downward travel before this overscroll settles. */
    if (!orb_shell_twist(shell, 2)) return false;
    settle(shell);
    s_input.point = (lv_point_t){233, 198};
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer);
    for (unsigned i = 0; i < 8; i++) {
        s_input.point.y = 198 + (int)i * 21;
        lv_indev_read(s_pointer);
        advance(shell, 16);
        if (!motion_shot(directory, "scroll-drag", i * 16)) return false;
    }
    release_pointer(shell);
    if (!motion_frames(shell, directory, "scroll-release")) return false;

    reset_demo(shell, ORB_SHELL_SESSIONS);
    s_input.point = (lv_point_t){233, 330}; s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer);
    for (unsigned i = 0; i < 8; i++) {
        s_input.point.y = 330 - (int)i * 20;
        lv_indev_read(s_pointer); advance(shell, 16);
    }
    release_pointer(shell);
    if (!motion_frames(shell, directory, "scroll-up-release")) return false;

    reset_demo(shell, ORB_SHELL_STACK);
    if (!motion_shot(directory, "stack-rest", 0) || !orb_shell_twist(shell, 1)) return false;
    if (!motion_frames(shell, directory, "stack-page")) return false;

    reset_demo(shell, ORB_SHELL_FACE);
    orb_model_snapshot_t model;
    if (!orb_model_snapshot(&model)) return false;
    orb_session_t waiting = model.sessions[1];
    waiting.state = ORB_SESSION_WAITING;
    snprintf(waiting.reason, sizeof(waiting.reason), "Question ready");
    if (!orb_model_upsert_session(&waiting)) return false;
    advance(shell, 16);
    if (!motion_frames(shell, directory, "count-roll") || !visible_white_label(shell, "10:09")) return false;
    orb_shell_environment(shell, .25f, 0, 1, false);
    if (!motion_frames(shell, directory, "face-tilt")) return false;
    orb_shell_environment(shell, 0, 0, 1, true);
    if (!motion_frames(shell, directory, "face-always-on")) return false;
    orb_shell_environment(shell, 0, 0, 1, false);

    reset_demo(shell, ORB_SHELL_AGENTS);
    if (!orb_model_snapshot(&model)) return false;
    orb_session_t done = model.sessions[1];
    done.state = ORB_SESSION_DONE;
    snprintf(done.reason, sizeof(done.reason), "Review finished");
    if (!orb_model_upsert_session(&done)) return false;
    advance(shell, 16);
    if (!motion_frames(shell, directory, "agent-done") || !visible_white_label(shell, "Firmware build")) return false;

    reset_demo(shell, ORB_SHELL_STACK);
    if (!orb_model_snapshot(&model)) return false;
    orb_card_t arrival = model.cards[0];
    for (size_t i = 0; i < model.card_count; i++)
        if (!orb_model_cancel_card(model.cards[i].id)) return false;
    settle(shell);
    snprintf(arrival.id, sizeof(arrival.id), "approval-arrival");
    snprintf(arrival.title, sizeof(arrival.title), "Allow test run?");
    if (!orb_model_upsert_card(&arrival)) return false;
    advance(shell, 16);
    if (!motion_frames(shell, directory, "approval-arrival")) return false;
    if (!reward_evidence(shell, directory, true) || !reward_evidence(shell, directory, false)) return false;

    reset_demo(shell, ORB_SHELL_VOICE);
    orb_voice_state_t voice = {.phase = ORB_VOICE_RECORDING, .stream = 71};
    snprintf(voice.target, sizeof(voice.target), "codex-shell");
    if (!orb_model_voice_focus(voice.target, NULL) || !orb_model_set_voice(&voice)) return false;
    advance(shell, 16);
    if (!motion_frames(shell, directory, "voice-level-zero")) return false;
    voice.level = 100;
    if (!orb_model_set_voice(&voice)) return false;
    advance(shell, 16);
    if (!motion_frames(shell, directory, "voice-level-full")) return false;
    voice.phase = ORB_VOICE_CONFIRMING;
    snprintf(voice.text, sizeof(voice.text), "Please run the simulator tests.");
    if (!orb_model_set_voice(&voice)) return false;
    settle(shell);
    if (!motion_shot(directory, "voice-confirm", 0) || !press_control(shell, "Send")) return false;
    release_pointer(shell);
    orb_voice_request_t request;
    if (!orb_model_take_voice_request(&request) || request.action != ORB_VOICE_CONFIRM ||
        strcmp(request.target, voice.target)) return false;
    voice.phase = ORB_VOICE_SENDING;
    if (!orb_model_set_voice(&voice)) return false;
    advance(shell, 16);
    if (!motion_frames(shell, directory, "voice-send")) return false;
    /* Worker publishes IDLE only after the synthetic hub acknowledges Send. */
    voice.phase = ORB_VOICE_IDLE;
    if (!orb_model_set_voice(&voice) || !orb_model_snapshot(&model) ||
        !model.last_message.accepted || strcmp(model.last_message.message.text, voice.text)) return false;
    advance(shell, 16);
    if (!motion_frames(shell, directory, "voice-delivered")) return false;

    reset_demo(shell, ORB_SHELL_FACE);
    if (!orb_model_snapshot(&model)) return false;
    for (size_t i = 0; i < model.card_count; i++)
        if (strcmp(model.cards[i].id, "approval-build") && !orb_model_cancel_card(model.cards[i].id)) return false;
    orb_card_t final_card = model.cards[0];
    if (!orb_model_cancel_card(final_card.id)) return false;
    snprintf(final_card.id, sizeof(final_card.id), "approval-evidence-final");
    if (!orb_model_upsert_card(&final_card)) return false;
    settle(shell);
    orb_shell_select_card(shell, final_card.id);
    settle(shell);
    if (!press_control(shell, "Allow")) return false;
    release_pointer(shell);
    orb_answer_t answer;
    if (!orb_model_peek_answer(&answer) ||
        !orb_model_cancel_card(answer.card) ||
        !orb_model_complete_answer(answer.card, &answer, true, NULL)) return false;
    model.sessions[0].state = ORB_SESSION_BUSY;
    snprintf(model.sessions[0].reason, sizeof(model.sessions[0].reason), "Build started");
    if (!orb_model_upsert_session(&model.sessions[0])) return false;
    orb_shell_navigate(shell, ORB_SHELL_FACE);
    advance(shell, 16);
    if (!motion_frames(shell, directory, "all-clear") || !visible_white_label(shell, "10:09")) return false;
    advance(shell, 1400);
    if (!motion_shot(directory, "all-clear-expired", 2200)) return false;

    orb_shell_navigate(shell, ORB_SHELL_PLAY);
    settle(shell);
    if (!motion_shot(directory, "play-empty", 0)) return false;
    orb_apps_init(ORB_APP_NEEDS_IMU);
    orb_shell_apps_publish();
    orb_shell_navigate(shell, ORB_SHELL_FACE);
    settle(shell);
    orb_shell_navigate(shell, ORB_SHELL_PLAY);
    settle(shell);
    if (!motion_shot(directory, "play-registered", 0) || !press_control(shell, "Play")) return false;
    release_pointer(shell);
    if (!motion_frames(shell, directory, "play-launch") || !orb_apps_active() ||
        strcmp(orb_apps_active()->id, "marble")) return false;
    if (!press_control(shell, "Close")) return false;
    release_pointer(shell);
    if (!motion_frames(shell, directory, "play-close") || orb_apps_active()) return false;
    if (!ux_outcome_failure(shell, directory, true) || !ux_outcome_failure(shell, directory, false) ||
        !launcher_caption_evidence(shell, directory)) return false;
    printf("delight evidence: real presses, launch/reverse, overscroll, paging, state changes, decisions, voice and all-clear\n");
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: orb_shell_sim SCENE OUTPUT.png [default|larger|ax3] [bold] [rt|opaque] [rm] [ic]\n"
                        "       orb_shell_sim --flow EXISTING_DIRECTORY [flags]\n"
                        "       orb_shell_sim --delight EXISTING_DIRECTORY [flags]\n"
                        "       orb_shell_sim --nav EXISTING_DIRECTORY [flags]\n");
        return 2;
    }
    bool scripted = !strcmp(argv[1], "--flow") || !strcmp(argv[1], "--delight") || !strcmp(argv[1], "--nav") || !strcmp(argv[1], "--settings");
    orb_shell_snapshot_t snapshot;
    if (!scenario(&snapshot, scripted ? "face" : argv[1])) { fprintf(stderr, "unknown scene\n"); return 2; }
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    if (!s_display || !orb_model_init()) return 1;
    orb_ui_init(NULL);
    /* Match firmware boot: cold navigation snapshots must include settled
     * glass, whose deferred mask preparation normally runs before the shell. */
    orb_glass_cache_prewarm();
    if (!strcmp(argv[1], "play-registered") || !strncmp(argv[1], "apps", 4))
        orb_apps_init(ORB_APP_NEEDS_IMU);
    orb_model_load_demo();
    if (!strcmp(argv[1], "usage")) {
        orb_usage_snapshot_t usage = {.count = 4};
        for (unsigned i = 0; i < usage.count; i++) {
            usage.windows[i] = (orb_usage_window_t){.provider = i / 2,
                .used_percent = i == 0 ? 9 : i == 1 ? 100 : i == 2 ? 61 : 82,
                .resets_at = 1793219400 + (i % 2 ? 86400 : 0), .source = ORB_USAGE_STATUSLINE};
            snprintf(usage.windows[i].label, sizeof(usage.windows[i].label), "%s", i % 2 ? "Week" : "5 hours");
        }
        if (!orb_model_set_usage(&usage)) return 1;
    }
    if (!strncmp(argv[1], "stack-", 6) || !strcmp(argv[1], "agents-empty") || !strcmp(argv[1], "sessions-empty") || !strcmp(argv[1], "dot-empty") ||
        !strcmp(argv[1], "stack-empty") || !strcmp(argv[1], "notifications-empty")) orb_model_reset();
    if (!strncmp(argv[1], "stack-", 6)) {
        orb_time_init();
        if (!strcmp(argv[1], "stack-day")) orb_home_load_demo();
        if (!strcmp(argv[1], "stack-timer") &&
            (!orb_time_timer_set(210) || !orb_time_timer_start(0))) return 1;
        if (!strcmp(argv[1], "stack-stopwatch") && !orb_time_stopwatch_start(0)) return 1;
        if (!strcmp(argv[1], "stack-usage")) {
            orb_usage_snapshot_t usage = {.count = 1};
            usage.windows[0] = (orb_usage_window_t){.provider = ORB_AGENT_CLAUDE, .used_percent = 82,
                .source = ORB_USAGE_STATUSLINE};
            snprintf(usage.windows[0].label, sizeof(usage.windows[0].label), "Session window");
            if (!orb_model_set_usage(&usage)) return 1;
        }
    }
    if (!strncmp(argv[1], "home-", 5) || !strncmp(argv[1], "face-owner", 10)) orb_home_load_demo();
    if (!strncmp(argv[1], "face-owner", 10)) {
        /* Synthetic owner-review inventory: exactly ten lights are on. */
        orb_home_room_t room = {.id = "Review", .name = "Review"};
        orb_home_accessory_t lamps[10] = {0};
        for (unsigned i = 0; i < 10; i++) {
            snprintf(lamps[i].id, sizeof(lamps[i].id), "review-lamp-%u", i);
            snprintf(lamps[i].name, sizeof(lamps[i].name), "Review Lamp %u", i);
            strcpy(lamps[i].room, "Review"); lamps[i].kind = ORB_HOME_LIGHT;
            lamps[i].reachable = true; lamps[i].state.present = ORB_HOME_ON; lamps[i].state.on = true;
        }
        if (!orb_home_snapshot_chunk_available(&room, 1, lamps, 10, NULL, 0, true, true, false)) return 1;
        orb_home_set_connection(true, true);
        orb_model_snapshot_t model;
        if (!orb_model_snapshot(&model)) return 1;
        unsigned waiting = 0;
        for (unsigned i = 0; i < model.session_count; i++) {
            model.sessions[i].state = !strcmp(argv[1], "face-owner-waiting") && waiting++ < 2 ?
                ORB_SESSION_WAITING : ORB_SESSION_BUSY;
            if (!orb_model_upsert_session(&model.sessions[i])) return 1;
        }
    }
    bool online = snapshot.net == ORB_SHELL_NET_CONNECTED && snapshot.link == ORB_SHELL_LINK_ONLINE;
    orb_model_set_connection(online, online);
    if (!strncmp(argv[1], "answer-", 7)) {
        orb_model_snapshot_t model;
        if (!orb_model_snapshot(&model)) return 1;
        for (size_t i = 0; i < model.card_count; i++) {
            if (strcmp(model.cards[i].id, "choice-next")) continue;
            snprintf(model.cards[i].options[0].id, sizeof(model.cards[i].options[0].id), "link");
            snprintf(model.cards[i].options[0].label, sizeof(model.cards[i].options[0].label), "Device link");
            if (!orb_model_upsert_card(&model.cards[i])) return 1;
        }
    }
    bool increased_contrast = false;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "fahrenheit")) {
            orb_prefs_t prefs;
            orb_model_prefs_defaults(&prefs);
            prefs.temperature = ORB_TEMPERATURE_F;
            if (!orb_model_set_prefs(&prefs)) return 1;
        } else if (!strcmp(argv[i], "default")) {
            if (orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) != ORB_FONTS_OK) return 1;
        } else if (!strcmp(argv[i], "larger")) {
            if (orb_fonts_set_content_size(ORB_CONTENT_SIZE_LARGER) != ORB_FONTS_OK) return 1;
        } else if (!strcmp(argv[i], "ax3")) {
            if (orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) != ORB_FONTS_OK) return 1;
        } else if (!strcmp(argv[i], "bold")) {
            if (orb_fonts_set_bold_text(true) != ORB_FONTS_OK) return 1;
        } else if (!strcmp(argv[i], "rt") || !strcmp(argv[i], "opaque")) orb_ui_set_reduce_transparency(true);
        else if (!strcmp(argv[i], "rm")) orb_ui_set_reduce_motion(true);
        else if (!strcmp(argv[i], "ic")) increased_contrast = true;
        else { fprintf(stderr, "unknown flag: %s\n", argv[i]); return 2; }
    }
    s_pointer = lv_indev_create();
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    orb_shell_t *shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    if (!shell) return 1;
    if (increased_contrast) {
        /* Set the preference through the actual Settings control. A connected
         * synthetic snapshot lets mandatory setup/update previews retain their
         * service precedence when the requested snapshot is installed below. */
        orb_shell_snapshot_t connected;
        if (!scenario(&connected, "face")) return 1;
        orb_shell_update(shell, &connected);
        settle(shell);
        orb_shell_navigate(shell, ORB_SHELL_ACCESSIBILITY);
        settle(shell);
        if (!reveal(shell, "Increase contrast") || !tap(shell, "Increase contrast")) return 1;
        lv_obj_t *contrast = inline_switch(shell, "Increase contrast");
        if (!contrast || !orb_kit_switch_get_checked(contrast)) return 1;
        orb_shell_navigate(shell, ORB_SHELL_FACE);
        settle(shell);
    }
    orb_shell_update(shell, &snapshot);
    settle(shell);
    bool ok;
    if (!strcmp(argv[1], "--settings")) ok = settings_review(shell, argv[2]);
    else if (!strcmp(argv[1], "--nav")) ok = navigation(shell, argv[2]);
    else if (!strcmp(argv[1], "--delight")) ok = delight(shell, argv[2]);
    else if (scripted) ok = flow(shell, argv[2]);
    else {
        select_scene(shell, argv[1]);
        settle(shell);
        if (strcmp(argv[1], "voice-idle-session") && !model_scene(shell, argv[1])) return 1;
        if (!strcmp(argv[1], "home-dial-rotary") && !home_rotary_review(shell)) return 1;
        if (!strncmp(argv[1], "notifications-", 14) && strcmp(argv[1], "notifications-empty")) {
            if (!strcmp(argv[1], "notifications-received")) {
                orb_card_t event = {.id="fresh-event", .session="codex-shell", .kind=ORB_CARD_ATTENTION,
                    .title="Tests finished", .body="All simulator checks passed."};
                if (!orb_model_upsert_card(&event)) return 1;
                settle(shell);
            } else {
                if (!reveal(shell, "Clear all") || !tap(shell, "Clear all")) return 1;
                if (!strcmp(argv[1], "notifications-undo") && !tap(shell, "Undo clear")) return 1;
            }
        }
        const char *reveal_caption = !strcmp(argv[1], "agents-idle-row") ? "Loading idle sessions" :
            !strncmp(argv[1], "idle-more", 9) ? "More" : !strcmp(argv[1], "sessions-codex") ? "Codex" :
            !strcmp(argv[1], "sessions-dot") ? "Dot" : !strncmp(argv[1], "sessions-recent", 15) ? "Recent activity" : NULL;
        if (reveal_caption && !reveal(shell, reveal_caption)) return 1;
        if (!strcmp(argv[1], "idle-more-pending") || !strcmp(argv[1], "idle-more-error")) {
            if (!tap(shell, "More")) return 1;
            if (!strcmp(argv[1], "idle-more-error")) {
                orb_idle_request_t request;
                if (!orb_model_take_idle_request(&request) ||
                    !orb_model_complete_idle(&request, NULL, 0, NULL, "timeout")) return 1;
                settle(shell);
            }
        }
        if (!strcmp(argv[1], "sessions-touched")) {
            if (!reveal(shell, "Claude")) return 1;
            lv_obj_t *control = inline_switch(shell, "Claude");
            if (!control) return 1;
            bool checked = orb_kit_switch_get_checked(control);
            lv_area_t box; lv_obj_get_coords(control, &box);
            s_input.point = (lv_point_t){(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
            s_input.state = LV_INDEV_STATE_PRESSED;
            lv_indev_read(s_pointer);
            settle(shell);
            if (!lv_obj_has_state(control, LV_STATE_PRESSED) || orb_kit_switch_get_checked(control) != checked) return 1;
        }
        /* SessionLive keeps the stream inline; the former summary subpage is
         * intentionally represented by the same retained conversation view. */
        const char *access = (!strcmp(argv[1], "access-text-size") || !strcmp(argv[1], "access-text-selected")) ? "Text size" :
            !strcmp(argv[1], "access-bold") ? "Bold text" : !strcmp(argv[1], "access-motion") ? "Reduce motion" :
            !strcmp(argv[1], "access-transparency") ? "Reduce transparency" :
            !strcmp(argv[1], "access-contrast") ? "Increase contrast" : NULL;
        if (access) {
            if (!reveal(shell, access)) return 1;
            lv_obj_t *toggle = inline_switch(shell, access);
            bool checked = toggle && orb_kit_switch_get_checked(toggle);
            if (!tap(shell, access)) return 1;
            if (toggle) {
                toggle = inline_switch(shell, access);
                if (!toggle || orb_kit_switch_get_checked(toggle) == checked) return 1;
            }
        }
        if (!strcmp(argv[1], "access-text-selected") && !reveal(shell, orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? "Largest" : "Default")) return 1;
        const char *access_list = !strcmp(argv[1], "access-list-motion") ? "Reduce motion" :
            !strcmp(argv[1], "access-list-transparency") ? "Reduce transparency" :
            !strcmp(argv[1], "access-list-contrast") ? "Increase contrast" : NULL;
        if (access_list && !reveal(shell, access_list)) return 1;
        if (!strcmp(argv[1], "about-device-id") && !reveal(shell, "Device ID")) return 1;
        if (!strcmp(argv[1], "about-disclosure") && !reveal(shell, "Licenses")) return 1;
        if (!strncmp(argv[1], "about-licenses", 14) && (!reveal(shell, "Licenses") || !tap(shell, "Licenses"))) return 1;
        if (!strcmp(argv[1], "about-licenses-end") && !reveal(shell, "Phosphor Icons")) return 1;
        if (!strcmp(argv[1], "about-licenses-middle") && !reveal(shell, "Framework7 Icons")) return 1;
        if (!strncmp(argv[1], "sound-", 6)) {
            const char *target = !strcmp(argv[1], "sound-volume") ? "Volume" : "Quiet hours";
            if (!reveal(shell, target) || !tap(shell, target)) return 1;
            if (!strcmp(argv[1], "sound-start") && (!reveal(shell, "Starts") || !tap(shell, "Starts"))) return 1;
            if (!strcmp(argv[1], "sound-end") && (!reveal(shell, "Ends") || !tap(shell, "Ends"))) return 1;
        }
        if (!strcmp(argv[1], "apps-next")) { orb_shell_twist(shell, 1); settle(shell); }
        unsigned next = 0;
        if (!strcmp(argv[1], "agents-next") || !strcmp(argv[1], "settings-next")) next = 1;
        else if (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3) {
            if (!strcmp(argv[1], "choice-options-next")) next = 1;
        }
        if (!strncmp(argv[1], "home-controls", 13)) next = 1;
        if (!strcmp(argv[1], "home-stack")) {
            orb_model_snapshot_t model;
            if (orb_model_snapshot(&model)) next = (unsigned)model.card_count;
        }
        bool paging = !strncmp(argv[1], "control-", 8) || !strncmp(argv[1], "home-controls", 13) || !strcmp(argv[1], "home-stack");
        for (unsigned page = 0; page < next; page++) {
            if (paging && orb_shell_surface(shell) == ORB_SHELL_CONTROL_CENTER) {
                nav_pointer(233, 320, true); advance(shell, 40);
                nav_pointer(233, 120, true); advance(shell, 80);
                nav_pointer(233, 120, false); settle(shell);
            } else {
                if (!orb_shell_twist(shell, !strcmp(argv[1], "agents-next") || paging ? 1 : 2)) return 1;
                settle(shell);
            }
        }
        ok = sim_display_save_png(argv[2]) && verify_back(argv[1]);
    }
    orb_shell_destroy(shell);
    lv_indev_delete(s_pointer);
    return ok ? 0 : 1;
}
