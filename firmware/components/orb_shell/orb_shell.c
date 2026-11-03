#include "orb_shell.h"
#include "orb_nav.h"
#include "orb_nav_admission.h"
#include "orb_workspace.h"
#include "orb_shell_power.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_status.h"
#include "orb_ui.h"
#include "orb_kit_shell_controls.h"
#include "orb_kit.h"
#include "orb_kit_motion.h"
#include "orb_kit_pages.h"
#include "orb_page.h"
#include "orb_input.h"
#include "orb_apps.h"
#include "orb_time.h"
#include "orb_time_host.h"
#include "orb_feedback.h"
#include "orb_sound.h"
#include "orb_sound_overlay.h"
#include "orb_system_state.h"
#include "system/mac_screen.h"
#include "system/ota_screen.h"
#include "system/boot_screen.h"
#include "orb_system_copy.h"
#include "orb_place_view.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

/* Optional provider hooks: the base shell simulator also builds without games.
 * Strong game-provider definitions enqueue requests for main-owned lifecycle. */
__attribute__((weak)) bool orb_games_request_pause(void) { return false; }
__attribute__((weak)) bool orb_games_request_back(void) { return false; }

typedef enum { MENU_CLOSED, MENU_CONNECTION, MENU_MAINTENANCE, MENU_FORGET, MENU_UNPAIR, MENU_UPDATES } menu_t;

struct orb_shell {
    lv_obj_t *root, *system;
    orb_system_scaffold_t scaffold;
    orb_app_host_t *boot_host;
    orb_app_host_t *ota_host;
    orb_ota_wait_t ota_wait;
    orb_app_host_t *mac_host;
    int16_t mac_scroll;
    lv_obj_t *mic;
    orb_shell_snapshot_t snapshot;
    orb_sound_overlay_t sound_overlay;
    bool alarm_seen, alarm_presentation_dismissed, onboarding_seen, alarm_was_paused;
    orb_app_host_t *alarm_host; /* the alert layer's framework Time screen */
    orb_shell_action_cb_t action;
    void *ctx;
    orb_shell_speech_stop_cb_t speech_stop;
    void *speech_stop_ctx;
    menu_t menu;
    view_t view;
    orb_shell_action_t primary_action;
    bool primary_is_menu, primary_is_maintenance, check_pending, ota_background;
    orb_workspace_t *workspace;
    orb_shell_calm_t *calm;
    lv_point_t press_start;
    orb_nav_t system_nav;
    lv_timer_t *system_nav_timer;
    lv_indev_t *system_input;
    lv_indev_t *contact_input;
    lv_obj_t *contact_target;
    lv_point_t contact_start;
    uint32_t contact_at;
    bool contact_down, contact_cancelled;
    bool system_suppress_click, system_display_active, system_always_on;
    bool nav_benchmark, fonts_pending;
    menu_t nav_saved_menu;
    int nav_saved_reading;
    lv_obj_t *button_sheet, *button_sheet_detail;
    orb_sheet_lifecycle_t sheet;
    bool talk_held, talk_synthetic, talk_from_alert;
    lv_obj_t *app_parent;
    orb_shell_surface_t app_return, app_overlay_surface;
    bool app_overlay, app_was_paused;
};

static orb_screen_instance_t *alarm_view(const orb_shell_t *s) { return orb_app_host_current(s->alarm_host); }
static void render(orb_shell_t *s);
static void button_sheet(orb_shell_t *s, bool paused);

static bool volume_control_visible(const orb_shell_t *s)
{
    /* A retained editor behind another modal is not what the owner sees. */
    return s->view == VIEW_HOME && !s->button_sheet && !alarm_view(s) &&
        !orb_sound_overlay_has_speech(&s->sound_overlay) && orb_workspace_volume_control_visible(s->workspace);
}

static void contact_sample(orb_shell_t *s, lv_obj_t *target)
{
    lv_point_t point;
    lv_indev_get_point(s->contact_input, &point);
    int dx = point.x - s->contact_start.x, dy = point.y - s->contact_start.y;
    s->contact_cancelled |= dx * dx + dy * dy >= 100 ||
        lv_indev_get_scroll_obj(s->contact_input) != NULL ||
        lv_tick_get() - s->contact_at >= 500;
    if (!s->contact_cancelled) return;
    /* Keep native scrolling and local drag handlers alive. Resetting the
     * indev here would strand a list when its DOWN landed on a button. */
    for (lv_obj_t *obj = target; obj; obj = lv_obj_get_parent(obj)) {
        lv_obj_remove_state(obj, LV_STATE_PRESSED);
        orb_feedback_cancel(obj);
        orb_kit_press_cancel(obj);
        if (obj == s->root) break;
    }
}

static void contact_target_event(lv_event_t *event)
{
    orb_shell_t *s = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (lv_event_get_target_obj(event) == s->contact_target) s->contact_target = NULL;
    }
    else if (s->contact_input && s->contact_down && lv_event_get_code(event) == LV_EVENT_PRESSING)
        contact_sample(s, lv_event_get_target_obj(event));
}

static void contact_target(orb_shell_t *s, lv_obj_t *target)
{
    if (s->contact_target == target) return;
    if (s->contact_target)
        lv_obj_remove_event_cb_with_user_data(s->contact_target, contact_target_event, s);
    s->contact_target = target;
    if (target) lv_obj_add_event_cb(target, contact_target_event, LV_EVENT_ALL, s);
}

static void contact_input_event(lv_event_t *event)
{
    orb_shell_t *s = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) { s->contact_input = NULL; return; }
    if (!s->contact_input) return;
    if (code == LV_EVENT_PRESSED) contact_target(s, lv_event_get_param(event));
    if (code == LV_EVENT_PRESSED && !s->contact_down) {
        orb_kit_motion_interrupt(NULL); /* Includes system and top-layer ink; waits survive. */
        /* Once attached, this guard also sees top-layer controls. Their
         * contacts do not bubble through the shell root. */
        lv_indev_get_point(s->contact_input, &s->contact_start);
        s->contact_at = lv_tick_get();
        s->contact_cancelled = false;
        s->contact_down = true;
    }
    lv_obj_t *target = lv_event_get_param(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED) {
        contact_sample(s, target);
        if (code == LV_EVENT_RELEASED) s->contact_down = false;
        else if (s->contact_cancelled) lv_indev_stop_processing(s->contact_input);
    }
}

static void contact_event(lv_event_t *event)
{
    orb_shell_t *s = lv_event_get_user_data(event);
    lv_indev_t *input = lv_indev_active();
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        if (s->contact_input != input) {
            if (s->contact_input)
                lv_indev_remove_event_cb_with_user_data(s->contact_input, contact_input_event, s);
            s->contact_input = input;
            lv_indev_add_event_cb(input, contact_input_event, LV_EVENT_ALL, s);
            s->contact_down = false;
        }
        if (!s->contact_down) {
            orb_kit_motion_interrupt(NULL);
            lv_indev_get_point(input, &s->contact_start);
            s->contact_at = lv_tick_get();
            s->contact_cancelled = false;
            s->contact_down = true;
            contact_target(s, lv_event_get_target_obj(event));
        }
    }
    if (s->contact_input == input) contact_sample(s, lv_event_get_target_obj(event));
}
static void close_app_overlay(orb_shell_t *s)
{
    if (!s->app_overlay) return;
    s->app_overlay = false;
    lv_obj_set_parent(s->root, s->app_parent);
    if (s->workspace) orb_workspace_navigate(s->workspace, s->app_return);
    if (!s->app_was_paused && orb_apps_active()) orb_apps_request_pause(false);
    s->app_parent = NULL;
}
static void close_button_sheet(orb_shell_t *s)
{
    if (!s->button_sheet) return;
    lv_obj_delete(s->button_sheet);
    s->button_sheet = s->button_sheet_detail = NULL;
    orb_workspace_covered(s->workspace, false);
    if (orb_sheet_end(&s->sheet, s->app_overlay)) orb_apps_request_pause(false);
}

static void dismiss_button_sheet(orb_shell_t *s)
{
    if (!s->button_sheet) return;
    orb_sound_play(ORB_SOUND_CLOSE);
    close_button_sheet(s);
}

static void sheet_gesture(lv_event_t *event)
{
    orb_shell_t *s = lv_event_get_user_data(event);
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) lv_indev_get_point(input, &s->press_start);
    else if (hypotf(s->press_start.x - 233, s->press_start.y - 233) >= 193 &&
             s->press_start.x < 233 && abs(s->press_start.x - 233) > abs(s->press_start.y - 233) &&
             lv_indev_get_gesture_dir(input) == LV_DIR_RIGHT) {
        lv_indev_wait_release(input);
        dismiss_button_sheet(s);
    }
}

static void sheet_clicked(lv_event_t *event)
{
    orb_shell_t *s = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target_obj(event);
    intptr_t action = (intptr_t)lv_obj_get_user_data(target);
    if (action && !orb_input_require_physical(s->root)) return;
    bool paused = s->sheet.pause_page;
    dismiss_button_sheet(s);
    if (!action) return;
    if (paused) {
        if (action == 3) orb_apps_request_pause(false);
        else orb_apps_request_exit();
        return;
    }
    if (s->action) s->action(action == 1 ? ORB_SHELL_ACTION_SLEEP : ORB_SHELL_ACTION_RESTART, s->ctx);
}

static lv_obj_t *sheet_control(orb_shell_t *s, const char *caption, intptr_t action,
                               int cy, bool primary)
{
    lv_obj_t *obj = primary ? orb_kit_button_primary_create(s->button_sheet, caption, ORB_KIT_CODEX) :
                             orb_kit_button_secondary_create(s->button_sheet, caption);
    if (!obj) return NULL;
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, cy - lv_obj_get_height(obj) / 2);
    lv_obj_set_user_data(obj, (void *)action);
    lv_obj_set_event_bubble(obj, true);
    lv_obj_add_event_cb(obj, sheet_clicked, LV_EVENT_CLICKED, s);
    orb_glass_materialise(obj, true);
    return obj;
}

static void button_sheet_status(orb_shell_t *s)
{
    if (!s->button_sheet_detail) return;
    char version[48], status[80], reserve[80];
    orb_system_software_version(version, sizeof(version), s->snapshot.firmware_version);
    if (s->snapshot.battery_pct >= 0)
        snprintf(status, sizeof(status), "%d%% · %s", s->snapshot.battery_pct, version);
    else snprintf(status, sizeof(status), "%s%s%s", s->snapshot.usb ? "USB" : "",
                  s->snapshot.usb ? " · " : "", version);
    snprintf(reserve, sizeof(reserve), "100%% · %s", version);
    orb_kit_value_set(s->button_sheet_detail, status, reserve);
}

static void button_sheet(orb_shell_t *s, bool paused)
{
    bool opening = !s->button_sheet || s->sheet.pause_page != paused;
    bool was_paused = s->button_sheet ? s->sheet.runtime_was_paused : orb_apps_paused();
    close_button_sheet(s);
    if (orb_sheet_begin(&s->sheet, paused, orb_apps_active() != NULL, was_paused))
        orb_apps_request_pause(true);
    s->button_sheet = orb_kit_sheet_surface(lv_display_get_layer_top(lv_obj_get_display(s->root)));
    orb_workspace_covered(s->workspace, true);
    lv_obj_add_event_cb(s->button_sheet, sheet_gesture, LV_EVENT_PRESSED, s);
    lv_obj_add_event_cb(s->button_sheet, sheet_gesture, LV_EVENT_GESTURE, s);
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    orb_kit_rim_title_create(s->button_sheet, paused ? "Paused" : "Power", ORB_KIT_GREY);
    lv_obj_t *close = orb_kit_close_create(s->button_sheet);
    if (close) {
        lv_obj_add_event_cb(close, sheet_clicked, LV_EVENT_CLICKED, s);
        orb_glass_materialise(close, true);
    }
    if (paused) {
        /* The one primary is always on the bottom arc. BOOT also dismisses. */
        sheet_control(s, "Exit app", 2, 266, false);
        sheet_control(s, "Resume", 3, 380, true);
    } else {
        lv_obj_t *detail = s->button_sheet_detail = lv_label_create(s->button_sheet);
        orb_text_apply(detail, ORB_TEXT_FOOTNOTE2, ORB_TEXT_TABULAR, 0);
        lv_obj_set_style_text_color(detail, ORB_KIT_SECONDARY, 0);
        lv_obj_set_style_text_opa(detail, ORB_KIT_SECONDARY_OPA, 0);
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(detail, 354);
        lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, 151);
        button_sheet_status(s);
        /* Power actions remain equal neutral decisions; neither is default. */
        sheet_control(s, "Sleep display", 1, 220, false);
        sheet_control(s, "Restart", 2, 298, false);
        lv_obj_t *hint = lv_label_create(s->button_sheet);
        lv_label_set_text(hint, ax ? "Hold PWR 10 s\nto switch off" : "Keep holding PWR for 10 s\nto switch off");
        orb_text_apply(hint, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 0);
        lv_obj_set_style_text_color(hint, ORB_KIT_SECONDARY, 0);
        lv_obj_set_style_text_opa(hint, ORB_KIT_SECONDARY_OPA, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(hint, ax ? 250 : 280);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 342);
    }
    if (orb_workspace_increase_contrast(s->workspace)) orb_system_increase_contrast(s->button_sheet);
    if (opening) orb_sound_play(ORB_SOUND_OPEN);
}

static void workspace_action(orb_shell_action_t action, void *ctx)
{
    orb_shell_t *s = ctx;
    if (s->action) s->action(action, s->ctx);
}

static void workspace_service(void *ctx, bool connection)
{
    orb_shell_t *s = ctx;
    s->menu = connection ? MENU_CONNECTION : MENU_UPDATES;
    if (!connection) s->ota_background = false;
    render(s);
}

static void visible(lv_obj_t *obj, bool show)
{
    lv_obj_set_hidden(obj, !show);
    orb_kit_scroll_arc_refresh(obj);
}

static void text(lv_obj_t *label, const char *value)
{
    orb_kit_value_info_t live;
    if (orb_kit_value_info(label, &live)) orb_kit_value_set(label, value, NULL);
    else if (strcmp(lv_label_get_text(label), value) != 0) lv_label_set_text(label, value);
}

static bool busy(const orb_shell_snapshot_t *m)
{
    return m->ota == ORB_SHELL_OTA_DOWNLOADING || m->ota == ORB_SHELL_OTA_VERIFYING ||
           m->ota == ORB_SHELL_OTA_READY;
}

static bool online(const orb_shell_snapshot_t *m)
{
    return m->net == ORB_SHELL_NET_CONNECTED && m->link == ORB_SHELL_LINK_ONLINE;
}

static view_t choose_view(orb_shell_t *s)
{
    const orb_shell_snapshot_t *m = &s->snapshot;
    /* Present a new transfer immediately. Intentional Back/PWR can resume
     * local work while OTA continues with its existing sleep inhibitor. */
    if (busy(m) && !s->ota_background) {
        s->menu = MENU_CLOSED;
        if (m->ota == ORB_SHELL_OTA_DOWNLOADING) return VIEW_DOWNLOAD;
        return m->ota == ORB_SHELL_OTA_READY ? VIEW_READY : VIEW_VERIFY;
    }
    if (m->identify) return VIEW_IDENTIFY;
    if (s->menu == MENU_CONNECTION) return VIEW_CONNECTION;
    if (s->menu == MENU_MAINTENANCE) return VIEW_MAINTENANCE;
    if (s->menu == MENU_FORGET) return VIEW_FORGET;
    if (s->menu == MENU_UNPAIR) return VIEW_UNPAIR;
    if (m->service_error[0]) return VIEW_SERVICE;
    if (m->net == ORB_SHELL_NET_PROVISIONING ||
        ((m->net == ORB_SHELL_NET_FAILED || m->net == ORB_SHELL_NET_CONNECTING) &&
         m->pop[0] && m->service_name[0])) return VIEW_SETUP;
    if (m->net == ORB_SHELL_NET_STARTING) return VIEW_START;
    if (m->link == ORB_SHELL_LINK_PAIRING) return VIEW_PAIR;
    /* HIG §1: connection loss is status, not a modal navigation barrier.
     * Only an online offer may interrupt local apps; installation stays above. */
    if (online(m) && m->ota == ORB_SHELL_OTA_OFFERED && !s->ota_background) return VIEW_OFFER;
    if (online(m) && m->ota == ORB_SHELL_OTA_FAILED && !s->ota_background) return VIEW_FAILED;
    if (s->menu == MENU_UPDATES) return VIEW_UPDATES;
    return VIEW_HOME;
}

static void line(orb_shell_t *s, line_t which, const char *value, orb_text_style_t style,
                 orb_text_variant_t variant, unsigned max_lines)
{
    orb_system_scaffold_line(&s->scaffold, s->view, which, value, style, variant, max_lines);
}

static void primary(orb_shell_t *s, const char *label, orb_shell_action_t action, bool enabled)
{
    orb_system_scaffold_primary(&s->scaffold, label, enabled);
    s->primary_action = action;
}

static bool updates_view(view_t view)
{
    return view == VIEW_UPDATES || view == VIEW_OFFER || view == VIEW_DOWNLOAD ||
           view == VIEW_VERIFY || view == VIEW_READY || view == VIEW_FAILED;
}

static orb_ota_page_t ota_page(view_t view)
{
    switch (view) {
    case VIEW_OFFER: return 1;
    case VIEW_DOWNLOAD: return 2;
    case VIEW_VERIFY: return 3;
    case VIEW_READY: return 4;
    case VIEW_FAILED: return 5;
    default: return 0;
    }
}

static bool ota_command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    (void)instance;
    orb_shell_t *s = context;
    if (s->system_suppress_click || !online(&s->snapshot)) return false;
    orb_shell_action_t action = intent->value.command.action;
    if (action == ORB_SHELL_ACTION_UPDATE && !orb_input_require_physical(s->root)) return false;
    if (action == ORB_SHELL_ACTION_CHECK_UPDATE) { s->check_pending = true; s->ota_wait.waiting = false; }
    if (s->action) s->action(action, s->ctx);
    render(s);
    return true;
}

static void ota_line(void *context, unsigned which, const char *value,
    orb_text_style_t style, orb_text_variant_t variant, unsigned max_lines)
{
    line(context, which, value, style, variant, max_lines);
}
static void ota_primary(void *context, const char *label, orb_shell_action_t action, bool enabled)
{
    primary(context, label, action, enabled);
}
static bool ota_scaffold(void *context, orb_ota_scaffold_t *port)
{
    orb_shell_t *s = context;
    *port = (orb_ota_scaffold_t){.system = s->system, .reading = s->scaffold.reading, .ring = s->scaffold.ring,
        .art = s->scaffold.art, .wifi = s->scaffold.wifi, .battery = s->scaffold.battery, .power = s->scaffold.power,
        .settings = s->scaffold.settings, .primary = s->scaffold.primary, .cancel = s->scaffold.cancel,
        .line = ota_line, .primary_action = ota_primary, .context = s, .wait = &s->ota_wait};
    memcpy(port->lines, s->scaffold.lines, sizeof(port->lines));
    return true;
}
static void ota_controls(orb_shell_t *s, bool create)
{
    if (!create) { orb_app_host_detach_page(s->ota_host, s->system); return; }
    if (!s->ota_host) s->ota_host = orb_ota_host_create(false, ota_command, s);
    (void)orb_ota_host_ensure(s->ota_host, s->system, &(orb_ota_source_t){ota_scaffold, s});
}

static lv_obj_t *system_reader(orb_shell_t *s)
{
    lv_obj_t *reader = orb_mac_host_reader(s->mac_host);
    return reader ? reader : s->scaffold.reading;
}

static bool mac_view(view_t view)
{
    return view == VIEW_CONNECTION || view == VIEW_MAINTENANCE ||
           view == VIEW_FORGET || view == VIEW_UNPAIR;
}

static bool mac_command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    (void)instance;
    orb_shell_t *s = context;
    if (s->system_suppress_click) return false;
    intptr_t command = intent->value.command.action;
    if (command == 4 || command == 5) {
        if (!orb_input_require_physical(s->root)) return false;
        s->menu = MENU_CLOSED;
        if (s->action) s->action(command == 4 ? ORB_SHELL_ACTION_FORGET_NETWORK :
                                 ORB_SHELL_ACTION_UNPAIR, s->ctx);
    } else s->menu = command == 2 ? MENU_FORGET : command == 3 ? MENU_UNPAIR : MENU_MAINTENANCE;
    render(s);
    return true;
}


static void layout_mac(orb_shell_t *s)
{
    if (!s->mac_host) s->mac_host = orb_mac_host_create(false, mac_command, s);
    if (!orb_mac_host_attach(s->mac_host, s->system,
        (orb_mac_page_t)(s->view - VIEW_CONNECTION), &s->snapshot, s->mac_scroll)) return;
    lv_obj_t *root = orb_screen_root(orb_app_host_current(s->mac_host));
    for (unsigned i = 0; i < lv_obj_get_child_count(s->system); ++i) {
        lv_obj_t *child = lv_obj_get_child(s->system, i);
        if (child != root) visible(child, false);
    }
}

static bool boot_view(view_t view)
{
    return view == VIEW_START || view == VIEW_SETUP || view == VIEW_PAIR ||
        view == VIEW_SERVICE || view == VIEW_IDENTIFY;
}
static bool boot_command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    (void)instance; (void)intent;
    orb_shell_t *s = context;
    if (s->system_suppress_click) return false;
    s->menu = MENU_CONNECTION;
    render(s);
    return true;
}
static void boot_controls(orb_shell_t *s)
{
    if (!boot_view(s->view)) { orb_app_host_detach_page(s->boot_host, s->system); return; }
    if (!s->boot_host) s->boot_host = orb_boot_host_create(false, boot_command, s);
    (void)orb_boot_host_ensure(s->boot_host, s->system, &s->scaffold);
}

static void layout(orb_shell_t *s)
{
    if (updates_view(s->view)) { orb_ota_screen_layout(orb_app_host_current(s->ota_host)); return; }
    if (mac_view(s->view)) { layout_mac(s); return; }
    if (boot_view(s->view)) { orb_boot_screen_layout(&s->scaffold, s->view); return; }
    orb_system_scaffold_layout(&s->scaffold, s->view);
}

static void render(orb_shell_t *s)
{
    const orb_shell_snapshot_t *m = &s->snapshot;
    view_t previous_view = s->view;
    if (orb_mac_host_reader(s->mac_host) && previous_view == VIEW_CONNECTION)
        s->mac_scroll = (int16_t)lv_obj_get_scroll_y(orb_mac_host_reader(s->mac_host));
    if (updates_view(previous_view)) orb_ota_screen_capture(orb_app_host_current(s->ota_host), ota_page(previous_view));
    s->view = choose_view(s);
    visible(s->scaffold.system_title, false);
    if (!mac_view(s->view)) orb_app_host_detach_page(s->mac_host, s->system);
    ota_controls(s, updates_view(s->view));
    boot_controls(s);
    if (previous_view != s->view) lv_obj_scroll_to_y(s->scaffold.reading, 0, LV_ANIM_OFF);
    if (s->view == VIEW_HOME) {
        /* The app tree is created after mandatory setup has finished.
         * Its pages are replaced on navigation, never kept as hidden screens. */
        if (!s->workspace) {
            s->workspace = orb_workspace_create(s->root, workspace_action, workspace_service, s);
            if (s->workspace) {
                /* Sound init has loaded NVS synchronously. Use the shell's
                 * authority before its environment tick can reapply Silent. */
                orb_sound_preferences_t sound;
                orb_sound_preferences(&sound);
                orb_shell_set_silent(s, sound.silent);
            }
        }
        if (s->workspace) {
            visible(s->system, false);
            orb_workspace_covered(s->workspace, s->button_sheet != NULL);
            orb_workspace_update(s->workspace, m);
            orb_workspace_show(s->workspace, true);
            return;
        }
    }
    visible(s->system, true);
    visible(s->scaffold.art, false);
    if (s->workspace) orb_workspace_show(s->workspace, false);
    lv_obj_t *controls[] = {s->scaffold.settings, s->scaffold.primary, s->scaffold.forget, s->scaffold.unpair, s->scaffold.cancel};
    /* A service interruption retargets immediately. Cancel old entrances before
     * assigning visibility, so no timer can reveal a now-hidden control. */
    for (unsigned i = 0; i < sizeof(controls) / sizeof(*controls); i++) {
        orb_glass_set_amount(controls[i], 1.0f);
        /* Turning Increase Contrast off restores the ordinary material rim. */
        lv_obj_remove_local_style_prop(controls[i], LV_STYLE_BORDER_WIDTH, 0);
    }
    for (unsigned i = 0; i < LINE_COUNT; i++) {
        line(s, i, "", ORB_TEXT_BODY, ORB_TEXT_REGULAR, 1);
        lv_obj_remove_local_style_prop(s->scaffold.lines[i], LV_STYLE_TEXT_LETTER_SPACE, 0);
    }
    primary(s, "", ORB_SHELL_ACTION_CHECK_UPDATE, false);
    s->primary_is_menu = false;
    s->primary_is_maintenance = false;
    bool menu_open = s->view == VIEW_CONNECTION || s->view == VIEW_MAINTENANCE || s->view == VIEW_FORGET ||
                     s->view == VIEW_UNPAIR || s->view == VIEW_UPDATES;
    visible(s->scaffold.settings, !menu_open && !busy(m) && !m->identify && s->view != VIEW_SERVICE);
    visible(s->scaffold.forget, s->view == VIEW_MAINTENANCE);
    visible(s->scaffold.unpair, s->view == VIEW_MAINTENANCE);
    visible(s->scaffold.ring, busy(m) || s->view == VIEW_FAILED);
    if (!updates_view(s->view)) orb_kit_ring_set_value(s->scaffold.ring, m->ota == ORB_SHELL_OTA_DOWNLOADING || m->ota == ORB_SHELL_OTA_FAILED ?
                     LV_CLAMP(0, m->progress, 100) : 100, false);
    lv_obj_set_style_arc_color(s->scaffold.ring, m->ota == ORB_SHELL_OTA_FAILED ? ORB_RED : ORB_BLUE, LV_PART_INDICATOR);


    switch (s->view) {
    case VIEW_START: case VIEW_SETUP: case VIEW_PAIR: case VIEW_SERVICE: case VIEW_IDENTIFY:
        orb_boot_screen_content(orb_app_host_current(s->boot_host), &s->scaffold, s->view, m);
        if (s->view == VIEW_SERVICE) { s->primary_action = ORB_SHELL_ACTION_CHECK_UPDATE; s->primary_is_menu = true; }
        break;
    case VIEW_HOME:
        line(s, LINE_TITLE, m->clock_time[0] ? "" : (m->device_name[0] ? m->device_name : "Orb"),
             ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED, 1);
        if (m->clock_time[0]) {
            line(s, LINE_CODE, m->clock_time, ORB_TEXT_TITLE1, ORB_TEXT_TABULAR, 1);
            orb_clock_apply(s->scaffold.lines[LINE_CODE], ORB_CLOCK_MEDIUM, 0);
        }
        line(s, LINE_BODY, "Connected", ORB_TEXT_BODY, ORB_TEXT_REGULAR, 1);
        line(s, LINE_DETAIL, m->hub_name, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 1);
        primary(s, "Updates", ORB_SHELL_ACTION_CHECK_UPDATE, true);
        break;
    case VIEW_UPDATES: case VIEW_OFFER: case VIEW_DOWNLOAD: case VIEW_VERIFY: case VIEW_READY: case VIEW_FAILED:
        orb_ota_screen_content(orb_app_host_current(s->ota_host), ota_page(s->view), m, s->check_pending);
        break;
    case VIEW_CONNECTION: /* layout_mac() owns the visible rows and their copy. */
        primary(s, "Maintenance", ORB_SHELL_ACTION_CHECK_UPDATE, true);
        s->primary_is_maintenance = true;
        break;
    case VIEW_MAINTENANCE:
        line(s, LINE_TITLE, "Maintenance", ORB_TEXT_TITLE3, ORB_TEXT_EMPHASIZED, 1);
        break;
    case VIEW_FORGET:
        line(s, LINE_TITLE, "Forget Wi-Fi?", ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED, 1);
        line(s, LINE_BODY, "You'll need to set up Wi-Fi again.", ORB_TEXT_BODY, ORB_TEXT_REGULAR, 3);
        primary(s, "Forget Wi-Fi", ORB_SHELL_ACTION_FORGET_NETWORK, true);
        break;
    case VIEW_UNPAIR:
        line(s, LINE_TITLE, "Unpair this Mac?", ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED, 1);
        line(s, LINE_BODY, "Orb will ask for a new pairing code. Wi-Fi stays connected.", ORB_TEXT_BODY, ORB_TEXT_REGULAR, 3);
        primary(s, "Unpair", ORB_SHELL_ACTION_UNPAIR, true);
        break;
    }
    orb_system_scaffold_identity(&s->scaffold, s->view != VIEW_HOME && !updates_view(s->view) && !mac_view(s->view));
    if (!updates_view(s->view)) orb_system_scaffold_status(&s->scaffold, &s->snapshot);
    layout(s);
    if (s->view != VIEW_PAIR && s->view != VIEW_SETUP && s->view != VIEW_DOWNLOAD &&
        s->view != VIEW_VERIFY && s->view != VIEW_READY && s->view != VIEW_FAILED &&
        !updates_view(s->view) && !mac_view(s->view)) orb_system_scaffold_layout_status(&s->scaffold, &s->snapshot);
    if (orb_workspace_increase_contrast(s->workspace)) orb_system_increase_contrast(s->system);
    if (previous_view != s->view) {
        /* HIG §§2.2/6: develop the material after layout; the transient absent
         * state must not change the space reserved for its eventual target. */
        for (unsigned i = 0; i < sizeof(controls) / sizeof(*controls); i++) {
            if (lv_obj_is_hidden(controls[i])) continue;
            orb_glass_set_amount(controls[i], 0.0f);
            orb_glass_materialise(controls[i], true);
        }
    }
}

static void clicked(lv_event_t *event)
{
    orb_shell_t *s = lv_event_get_user_data(event);
    if (s->system_suppress_click) return;
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (orb_app_host_current(s->boot_host) && boot_view(s->view) && (target == s->scaffold.settings ||
        (target == s->scaffold.primary && s->primary_is_menu))) {
        orb_boot_screen_activate(orb_app_host_current(s->boot_host));
        return;
    }
    if (target == s->scaffold.settings || (target == s->scaffold.primary && s->primary_is_menu)) s->menu = MENU_CONNECTION;
    else if (target == s->scaffold.primary && s->primary_is_maintenance) s->menu = MENU_MAINTENANCE;
    else if (target == s->scaffold.cancel)
        s->menu = s->menu == MENU_CONNECTION || s->menu == MENU_UPDATES ? MENU_CLOSED :
                  s->menu == MENU_MAINTENANCE ? MENU_CONNECTION : MENU_MAINTENANCE;
    else if (target == s->scaffold.forget) s->menu = MENU_FORGET;
    else if (target == s->scaffold.unpair) s->menu = MENU_UNPAIR;
    else if (target == s->scaffold.primary) {
        orb_shell_action_t action = s->primary_action;
        if ((action == ORB_SHELL_ACTION_UPDATE || action == ORB_SHELL_ACTION_FORGET_NETWORK ||
             action == ORB_SHELL_ACTION_UNPAIR) && !orb_input_require_physical(s->root)) return;
        if ((action == ORB_SHELL_ACTION_CHECK_UPDATE || action == ORB_SHELL_ACTION_UPDATE) &&
            !online(&s->snapshot)) return;
        if (s->view != VIEW_UPDATES) s->menu = MENU_CLOSED;
        if (action == ORB_SHELL_ACTION_CHECK_UPDATE) { s->check_pending = true; s->ota_wait.waiting = false; }
        if (s->action) s->action(action, s->ctx);
    }
    render(s);
}

static lv_obj_t *button(orb_shell_t *s, const char *label, int32_t cx, int32_t cy,
                        int32_t width)
{
    lv_obj_t *obj = orb_kit_recovery_button_create(s->system, label, cx, cy, width);
    lv_obj_add_event_cb(obj, clicked, LV_EVENT_CLICKED, s);
    return obj;
}

static void fonts_apply(void *context)
{
    orb_shell_t *s = context;
    s->fonts_pending = false;
    render(s);
    if (s->button_sheet) button_sheet(s, s->sheet.pause_page);
}

static void fonts_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    LV_UNUSED(subject);
    orb_shell_t *s = lv_observer_get_user_data(observer);
    if (s->fonts_pending) return;
    /* Kit controls also observe the shared font subject. Rebuilding a screen
     * inside that delivery would remove/add observers under its iterator and
     * can repeatedly notify newly created controls. Reflow once after delivery. */
    s->fonts_pending = lv_async_call(fonts_apply, s) == LV_RESULT_OK;
}

static void system_rotary_changed(void *ctx, float value, float delta)
{
    orb_shell_t *s = ctx;
    (void)delta;
    if ((int32_t)lrintf(value) != lv_obj_get_scroll_y(system_reader(s))) orb_sound_play(ORB_SOUND_DETENT);
    lv_obj_scroll_to_y(system_reader(s), (int32_t)lrintf(value), LV_ANIM_OFF);
}

static void system_nav_stop(orb_shell_t *s)
{
    orb_nav_touch(&s->system_nav, ORB_NAV_TOUCH_CANCEL, 0, 0, lv_tick_get(), false, false);
    if (s->system_input) {
        /* Suspending an idle menu is not a moved contact. Only a pointer
         * still held through this cancellation must lose its later click. */
        bool held = lv_indev_get_state(s->system_input) == LV_INDEV_STATE_PRESSED;
        if (held)
            s->system_suppress_click = true;
        lv_indev_reset(s->system_input, NULL);
        /* An event-driven input may not read again until the next press.
         * Waiting for another release after an ordinary lift swallows it. */
        if (held) lv_indev_wait_release(s->system_input);
        s->system_input = NULL;
    }
    lv_obj_stop_scroll_anim(system_reader(s));
    if (s->system_nav_timer) lv_timer_pause(s->system_nav_timer);
}

static void system_nav_tick(lv_timer_t *timer)
{
    orb_shell_t *s = lv_timer_get_user_data(timer);
    orb_nav_t *nav = &s->system_nav;
    uint32_t now = lv_tick_get();
    if (!s->system_display_active || !lv_obj_is_visible(system_reader(s)) ||
        !lv_obj_is_visible(s->system) || alarm_view(s) || s->button_sheet) {
        system_nav_stop(s);
        return;
    }
    if (nav->touch.down && s->system_input) {
        lv_point_t p; lv_indev_get_point(s->system_input, &p);
        orb_nav_touch_phase_t phase = lv_indev_get_state(s->system_input) == LV_INDEV_STATE_PRESSED ?
            ORB_NAV_TOUCH_MOVE : ORB_NAV_TOUCH_UP;
        orb_nav_event_t event = orb_nav_touch(nav, phase, p.x, p.y, now, false, false);
        if (event.gesture == ORB_NAV_GESTURE_ROTARY) {
            s->system_suppress_click = true;
            if (event.type == ORB_NAV_EVENT_BEGIN) {
                /* The crown has one owner. Keep sampling its raw state while
                 * excluding native pointer deltas and the original click. */
                lv_indev_reset(s->system_input, NULL);
                lv_indev_wait_release(s->system_input);
                system_rotary_changed(s, nav->rotary.owner.value, 0);
            }
        }
    }
    orb_nav_tick(nav, now);
    if (!nav->touch.down && !nav->rotary.flinging) {
        s->system_input = NULL;
        lv_timer_pause(timer);
    }
}

static void system_gesture(lv_event_t *event)
{
    orb_shell_t *s = lv_event_get_user_data(event);
    if (!s->system_display_active) return;
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    lv_point_t p; lv_indev_get_point(input, &p);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        s->press_start = p;
        s->system_suppress_click = false;
        orb_nav_touch(&s->system_nav, ORB_NAV_TOUCH_CANCEL, 0, 0, lv_tick_get(), false, false);
        s->system_input = NULL;
        if (s->system_nav_timer && lv_obj_is_visible(system_reader(s))) {
            lv_obj_update_layout(system_reader(s));
            int top = lv_obj_get_scroll_y(system_reader(s));
            int maximum = LV_MAX(0, top + lv_obj_get_scroll_bottom(system_reader(s)));
            if (maximum > 0) {
                orb_nav_rotary_owner_t owner = {.owner = 1, .ctx = s,
                    .change = system_rotary_changed, .minimum = 0, .maximum = maximum,
                    .value = top, .detent = 72, .units_per_turn = 900};
                orb_nav_rotary_bind(&s->system_nav, &owner);
                orb_nav_touch(&s->system_nav, ORB_NAV_TOUCH_DOWN, p.x, p.y, lv_tick_get(), false, top <= 0);
                s->system_input = input;
                lv_timer_resume(s->system_nav_timer);
            }
        }
    } else {
        int dx = p.x - s->press_start.x, dy = p.y - s->press_start.y;
        /* Release dispatch precedes CLICKED even when the navigation timer
         * has not run. Movement stays cancelled after returning to the row. */
        if (dx * dx + dy * dy >= 10 * 10) s->system_suppress_click = true;
    }
    if (code == LV_EVENT_GESTURE &&
               hypotf(s->press_start.x - 233, s->press_start.y - 233) >= 193 &&
               s->press_start.x < 233 && abs(s->press_start.x - 233) > abs(s->press_start.y - 233) &&
               lv_indev_get_gesture_dir(input) == LV_DIR_RIGHT &&
               (s->menu != MENU_CLOSED || updates_view(s->view))) {
        lv_indev_wait_release(input);
        /* Back has the same authority and restoration path as BOOT, including
         * an automatically presented OTA offer/progress/error with no menu. */
        orb_shell_back(s);
    }
}

static void deleted(lv_event_t *event)
{
    orb_shell_t *s = lv_event_get_user_data(event);
    if (s->fonts_pending) lv_async_call_cancel(fonts_apply, s);
    contact_target(s, NULL);
    if (s->contact_input)
        lv_indev_remove_event_cb_with_user_data(s->contact_input, contact_input_event, s);
    if (s->system_nav_timer) lv_timer_delete(s->system_nav_timer);
    orb_app_host_destroy(s->alarm_host);
    orb_app_host_destroy(s->mac_host);
    orb_app_host_destroy(s->ota_host);
    orb_app_host_destroy(s->boot_host);
    orb_sound_overlay_destroy(&s->sound_overlay);
    orb_shell_calm_destroy(s->calm);
    close_button_sheet(s);
    if (s->app_overlay && !s->app_was_paused) orb_apps_request_pause(false);
    if (s->mic) lv_obj_delete(s->mic);
    free(s);
}

orb_shell_t *orb_shell_create(lv_obj_t *parent, orb_shell_action_cb_t cb, void *ctx)
{
    /* Status/UI bookkeeping is not DMA/ISR data. Keep it in PSRAM like the
     * workspace/open-cache owner instead of generic malloc's small-block heap. */
#ifdef ESP_PLATFORM
    orb_shell_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    orb_shell_t *s = malloc(sizeof(*s));
#endif
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    orb_nav_init(&s->system_nav, 0);
    orb_nav_register(&s->system_nav, (orb_nav_place_t){.id = 1, .role = ORB_NAV_ROLE_SCREEN, .scrollable = true});
    orb_nav_set_current(&s->system_nav, 1);
    s->system_display_active = true;
    orb_sound_overlay_init(&s->sound_overlay);
    s->action = cb;
    s->ctx = ctx;
    s->snapshot.battery_pct = -1;
    s->snapshot.rssi = -127;
    if (!parent) {
        /* The screen itself keeps LVGL's light default background. Whenever
         * a transition moves the root off it, that bare screen showed as a
         * white disc. */
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    }
    s->root = lv_obj_create(parent ? parent : lv_screen_active());
    lv_obj_remove_style_all(s->root);
    lv_obj_set_size(s->root, ORB_SCREEN_D, ORB_SCREEN_D);
    lv_obj_set_pos(s->root, 0, 0);
    lv_obj_set_scrollable(s->root, false);
    lv_obj_set_style_bg_color(s->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s->root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s->root, ORB_LABEL, 0);
    s->system = lv_obj_create(s->root);
    lv_obj_remove_style_all(s->system);
    lv_obj_set_size(s->system, ORB_SCREEN_D, ORB_SCREEN_D);
    lv_obj_set_scrollable(s->system, false);
    lv_obj_set_gesture_bubble(s->system, false);
    lv_obj_add_event_cb(s->system, system_gesture, LV_EVENT_PRESSED, s);
    lv_obj_add_event_cb(s->system, system_gesture, LV_EVENT_PRESSING, s);
    lv_obj_add_event_cb(s->system, system_gesture, LV_EVENT_RELEASED, s);
    lv_obj_add_event_cb(s->system, system_gesture, LV_EVENT_PRESS_LOST, s);
    lv_obj_add_event_cb(s->system, system_gesture, LV_EVENT_GESTURE, s);
    orb_system_scaffold_create(&s->scaffold, s->system, clicked, s);
    s->scaffold.forget = button(s, "Forget Wi-Fi", 233, 239, 286);
    s->scaffold.unpair = button(s, "Unpair Mac", 233, 335, 286);
    s->scaffold.cancel = button(s, "Cancel", 233, 405, 250);
    lv_obj_add_event_cb(s->root, deleted, LV_EVENT_DELETE, s);
    lv_obj_add_event_cb(s->root, contact_event, LV_EVENT_PRESSED, s);
    lv_obj_add_event_cb(s->root, contact_event, LV_EVENT_PRESSING, s);
    lv_obj_add_event_cb(s->root, contact_event, LV_EVENT_RELEASED, s);
    s->system_nav_timer = lv_timer_create(system_nav_tick, 16, s);
    if (s->system_nav_timer) lv_timer_pause(s->system_nav_timer);
    s->calm = orb_shell_calm_create(s, s->root);
    if (!s->calm) {
        lv_obj_delete(s->root);
        return NULL;
    }
    render(s);
    lv_subject_add_observer_obj(orb_fonts_subject(), fonts_changed, s->root, s);
    return s;
}

/* Only the fields drawn by this view can require text/layout work. Status
 * widgets already ignore unchanged visible state (e.g. RSSI within one bar).
 * Keeping unrelated data out of this path preserves glyph and glass caches. */
static bool content_changed(view_t view, const orb_shell_snapshot_t *a, const orb_shell_snapshot_t *b)
{
#define CHANGED(field) (strcmp(a->field, b->field) != 0)
    switch (view) {
    case VIEW_SETUP: return a->net != b->net || CHANGED(service_name) || CHANGED(pop);
    case VIEW_PAIR: return CHANGED(pairing_code) || CHANGED(hub_name);
    case VIEW_HOME: return CHANGED(clock_time) || CHANGED(hub_name) ||
                           (!b->clock_time[0] && CHANGED(device_name));
    case VIEW_UPDATES: return online(a) != online(b) || CHANGED(firmware_version) || CHANGED(update_check_result) ||
                              a->update_check_revision != b->update_check_revision;
    case VIEW_CONNECTION: return a->net != b->net || a->link != b->link || CHANGED(ssid) || CHANGED(hub_name);
    case VIEW_OFFER:
        return CHANGED(version) || CHANGED(notes) ||
               ((a->battery_pct >= 0 && a->battery_pct < 30 && !a->charging) !=
                (b->battery_pct >= 0 && b->battery_pct < 30 && !b->charging));
    case VIEW_DOWNLOAD:
    case VIEW_VERIFY:
    case VIEW_READY: return CHANGED(version);
    case VIEW_FAILED: return CHANGED(error);
    case VIEW_SERVICE: return CHANGED(service_error);
    case VIEW_IDENTIFY: return CHANGED(device_name) || CHANGED(device_id);
    default: return false;
    }
#undef CHANGED
}

void orb_shell_update(orb_shell_t *s, const orb_shell_snapshot_t *snapshot)
{
    if (!s || !snapshot) return;
    orb_shell_snapshot_t previous = s->snapshot;
    s->snapshot = *snapshot;
    /* Snapshots cross task boundaries; never depend on a producer's terminator. */
#define TERMINATE(field) s->snapshot.field[sizeof(s->snapshot.field) - 1] = '\0'
    TERMINATE(service_name); TERMINATE(pop); TERMINATE(ssid); TERMINATE(pairing_code);
    TERMINATE(hub_name); TERMINATE(device_name); TERMINATE(device_id); TERMINATE(clock_time);
    TERMINATE(clock_date); TERMINATE(firmware_version);
    TERMINATE(version); TERMINATE(notes); TERMINATE(error); TERMINATE(service_error);
    TERMINATE(update_check_result);
#undef TERMINATE
    button_sheet_status(s);
    orb_shell_calm_status(s->calm, &s->snapshot);
    if (s->snapshot.service_error[0] && strcmp(previous.service_error, s->snapshot.service_error) != 0) {
        s->menu = MENU_CLOSED;
    }
    if (s->snapshot.update_check_revision != previous.update_check_revision ||
        s->snapshot.ota != ORB_SHELL_OTA_IDLE || !online(&s->snapshot) ||
        s->snapshot.service_error[0]) s->check_pending = false;
    if (previous.ota != s->snapshot.ota || previous.update_check_revision != s->snapshot.update_check_revision ||
        !online(&s->snapshot)) s->ota_wait.waiting = false;
    /* Leaving progress never cancels flash work. A new transfer starts in its
     * progress view; later verify/ready snapshots do not steal browsing focus. */
    if (previous.ota != s->snapshot.ota && !(busy(&previous) && busy(&s->snapshot)))
        s->ota_background = false;
    view_t next = choose_view(s);
    if (next == VIEW_HOME && s->view == VIEW_HOME && s->workspace) {
        orb_workspace_update(s->workspace, &s->snapshot);
        return;
    }
    if (next != s->view || content_changed(next, &previous, &s->snapshot)) {
        render(s);
    } else {
        if (next != VIEW_PAIR && next != VIEW_SETUP && next != VIEW_DOWNLOAD &&
            next != VIEW_VERIFY && next != VIEW_READY && next != VIEW_FAILED &&
            !updates_view(next) && !mac_view(next)) orb_system_scaffold_status(&s->scaffold, &s->snapshot);
        if ((previous.battery_pct >= 0) != (s->snapshot.battery_pct >= 0) ||
            (s->snapshot.battery_pct < 0 && previous.usb != s->snapshot.usb)) orb_system_scaffold_layout_status(&s->scaffold, &s->snapshot);
        if (next == VIEW_DOWNLOAD && LV_CLAMP(0, previous.progress, 100) !=
            LV_CLAMP(0, s->snapshot.progress, 100)) {
            orb_ota_screen_progress(orb_app_host_current(s->ota_host), s->snapshot.progress);
        }
    }
}

void orb_shell_destroy(orb_shell_t *s)
{
    if (s) lv_obj_delete(s->root);
}

lv_obj_t *orb_shell_root(const orb_shell_t *s)
{
    return s ? s->root : NULL;
}

orb_shell_calm_t *orb_shell_calm_context(const orb_shell_t *s)
{
    return s ? s->calm : NULL;
}

bool orb_shell_face_snapshot(const orb_shell_t *s, orb_faces_snapshot_t *snapshot)
{
    return s && orb_workspace_face_snapshot(s->workspace, snapshot);
}

bool orb_shell_notice_allowed(const orb_shell_t *s)
{
    return s && s->view == VIEW_HOME && s->workspace && !alarm_view(s) &&
        !s->button_sheet && !s->app_overlay && !orb_apps_active() && !s->talk_held && !orb_sound_overlay_has_speech(&s->sound_overlay);
}

void orb_shell_environment(orb_shell_t *s, float x, float y, float z, bool always_on)
{
    if (!s) return;
    if (s->workspace) orb_workspace_environment(s->workspace, x, y, z, always_on);
    s->system_always_on = always_on;
    orb_kit_motion_set_active(s->system, s->system_display_active && !always_on);
    orb_sound_overlay_set_motion_active(&s->sound_overlay, s->system_display_active && !always_on);
}

bool orb_shell_time_alert_active(const orb_shell_t *s)
{
    return s && alarm_view(s);
}

void orb_shell_display_active(orb_shell_t *s, bool active)
{
    if (s) {
        s->system_display_active = active;
        orb_kit_motion_set_active(s->system, active && !s->system_always_on);
        orb_sound_overlay_set_motion_active(&s->sound_overlay, active && !s->system_always_on);
        if (!active) system_nav_stop(s);
    }
    if (s && s->workspace) orb_workspace_display_active(s->workspace, active && !alarm_view(s));
}

static void close_time_presentation(orb_shell_t *s)
{
    orb_app_host_destroy(s->alarm_host); s->alarm_host = NULL;
    orb_shell_calm_alert(s->calm, false);
    if (!s->alarm_was_paused && orb_apps_active()) orb_apps_request_pause(false);
    s->alarm_was_paused = true;
    orb_power_snapshot_t power = orb_shell_power_snapshot(s);
    orb_workspace_display_active(s->workspace,
        !power.revision || (power.state == ORB_POWER_ACTIVE && !power.redact));
}

void orb_shell_poll(orb_shell_t *s)
{
    if (!s) return;
    /* Crown/system capture can consume RELEASED. Raw input still reaches its
     * endpoint, so the next physical contact must start a fresh tap budget. */
    if (s->contact_input && lv_indev_get_state(s->contact_input) == LV_INDEV_STATE_RELEASED)
        s->contact_down = false;
    /* Apply privacy and forced silence before a new model event can paint or
     * play its cue. The final poll restores overlay order after mic/sheets. */
    orb_shell_calm_poll(s->calm);
    orb_time_snapshot_t alarm;
    orb_time_snapshot(orb_time_now_us(), &alarm);
    bool needs_attention = alarm.alarm_ringing || alarm.reminder_due ||
                           alarm.timer_state == ORB_TIME_FINISHED;
    if (needs_attention) {
        if (!alarm_view(s) && !s->alarm_presentation_dismissed) {
            orb_shell_calm_alert(s->calm, true);
            close_button_sheet(s);
            if (s->speech_stop) s->speech_stop(s->speech_stop_ctx);
            if (!s->alarm_seen) {
                s->alarm_was_paused = !orb_apps_active() || orb_apps_paused();
                if (orb_apps_active()) orb_apps_request_pause(true);
            }
            /* The game owns a separate LVGL screen. An independent top-layer
             * Time view interrupts every surface without changing app admission
             * or losing the owner's navigation beneath it. */
            if (!orb_time_host_present(&s->alarm_host, lv_display_get_layer_top(lv_obj_get_display(s->root))))
                orb_shell_calm_alert(s->calm, false);
            else orb_workspace_display_active(s->workspace, false);
        }
        orb_app_host_poll(s->alarm_host);
    } else if (s->alarm_seen) {
        close_time_presentation(s);
        s->alarm_presentation_dismissed = false;
    }
    s->alarm_seen = needs_attention;
    if (!s->onboarding_seen && !needs_attention && s->view == VIEW_HOME && orb_system_onboarding_needed()) {
        s->onboarding_seen = true;
        orb_workspace_navigate(s->workspace, ORB_SHELL_ONBOARDING);
    }
    /* HIG §8: BOOT also works while a mandatory system surface is visible.
     * The recording indicator belongs to the display's system overlay;
     * OTA/setup keeps precedence and can never conceal microphone capture. */
    orb_voice_state_t voice;
    bool have_voice = orb_model_get_voice(&voice);
    if (s->app_overlay && !orb_apps_active()) close_app_overlay(s);
    bool active = s->talk_held || (have_voice &&
                  (voice.phase == ORB_VOICE_STARTING || voice.phase == ORB_VOICE_RECORDING));
    bool workspace_mic = s->view == VIEW_HOME && s->workspace &&
                         orb_workspace_surface(s->workspace) == ORB_SHELL_VOICE;
    if (active && !s->mic) {
        /* Games own a separate active screen. The system indicator and power
         * sheets live on the display's top layer, above every app screen. */
        s->mic = lv_label_create(lv_display_get_layer_top(lv_obj_get_display(s->root)));
        lv_label_set_text(s->mic, ORB_ICON_MIC " Listening");
        lv_obj_set_style_text_color(s->mic, ORB_RED, 0);
        lv_obj_set_style_bg_color(s->mic, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s->mic, LV_OPA_COVER, 0);
        lv_obj_set_style_text_align(s->mic, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_clickable(s->mic, false);
    }
    if (s->mic) {
        visible(s->mic, active && !workspace_mic);
        if (active) {
            text(s->mic, s->talk_held && !s->talk_synthetic && have_voice && voice.phase == ORB_VOICE_ERROR ?
                 ORB_ICON_MIC_OFF " No voice" : ORB_ICON_MIC " Listening");
            orb_text_apply(s->mic, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED, 0);
            if (s->talk_from_alert) {
                /* HIG §§1/7: Listening is a temporary presentation, not text
                 * laid over the alert's rim title, artwork or actions. Cover
                 * and intercept that surface until the held contact ends. */
                int height = lv_font_get_line_height(lv_obj_get_style_text_font(s->mic, 0));
                lv_obj_set_size(s->mic, 466, 466);
                lv_obj_set_pos(s->mic, 0, 0);
                lv_obj_set_style_pad_top(s->mic, (466 - height) / 2, 0);
                lv_obj_set_clickable(s->mic, true);
            } else {
                lv_obj_set_size(s->mic, 184, 82);
                lv_obj_set_pos(s->mic, 141, 54);
                lv_obj_set_style_pad_top(s->mic, 0, 0);
                lv_obj_set_clickable(s->mic, false);
            }
            lv_obj_move_foreground(s->mic);
        }
    }
    /* Keep receipts and phase changes current even during a system screen.
     * The workspace remains hidden until that system task is complete. */
    if (s->workspace) orb_workspace_poll(s->workspace);
    /* Keep delivery/error receipts visible until explicitly dismissed. A rim
     * Back follows the same restoration path as the physical Back button. */
    if (s->app_overlay && !s->talk_held &&
        orb_workspace_surface(s->workspace) != s->app_overlay_surface) {
        if (s->app_overlay_surface == ORB_SHELL_VOICE) orb_model_voice_request(ORB_VOICE_CANCEL, NULL, NULL);
        close_app_overlay(s);
    }
    orb_shell_calm_poll(s->calm);
    /* The alert owns its own opaque surface above privacy. Keep the retained
     * task covered while making alert actions and BOOT Listening visible. */
    if (alarm_view(s)) lv_obj_move_foreground(orb_screen_root(alarm_view(s)));
    orb_power_snapshot_t power = orb_shell_power_snapshot(s);
    if (s->button_sheet) {
        /* Retain Resume through lock, but never raise app controls over the
         * public privacy Face. Authorized wake restores this same sheet. */
        visible(s->button_sheet, !power.revision || (power.state == ORB_POWER_ACTIVE && !power.redact));
        if (lv_obj_is_visible(s->button_sheet)) lv_obj_move_foreground(s->button_sheet);
    }
    if (s->mic && lv_obj_is_visible(s->mic)) lv_obj_move_foreground(s->mic);
    bool visible = !needs_attention && (!power.revision || (power.state == ORB_POWER_ACTIVE && !power.redact));
    orb_sound_overlay_set_control_visible(&s->sound_overlay, volume_control_visible(s));
    orb_sound_overlay_poll(&s->sound_overlay, lv_obj_get_display(s->root), visible);
}

void orb_shell_show_volume(orb_shell_t *s)
{
    if (s && !s->alarm_seen) {
        orb_sound_overlay_set_control_visible(&s->sound_overlay, volume_control_visible(s));
        orb_sound_overlay_volume(&s->sound_overlay, lv_obj_get_display(s->root));
    }
}

void orb_shell_media_volume(orb_shell_t *s, unsigned volume)
{
    if (s && !s->alarm_seen) {
        orb_sound_overlay_set_control_visible(&s->sound_overlay, volume_control_visible(s));
        orb_sound_overlay_media(&s->sound_overlay, lv_obj_get_display(s->root), volume);
    }
}

void orb_shell_spoken_reply(orb_shell_t *s, bool active, bool buffering, const char *caption)
{
    if (!s) return;
    orb_agent_t agent;
    bool known = active && orb_workspace_speech_agent(s->workspace, &agent);
    orb_sound_overlay_speech(&s->sound_overlay, lv_obj_get_display(s->root), active && !s->alarm_seen,
                             buffering, caption, known ? (int)agent : -1);
}

void orb_shell_fixture_notification_age(orb_shell_t *s, const char *id, unsigned age_ms)
{
    if (s) orb_workspace_fixture_notification_age(s->workspace, id, age_ms);
}

static bool resume_workspace(orb_shell_t *s)
{
    if (!s) return false;
    bool menu = s->view == VIEW_CONNECTION || s->view == VIEW_MAINTENANCE || s->view == VIEW_FORGET ||
                s->view == VIEW_UNPAIR || s->view == VIEW_UPDATES;
    if (s->snapshot.ota != ORB_SHELL_OTA_IDLE && !s->snapshot.service_error[0] && !s->snapshot.identify) {
        s->ota_background = true;
        s->menu = MENU_CLOSED;
        render(s);
    } else if (menu && !s->snapshot.service_error[0] && !s->snapshot.identify) {
        s->menu = MENU_CLOSED;
        render(s);
    }
    return s->view == VIEW_HOME && s->workspace;
}

void orb_shell_navigate(orb_shell_t *s, orb_shell_surface_t surface)
{
    if (!s || alarm_view(s)) return;
    if (surface == ORB_SHELL_UPDATES && s->snapshot.ota != ORB_SHELL_OTA_IDLE) {
        s->ota_background = false;
        render(s);
        return;
    }
    if (resume_workspace(s)) orb_workspace_navigate(s->workspace, surface);
}

orb_shell_surface_t orb_shell_surface(const orb_shell_t *s)
{
    if (s && alarm_view(s)) return ORB_SHELL_TIME;
    if (s && (s->view == VIEW_UPDATES || s->view == VIEW_DOWNLOAD ||
              s->view == VIEW_VERIFY || s->view == VIEW_READY || s->view == VIEW_FAILED)) return ORB_SHELL_UPDATES;
    return s && s->workspace ? orb_workspace_surface(s->workspace) : ORB_SHELL_FACE;
}

orb_shell_surface_t orb_shell_nav_current(const orb_shell_t *s)
{
    return s && s->workspace ? orb_workspace_nav_current(s->workspace) : ORB_SHELL_FACE;
}

void orb_shell_open_metrics(const orb_shell_t *s, orb_shell_open_metrics_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (s && s->workspace) orb_workspace_open_metrics(s->workspace, out);
}

void orb_shell_back(orb_shell_t *s)
{
    if (s && orb_shell_calm_dismiss_notice(s->calm)) return;
    if (s && alarm_view(s)) {
        /* Back dismisses only this presentation. Time's service retains its
         * ringing latch until the owner chooses Stop or Snooze in Time. */
        s->alarm_presentation_dismissed = true;
        close_time_presentation(s);
        orb_sound_play(ORB_SOUND_CLOSE);
        return;
    }
    if (s && s->button_sheet) { dismiss_button_sheet(s); return; }
    if (s && s->app_overlay) {
        if (s->app_overlay_surface == ORB_SHELL_VOICE) orb_model_voice_request(ORB_VOICE_CANCEL, NULL, NULL);
        close_app_overlay(s);
        return;
    }
    if (orb_apps_active() && orb_shell_surface(s) != ORB_SHELL_TIME) {
        if (orb_games_request_back()) return;
        orb_apps_request_exit();
        return; /* The app's parent is the still-live Play list. */
    }
    if (s && s->menu != MENU_CLOSED) {
        s->menu = s->menu == MENU_CONNECTION || s->menu == MENU_UPDATES ? MENU_CLOSED :
                  s->menu == MENU_MAINTENANCE ? MENU_CONNECTION : MENU_MAINTENANCE;
        render(s);
        return;
    }
    if (resume_workspace(s)) orb_workspace_back(s->workspace);
}

bool orb_shell_nav_bench(orb_shell_t *s, const char *gesture, uint32_t elapsed_ms, bool finish)
{
    if (!s || !s->workspace) {
        orb_nav_admission_fail(ORB_NAV_ADMISSION_WORKSPACE_HOLD);
        return false;
    }
    if (finish) {
        bool restored = orb_workspace_nav_bench(s->workspace, gesture, 0, true);
        if (s->nav_benchmark) {
            s->nav_benchmark = false;
            s->menu = s->nav_saved_menu;
            render(s);
            lv_obj_scroll_to_y(s->scaffold.reading, s->nav_saved_reading, LV_ANIM_OFF);
        }
        return restored;
    }
    if (!elapsed_ms && !s->nav_benchmark) {
        /* Optional service menus can be suspended and restored. Setup,
         * installation, identification and service failures keep precedence. */
        if (!gesture || (strcmp(gesture, "slide-left") && strcmp(gesture, "slide-right") &&
            strcmp(gesture, "stack-up") && strcmp(gesture, "shade-down") && strcmp(gesture, "back") &&
            strcmp(gesture, "iris") && strcmp(gesture, "rotary"))) return false;
        menu_t menu = s->menu;
        s->menu = MENU_CLOSED;
        bool allowed = choose_view(s) == VIEW_HOME;
        s->menu = menu;
        if (!allowed) {
            orb_nav_admission_fail(ORB_NAV_ADMISSION_WORKSPACE_HOLD);
            return false;
        }
        s->nav_saved_menu = menu;
        s->nav_saved_reading = lv_obj_get_scroll_y(s->scaffold.reading);
        s->nav_benchmark = true;
        if (s->view != VIEW_HOME) { s->menu = MENU_CLOSED; render(s); }
        if (!orb_workspace_nav_bench(s->workspace, gesture, 0, false)) {
            orb_shell_nav_bench(s, gesture, 0, true);
            return false;
        }
        return true;
    }
    return s->nav_benchmark && s->view == VIEW_HOME &&
           orb_workspace_nav_bench(s->workspace, gesture, elapsed_ms, false);
}

void orb_shell_select_session(orb_shell_t *s, const char *id)
{
    if (s && s->view == VIEW_HOME && s->workspace) orb_workspace_session(s->workspace, id);
}

void orb_shell_select_card(orb_shell_t *s, const char *id)
{
    if (s && s->view == VIEW_HOME && s->workspace) orb_workspace_card(s->workspace, id);
}

void orb_shell_power(orb_shell_t *s)
{
    if (!s) return;
    if (s->app_overlay) {
        if (s->app_overlay_surface == ORB_SHELL_VOICE) orb_model_voice_request(ORB_VOICE_CANCEL, NULL, NULL);
        close_app_overlay(s);
    }
    if (orb_apps_active() && orb_shell_surface(s) != ORB_SHELL_TIME) {
        if (orb_games_request_pause()) {
            dismiss_button_sheet(s);
        } else button_sheet(s, true);
    } else dismiss_button_sheet(s);
    /* Preserve the place and pending work under the same retained privacy
     * surface used by idle AOD. A game is paused before it is covered. */
    orb_shell_calm_lock(s->calm);
}

void orb_shell_talk(orb_shell_t *s, bool held)
{
    if (!s) return;
    s->talk_held = held;
    if (s->workspace) orb_workspace_talk(s->workspace, held);
    orb_shell_poll(s);
}

void orb_shell_set_speech_stop(orb_shell_t *s, orb_shell_speech_stop_cb_t stop, void *ctx)
{
    if (!s) return;
    s->speech_stop = stop;
    s->speech_stop_ctx = ctx;
    s->sound_overlay.stop = stop;
    s->sound_overlay.stop_ctx = ctx;
}

bool orb_shell_double_tap(orb_shell_t *s)
{
    if (!s) return false;
    /* Deliberate allowlist, independent of the primary glass button. Reusing
     * its CLICKED event would bypass synthetic-input guards on cards/Home/Send.
     * A case tap can acknowledge a local timer or cancel speech. Ringing
     * alarms require their visible Stop or Snooze action. */
    orb_time_snapshot_t time;
    orb_time_snapshot(orb_time_now_us(), &time);
    bool timer = time.timer_state == ORB_TIME_FINISHED;
    if (timer && !time.alarm_ringing) {
        orb_time_dismiss(true, false);
        return true;
    }
    if (time.alarm_ringing) return false;
    if (s->speech_stop && s->speech_stop(s->speech_stop_ctx)) return true;
    if (orb_shell_calm_dismiss_notice(s->calm)) return true;
    return s->view == VIEW_HOME && orb_workspace_dismiss_banner(s->workspace);
}

void orb_shell_set_silent(orb_shell_t *s, bool silent)
{
    if (s && s->workspace) orb_workspace_set_silent(s->workspace, silent);
}

bool orb_shell_twist(orb_shell_t *s, int detents)
{
    if (!s || !s->system_display_active || orb_shell_calm_notice_visible(s->calm) || s->button_sheet || s->talk_held ||
        orb_sound_overlay_has_speech(&s->sound_overlay)) return false;
    /* Main 2.0: a twist belongs to visible content. The volume HUD is output
     * only, and a game or ringing alarm never acquires a hidden volume owner. */
    if (alarm_view(s) || (orb_apps_active() && !s->app_overlay &&
        orb_shell_surface(s) != ORB_SHELL_TIME)) return false;
    if (s->view != VIEW_HOME) {
        if (!system_reader(s) || !lv_obj_is_visible(system_reader(s))) return false;
        int top = lv_obj_get_scroll_y(system_reader(s));
        int limit = LV_MAX(0, top + lv_obj_get_scroll_bottom(system_reader(s)));
        int next = LV_CLAMP(0, top + detents * 72, limit);
        if (next != top) orb_sound_play(ORB_SOUND_DETENT);
        lv_obj_scroll_to_y(system_reader(s), next, LV_ANIM_OFF);
        return true;
    }
    return orb_workspace_twist(s->workspace, detents);
}

void orb_shell_button(orb_shell_t *s, const orb_button_event_t *event)
{
    if (!s || !event) return;
    if (orb_shell_calm_button(s->calm, event)) return;
    if (event->button == ORB_BUTTON_PWR) {
        if (s->app_overlay && event->gesture != ORB_BUTTON_DOWN &&
            event->gesture != ORB_BUTTON_UP && event->gesture != ORB_BUTTON_DOUBLE) {
            if (s->app_overlay_surface == ORB_SHELL_VOICE) orb_model_voice_request(ORB_VOICE_CANCEL, NULL, NULL);
            close_app_overlay(s);
        }
        if (event->gesture == ORB_BUTTON_SHORT) orb_shell_power(s);
        else if (event->gesture == ORB_BUTTON_DOUBLE) {
            if (s->app_overlay && s->app_overlay_surface == ORB_SHELL_VOICE) {
                orb_model_voice_request(ORB_VOICE_CANCEL, NULL, NULL);
                s->talk_held = false;
                orb_workspace_talk(s->workspace, false);
            }
            bool sheet_owned_pause = s->button_sheet && s->sheet.pauses_runtime && !s->sheet.runtime_was_paused;
            dismiss_button_sheet(s);
            if (orb_apps_active() && orb_shell_surface(s) != ORB_SHELL_TIME && s->view == VIEW_HOME) {
                if (!s->app_overlay) {
                    s->app_parent = lv_obj_get_parent(s->root);
                    s->app_return = orb_shell_surface(s);
                    s->app_was_paused = orb_apps_paused() && !sheet_owned_pause;
                    s->app_overlay = true;
                }
                s->app_overlay_surface = ORB_SHELL_CONTROL_CENTER;
                orb_apps_request_pause(true);
                lv_obj_set_parent(s->root, lv_display_get_layer_top(lv_obj_get_display(s->root)));
            }
            orb_shell_navigate(s, ORB_SHELL_CONTROL_CENTER);
        } else if (event->gesture == ORB_BUTTON_HOLD) button_sheet(s, false);
    } else if (event->button == ORB_BUTTON_BOOT) {
        if (event->gesture == ORB_BUTTON_SHORT || event->gesture == ORB_BUTTON_DOUBLE) {
            if (!s->button_sheet && !s->app_overlay && !orb_apps_active() && s->view == VIEW_HOME &&
                orb_shell_surface(s) == ORB_SHELL_FACE && !orb_workspace_face_local(s->workspace))
                orb_shell_navigate(s, ORB_SHELL_APPS);
            else orb_shell_back(s);
        }
        else if (event->gesture == ORB_BUTTON_HOLD && !s->talk_held) {
            bool sheet_owned_pause = s->button_sheet && s->sheet.pauses_runtime && !s->sheet.runtime_was_paused;
            s->talk_from_alert = alarm_view(s) != NULL;
            if (alarm_view(s)) {
                /* BOOT hold deliberately enters voice. Dismiss only the alert
                 * presentation, retaining its service latch and the task lock.
                 * Carry its pause ownership into the ordinary voice overlay. */
                sheet_owned_pause |= !s->alarm_was_paused;
                orb_shell_back(s);
            }
            dismiss_button_sheet(s);
            s->talk_synthetic = event->synthetic;
            orb_workspace_talk_origin(s->workspace, event->synthetic);
            if (!event->synthetic && orb_apps_active() && orb_shell_surface(s) != ORB_SHELL_TIME) {
                /* App screens are independent for damage isolation. Lift the
                 * existing shell while recording/reviewing so the transcript
                 * and physical Send remain visible; resume the same app. */
                if (!s->app_overlay) {
                    s->app_parent = lv_obj_get_parent(s->root);
                    s->app_return = orb_shell_surface(s);
                    s->app_was_paused = orb_apps_paused() && !sheet_owned_pause;
                    s->app_overlay = true;
                }
                s->app_overlay_surface = ORB_SHELL_VOICE;
                orb_apps_request_pause(true);
                lv_obj_set_parent(s->root, lv_display_get_layer_top(lv_obj_get_display(s->root)));
                orb_shell_navigate(s, ORB_SHELL_VOICE);
            }
            orb_shell_talk(s, true);
            /* A hold starts capture on every screen. Release uses the existing
             * stop/transcribe/review flow, never implicit confirmation. */
            if (!event->synthetic) orb_model_voice_request(ORB_VOICE_START, NULL, NULL);
        } else if (event->gesture == ORB_BUTTON_UP && s->talk_held) {
            if (!s->talk_synthetic) orb_model_voice_request(ORB_VOICE_STOP, NULL, NULL);
            orb_shell_talk(s, false);
            s->talk_synthetic = false;
            s->talk_from_alert = false;
        }
    }
}

int orb_shell_brightness(const orb_shell_t *s)
{
    return s && s->workspace ? orb_workspace_brightness(s->workspace) : 70;
}
