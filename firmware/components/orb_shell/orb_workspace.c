/* The round shell: one live surface and bounded PSRAM launch snapshots.
 * HIG §§1/2: content is plain black; only navigation and actions use glass.
 * Model copies live in PSRAM, not the scarce internal/DMA heap. */
#include "orb_workspace.h"
#include "orb_shell_power.h"
#include "orb_faces_host.h"
#include "orb_delight_motion.h"
#include "orb_feedback.h"
#include "orb_sound.h"
#include "orb_apps.h"
#include "orb_play_host.h"
#include "orb_agents_host.h"
#include "orb_voice_host.h"
#include "orb_notification_history.h"
#include "orb_notifications_host.h"
#include "orb_receipt.h"
#include "orb_games.h"
#include "orb_home_host.h"
#include "orb_time_host.h"
#include "orb_shell_services.h"
#include "orb_nav.h"
#include "orb_nav_compositor.h"
#include "orb_nav_admission.h"
/* Stream F: bounded offline Time hooks; keep navigation ownership in shell. */
#include "orb_time.h"
#include "orb_time_view.h"
#include "orb_system_state.h"
#include "orb_system_copy.h"
#include "orb_stack.h"
#include "orb_agents_copy.h"
#include "orb_place_view.h"
#include "system/charging_screen.h"
#include "control_screen.h"
#include "apps_screen.h"
#include "stack_screen.h"
#include "system/system_views.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_ui.h"
#include "orb_kit_shell_controls.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_page.h"
#include "orb_route_context.h"
#include "orb_settings_host.h"
#include "orb_app_settings_units.h"
#include "orb_app_settings_root.h"
#include "orb_app_settings_sound.h"
#include "orb_app_settings_display.h"
#include "orb_app_settings_accessibility.h"
#include "orb_app_settings_battery.h"
#include "orb_app_settings_sources.h"
#include "orb_input.h"
#include "orb_workspace_snapshot.h"
#include "orb_neighbour.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#ifdef ESP_PLATFORM
#include "orb_display.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#else
/* Host display mirrors the no-blur band geometry for navigation captures. */
void orb_display_set_snapshot_transition(bool active);
bool orb_display_capture_snapshot(lv_obj_t *object, lv_draw_buf_t *destination);
#endif

#define SECONDARY ORB_LABEL_LEGACY_SECONDARY
#define EVENT_COUNT ORB_NOTIFICATION_HISTORY_COUNT
#define OPEN_CACHE_COUNT 2
#define OPEN_PIXELS (466u * 466u * 2u)
/* HIG §§4/6: retain a zoom from the launch icon without shrinking the smallest
 * 21px essential label below 20px, even in a captured intermediate frame. */
#define OPEN_MIN_SCALE 244
#ifdef ESP_PLATFORM
#if CONFIG_ORB_OPEN_LATENCY_FIXES
#define OPEN_FIXES 1
#else
#define OPEN_FIXES 0
#endif
#else
#define OPEN_FIXES ORB_OPEN_LATENCY_FIXES
#endif

typedef struct {
    lv_image_dsc_t image;
    orb_shell_snapshot_t status;
    uint64_t revision;
    orb_shell_surface_t surface;
    uint32_t used;
    bool valid, reduced_transparency, reduced_motion, contrast;
} open_cache_t;

enum {
    ACT_BACK = 100, ACT_PAGE_NEXT = 103, ACT_UNDO_CLEAR = 105, ACT_SESSION = 120, ACT_CARD = 160,
    ACT_OPTION = 200, ACT_REPLY = 212, ACT_CLEAR,
    ACT_RECENT = 240, ACT_SILENT = 263,
    ACT_VOICE_STOP = 268, ACT_VOICE_CANCEL, ACT_VOICE_SEND,
    ACT_VOICE_HOLD,
    ACT_IDLE_MORE,
    ACT_IDLE_SESSION = 320, ACT_GAME_EXIT = 380, ACT_HOME_SCENE, ACT_HOME_FIRST, ACT_HOME_SECOND, ACT_HOME_OFF,
    ACT_CONTROL_BRIGHTNESS = 700, ACT_CONTROL_VOLUME,
    ACT_TIME = 480, ACT_NOTICE_OPEN, ACT_NOTICE_REPLY, ACT_SLEEP, ACT_QUIET, ACT_FLASHLIGHT,
    ACT_STACK_SCENES = 489, ACT_FOCUS_NEXT, ACT_FOCUS_PREVIOUS, ACT_FOCUS_OPEN, ACT_APPROVAL_NEXT, ACT_RESULT_RETRY, ACT_RESULT_DONE, ACT_NOTICE_DISMISS, ACT_FOCUS_SELECT = 520,
};

struct orb_workspace {
    lv_obj_t *root, *page, *list;
    orb_shell_snapshot_t status;
    const orb_model_snapshot_t *model;
    const orb_home_snapshot_t *home;
    workspace_snapshots_t snapshots;
    orb_app_host_t *home_host, *home_bench;
    orb_app_host_t *faces_host;
    bool face_launch;
    unsigned face_usage_page;
    orb_time_snapshot_t time;
    orb_app_host_t *time_host, *time_bench;
    orb_app_host_t *stack_host;
    uint32_t time_polled_at, time_nav_dirty;
    bool time_open, time_launched;
    char home_scene_id[33];
    orb_agents_host_t *agents_host, *agents_bench;
    orb_voice_host_t *voice_host;
    orb_voice_flow_t *voice_flow;
    orb_notification_history_t *notifications;
    orb_notifications_host_t *notifications_host;
    orb_idle_page_t *idle;
    orb_session_t selection;
    orb_shell_action_cb_t action;
    orb_workspace_service_cb_t service;
    void *ctx;
    orb_shell_surface_t surface;
    unsigned page_index;
    orb_app_host_t *play_host, *play_bench;
    orb_settings_host_t *settings, *settings_bench;
    orb_system_views_t *system_views;
    char session[ORB_MODEL_ID_MAX + 1], card[ORB_MODEL_CARD_ID_MAX + 1], notice[64];
    bool silent, held, shown, rebuilding, contrast, fixture, stack_scenes, home_fresh_entry;
    bool render_pending, navigation_pending, launch_pending, text_notice_pending;
    bool flashlight;
    int flashlight_brightness;
    orb_apps_view_t *apps_view, *apps_bench;
    orb_app_host_t *control_host;
    orb_app_host_t *charging_host;
    bool brightness_pending, control_volume, control_home_loading;
    orb_route_context_t places[ORB_SHELL_ONBOARDING + 1];
    int restore_scroll;
    bool restore_place;
    lv_point_t launch;
    bool suppress_click, curved;
    bool has_selection, selection_external;
    orb_shell_open_metrics_t open;
    uint32_t visited;
    lv_timer_t *open_timer;
    lv_display_t *display;
    lv_obj_t *heading, *preview, *content;
    open_cache_t cache[OPEN_CACHE_COUNT];
    int capture, capture_next_y;
    bool opening, scaffolded, build_due, first_presented, render_retry;
    float open_position, open_velocity;
    uint32_t open_elapsed;
    lv_timer_t *delight_timer;
    lv_obj_t *launch_disc;
    lv_point_t icon_origin;
    orb_shell_surface_t launched_app;
    uint32_t delight_at, clear_at, new_card_at, page_at, control_home_wait_at;
    uint32_t app_revision;
    bool counts_known, all_clear, face_down, always_on, display_active, covered;
    bool reverse_launch, page_motion, arrival;
    orb_delight_spring_t page_spring;
    bool notification_sound_pending;
    lv_obj_t *notification;
    orb_nav_t nav;
    orb_nav_compositor_t *compositor;
    lv_timer_t *nav_timer;
    lv_indev_t *nav_input;
    lv_obj_t *nav_overlay, *compass, *rotary_mark;
    orb_nav_compass_selection_t compass_selection;
    orb_shell_surface_t nav_target, nav_saved, nav_source;
    orb_nav_transition_kind_t nav_kind;
    orb_nav_direction_t nav_direction;
    orb_nav_token_rect_t nav_token;
    uint64_t nav_revision;
    uint32_t nav_at;
    float nav_drag_origin, nav_last_drag;
    float nav_reverse_prefix;
    bool nav_pending, nav_presenting, nav_released, nav_back, nav_applying, nav_commit;
    bool nav_first_presented, nav_stack_reader;
    bool nav_open_preview, nav_settle_capture, nav_settle_failed, nav_apps_warm;
    bool nav_benchmark, nav_reverse;
    bool nav_routing, nav_speculative;
    unsigned nav_to;
    bool nav_live, nav_local_back, nav_fallback, nav_boundary;
    bool nav_local_object;
    float nav_boundary_distance;
    int nav_fallback_x, nav_fallback_y;
    orb_nav_t nav_saved_state;
    orb_nav_t nav_origin_state;
    int nav_source_scroll;
    int nav_live_x, nav_live_y;
    int nav_saved_scroll;
    int nav_scroll_position;
    bool nav_scroll_observed;
    struct {
        unsigned page_index;
        bool has_selection,
            selection_external;
        orb_session_t selection;
        orb_shell_surface_t launched_app;
        bool time_open, time_launched;
        lv_point_t launch, icon_origin;
        char session[ORB_MODEL_ID_MAX + 1], card[ORB_MODEL_CARD_ID_MAX + 1], notice[64];
    } nav_saved_context;
    orb_shell_surface_t nav_cached[ORB_NAV_SNAPSHOT_SLOTS];
    bool nav_apps_preview[ORB_NAV_SNAPSHOT_SLOTS];
};

static orb_screen_instance_t *agents_screen(const orb_workspace_t *w)
{
    return orb_agents_host_current(w->agents_host);
}

static orb_screen_instance_t *faces_screen(const orb_workspace_t *w)
{
    return orb_faces_host_current(w->faces_host);
}

static orb_screen_instance_t *home_screen(const orb_workspace_t *w)
{
    return orb_app_host_current(w->home_host);
}

static orb_screen_instance_t *time_screen(const orb_workspace_t *w)
{
    return orb_app_host_current(w->time_host);
}

/* Departure points record Time's current page, so a page change made just
 * before leaving does not later invalidate cached planes. */
static void workspace_time_save(orb_workspace_t *w)
{
    (void)orb_app_time_page_changed(time_screen(w));
}

static bool workspace_idle_route(orb_nav_id_t surface)
{
    return surface == ORB_SHELL_IDLE_SESSIONS || surface == ORB_SHELL_SESSIONS || surface == ORB_SHELL_DOT;
}

static bool workspace_idle_retained(const orb_workspace_t *w)
{
    /* Face is a canonical root: BOOT opens Apps rather than resurrecting a
     * previous pager, even if compatibility route history still contains it. */
    if (w->surface == ORB_SHELL_FACE || w->surface == ORB_SHELL_OFFLINE) return false;
    if (workspace_idle_route(w->surface)) return true;
    /* A nested session and temporary sheets still return to this exact pager.
     * Release its generation only after that parent leaves navigation history. */
    for (unsigned i = 0; i < w->nav.back_count; i++)
        if (workspace_idle_route(w->nav.back[i].id)) return true;
    return false;
}

static lv_obj_t *prose(orb_workspace_t *w, int y, int h);
static void render(orb_workspace_t *w);
static void render_now(orb_workspace_t *w);
static void animate_page(orb_workspace_t *w);
static void navigate(orb_workspace_t *w, orb_shell_surface_t surface, bool push);
static bool settings_prepare(orb_workspace_t *w, orb_shell_surface_t surface, bool arrival);
static void session_open_token(orb_workspace_t *w, const char *id, const orb_nav_token_t *source);
static void open_tick(lv_timer_t *timer);
static void open_refresh(lv_event_t *e);
static void open_finish_settled(orb_workspace_t *w);
static void nav_arrived(orb_workspace_t *w, bool fresh);
static void cache_clear(orb_workspace_t *w);
static void delight_tick(lv_timer_t *timer);
static void launch_frame(orb_workspace_t *w, uint32_t elapsed);
static void delight_wake(orb_workspace_t *w);
static bool nav_start(orb_workspace_t *w, orb_shell_surface_t destination,
                      orb_nav_transition_kind_t kind, orb_nav_direction_t direction,
                      bool backwards, const orb_nav_token_rect_t *token, bool interactive);
static bool nav_capture(orb_workspace_t *w, orb_shell_surface_t surface, unsigned slot);
static orb_workspace_t *workspace_create_preview(lv_obj_t *parent, const orb_workspace_t *source);
static void nav_cancel(orb_workspace_t *w);
static void nav_accept(orb_workspace_t *w);
static void nav_build_live(orb_workspace_t *w);
static void nav_tick(lv_timer_t *timer);
static void nav_bind_rotary(orb_workspace_t *w);
static lv_obj_t *nav_scroll_owner(orb_workspace_t *w);
static void nav_prepare_async(void *ctx);
static bool nav_button(orb_workspace_t *w, orb_shell_surface_t surface, bool backwards);
static void nav_compass_show(orb_workspace_t *w);
static bool nav_destination_visible(const orb_workspace_t *w, lv_point_t point);
static void stack_refresh(orb_workspace_t *w, orb_stack_refresh_t refresh);
static void agents_focus_change(orb_workspace_t *w, int delta);
static void clicked(orb_workspace_t *w, int act, lv_obj_t *target, const orb_launch_token_t *launch);
static void agents_adapter(orb_workspace_t *w);
static bool agents_refresh(orb_workspace_t *w);
static void voice_adapter(orb_workspace_t *w);
static void notification_restore(void *context, lv_obj_t *area, const char *identity);
static const orb_session_t *card_session(orb_workspace_t *w, const orb_card_t *c);
static bool nowplaying_selected(const orb_workspace_t *w);
static bool nowplaying_clicked(orb_workspace_t *w, int action);
static bool agents_result_action(orb_workspace_t *w, int act);
static bool stack_read(void *context, orb_stack_read_t *read)
{
    const orb_workspace_t *w = context;
    *read = orb_stack_project(w->model, w->home, &w->time, &w->status,
        w->card, w->notice, w->page_index, w->surface);
    return true;
}
static unsigned stack_entries(const orb_workspace_t *w, orb_stack_entry_t *entries)
{
    orb_stack_read_t read; stack_read((void *)w, &read);
    return orb_stack_screen_entries(&read, entries);
}
static unsigned stack_count(const orb_workspace_t *w)
{
    return stack_entries(w, NULL);
}
static orb_stack_entry_t stack_selected(const orb_workspace_t *w)
{
    orb_stack_entry_t entries[ORB_STACK_MAX];
    unsigned count = stack_entries(w, entries);
    return count && w->page_index < count ? entries[w->page_index] : (orb_stack_entry_t){0};
}
static uint32_t stack_signature(const orb_workspace_t *w)
{
    orb_stack_entry_t entries[ORB_STACK_MAX];
    unsigned count = stack_entries(w, entries);
    uint32_t hash = count;
    for (unsigned i = 0; i < count; i++) hash = hash * 33u + entries[i].kind * 17u + entries[i].index;
    return hash;
}
static void stack_restore_entry(orb_workspace_t *w, orb_stack_entry_t previous)
{
    orb_stack_entry_t entries[ORB_STACK_MAX];
    unsigned count = stack_entries(w, entries);
    for (unsigned i = 0; i < count; i++)
        if (entries[i].kind == previous.kind && entries[i].index == previous.index) {
            w->page_index = i; return;
        }
}
static lv_color_t accent(orb_workspace_t *w, orb_delight_color_t color)
{
    return orb_color_from_rgb(orb_delight_color(color, w->contrast || orb_ui_reduce_transparency()));
}

static lv_color_t design_accent(orb_workspace_t *w, orb_delight_color_t color)
{
    return orb_place_identity_accent(color, orb_system_surface_uses_system_chrome(w->surface), w->contrast);
}

static bool home_doorbell(const orb_card_t *card)
{
    return card && card->kind == ORB_CARD_ATTENTION && !strncmp(card->id, "home-doorbell-", 14);
}

static const orb_session_t *selected_session(orb_workspace_t *w)
{
    for (size_t i = 0; i < w->model->session_count; i++)
        if (!strcmp(w->session, w->model->sessions[i].id)) return &w->model->sessions[i];
    if (w->has_selection && !strcmp(w->session, w->selection.id)) return &w->selection;
    return NULL;
}

static const orb_card_t *selected_card(orb_workspace_t *w)
{
    if (w->surface == ORB_SHELL_STACK && !w->card[0]) {
        unsigned count = stack_count(w);
        if (count) w->page_index = LV_MIN(w->page_index, count - 1);
        orb_stack_entry_t entry = stack_selected(w);
        return entry.kind == ORB_STACK_CARD ? &w->model->cards[entry.index] : NULL;
    }
    for (size_t i = 0; i < w->model->card_count; i++)
        if (!strcmp(w->card, w->model->cards[i].id)) return &w->model->cards[i];
    return NULL;
}
static bool ready(const orb_workspace_t *w) { return w->model->online && w->model->synced; }

/* Focus changes only on navigation, never on a worker callback into LVGL.
 * Keeping a read-only session focused makes BOOT fail closed at the provider. */
static void voice_focus(orb_workspace_t *w)
{
    if (w->fixture || w->surface == ORB_SHELL_VOICE) return;
    const orb_session_t *s = (w->surface == ORB_SHELL_SESSION || w->surface == ORB_SHELL_AGENTS) ? selected_session(w) : NULL;
    const orb_card_t *c = (w->surface == ORB_SHELL_CARD || w->surface == ORB_SHELL_STACK ||
                     w->surface == ORB_SHELL_NOTIFICATION_DETAIL) ? selected_card(w) : NULL;
    orb_model_voice_focus(w->surface == ORB_SHELL_TIME ? "time" :
                          s ? s->id : c && c->reply && c->session[0] ? c->session : "dot",
                          c && c->reply && !c->answered ? c->id : NULL);
}

static bool voice_project(void *context, orb_screen_id_t screen, orb_app_voice_model_t *model)
{
    orb_workspace_t *w = context;
    (void)screen;
    *model = (orb_app_voice_model_t){.model = w->model,
        .selection = w->has_selection ? &w->selection : NULL, .flow = w->voice_flow, .notice = w->notice,
        .selection_external = w->selection_external, .fixture = w->fixture,
        .materialise = w->navigation_pending && !w->nav_applying,
        .interactive = !w->fixture && !w->suppress_click && !w->opening,
        .touch_allowed = !w->fixture && !w->nav_presenting && !w->nav_pending,
        .outcome_live = !w->opening && !w->open.active && !w->nav_applying && !w->nav_pending && !w->nav_presenting,
        .capture_colour = accent(w, ORB_DELIGHT_RED)};
    return true;
}
static bool voice_physical(void *context) { return orb_input_require_physical(((orb_workspace_t *)context)->root); }
static bool voice_request(void *context, orb_voice_action_t action)
{ (void)context; return orb_model_voice_request(action, NULL, NULL); }
static void voice_notice(void *context, const char *text)
{ orb_workspace_t *w = context; snprintf(w->notice, sizeof(w->notice), "%s", text); }
static bool voice_command(orb_workspace_t *w, orb_voice_action_t action)
{
    orb_app_voice_model_t model; voice_project(w, ORB_SHELL_VOICE, &model);
    return orb_voice_flow_command(w->voice_flow, &model, action, &(orb_voice_command_port_t){
        .context = w, .physical = voice_physical, .request = voice_request, .notice = voice_notice});
}

static void back(orb_workspace_t *w)
{
    if (faces_screen(w) && orb_screen_back(faces_screen(w)) == ORB_BACK_CONSUMED) { nav_bind_rotary(w); return; }
    if (time_screen(w) && orb_screen_back(time_screen(w)) == ORB_BACK_CONSUMED) return;
    if (home_screen(w) && orb_screen_back(home_screen(w)) == ORB_BACK_CONSUMED) return;
    if ((w->surface == ORB_SHELL_ABOUT || w->surface == ORB_SHELL_SETTINGS ||
        w->surface == ORB_SHELL_ACCESSIBILITY || w->surface == ORB_SHELL_DISPLAY || w->surface == ORB_SHELL_SOUND) &&
        orb_settings_host_back_for(w->settings, w->surface)) return;
    orb_nav_back_entry_t entry;
    orb_shell_surface_t dest = orb_nav_back_peek(&w->nav, &entry) ? entry.id : ORB_SHELL_FACE;
    if (!nav_button(w, dest, true) && settings_prepare(w, dest, true)) {
        orb_nav_back(&w->nav, NULL);
        navigate(w, dest, false);
    }
}

static void change_page(orb_workspace_t *w, int delta)
{
    if (w->surface != ORB_SHELL_STACK && w->surface != ORB_SHELL_CONTROL_CENTER && w->surface != ORB_SHELL_USAGE) return;
    unsigned count = w->surface == ORB_SHELL_STACK ? stack_count(w) : 2;
    if (!count || !delta) return;
    int target = (int)w->page_index + delta;
    if (w->surface == ORB_SHELL_CONTROL_CENTER)
        target = (target % (int)count + (int)count) % (int)count;
    else target = LV_CLAMP(0, target, (int)count - 1);
    if (target == (int)w->page_index) return;
    w->page_index = (unsigned)target;
    if (w->surface == ORB_SHELL_USAGE) {
        /* Design 1.1: only the retained centre pages; the comparison rings
         * stay fixed. A rim detent changes no fonts, layout or allocations. */
        (void)orb_app_agents_usage_change(agents_screen(w), w->page_index);
        w->nav_revision++;
        return;
    }
    if (w->surface == ORB_SHELL_STACK) w->card[0] = '\0';
    voice_focus(w);
    w->page_motion = true; w->page_at = lv_tick_get();
    orb_delight_spring_reset(&w->page_spring, delta > 0 ? 20 : -20);
    orb_delight_spring_target(&w->page_spring, 0);
    delight_wake(w);
    render(w);
}

static void clicked_event(lv_event_t *e)
{
    orb_workspace_t *w = lv_event_get_user_data(e);

    if (w->fixture || w->suppress_click) return;
    lv_obj_t *target = lv_event_get_target_obj(e);
    if (w->curved && w->list && lv_obj_get_parent(target) == w->list) {
        if (lv_obj_get_style_opa(target, 0) == LV_OPA_TRANSP) return;
        lv_indev_t *input = lv_indev_active();
        lv_point_t point;
        if (input) {
            lv_indev_get_point(input, &point);
            if (!orb_round_list_hit_test(target, &point)) return;
        }
    }
    clicked(w, (int)(intptr_t)lv_obj_get_user_data(target), target, NULL);
}

static void prepare_launch(orb_workspace_t *w, lv_obj_t *target, int act)
{
        if (!target && (w->surface == ORB_SHELL_NOTIFICATIONS || w->surface == ORB_SHELL_NOTIFICATION_DETAIL))
            target = orb_app_notifications_action(orb_notifications_host_current(w->notifications_host), act);
        if (target) { lv_obj_remove_state(target, LV_STATE_PRESSED); orb_feedback_cancel(target); }
        else if (w->surface == ORB_SHELL_STACK) orb_stack_prepare_launch(orb_app_host_current(w->stack_host), act);
        if (w->surface == ORB_SHELL_APPS) {
            if (target) orb_kit_press_cancel(target);
            else orb_apps_prepare_launch(orb_apps_view_current(w->apps_view), act);
        }
}

static void clicked_command(orb_workspace_t *w, int act)
{
    switch (act) {
    case ACT_SLEEP:
        if (w->action) w->action(ORB_SHELL_ACTION_SLEEP, w->ctx);
        return;
    case ACT_QUIET:
        if (w->ctx) orb_shell_set_quiet(w->ctx, !orb_shell_quiet(w->ctx));
        break;
    case ACT_FLASHLIGHT:
        if (!w->flashlight) w->flashlight_brightness = w->status.brightness;
        w->flashlight = !w->flashlight;
        w->status.brightness = w->flashlight ? 100 : w->flashlight_brightness;
        if (w->action) w->action(ORB_SHELL_ACTION_BRIGHTNESS, w->ctx);
        break;
    case ACT_NOTICE_OPEN: {
        const orb_card_t *card = selected_card(w);
        if (card && card->session[0]) orb_workspace_session(w, card->session);
        return;
    }
    case ACT_NOTICE_REPLY: {
        const orb_card_t *card = selected_card(w);
        if (card && card->reply) {
            orb_model_voice_focus(card->session, card->id);
            navigate(w, ORB_SHELL_VOICE, true);
        }
        return;
    }
    case ACT_FOCUS_NEXT: agents_focus_change(w, 1); return;
    case ACT_FOCUS_PREVIOUS: agents_focus_change(w, -1); return;
    case ACT_FOCUS_OPEN:
        if (selected_session(w)) orb_workspace_session(w, w->session);
        return;
    case ACT_APPROVAL_NEXT: {
        bool after = false;
        for (unsigned i = 0; i < w->model->card_count; i++) {
            const orb_card_t *c = &w->model->cards[i];
            if (!strcmp(c->id, w->card)) { after = true; continue; }
            if (after && !c->answered && (c->kind == ORB_CARD_APPROVAL || c->kind == ORB_CARD_CHOICE)) {
                orb_workspace_card(w, c->id); return;
            }
        }
        for (unsigned i = 0; i < w->model->card_count; i++) {
            const orb_card_t *c = &w->model->cards[i];
            if (!c->answered && (c->kind == ORB_CARD_APPROVAL || c->kind == ORB_CARD_CHOICE)) {
                orb_workspace_card(w, c->id); return;
            }
        }
        return;
    }
    case ACT_BACK: back(w); return;
    case ACT_PAGE_NEXT: change_page(w, 1); return;
    case ACT_REPLY: navigate(w, ORB_SHELL_VOICE, true); return;
    case ACT_IDLE_MORE:
        if (!ready(w) || !orb_model_idle_more())
            snprintf(w->notice, sizeof(w->notice), "Couldn’t load sessions");
        break;
    case ACT_VOICE_STOP: voice_command(w, ORB_VOICE_STOP); break;
    case ACT_VOICE_CANCEL: voice_command(w, ORB_VOICE_CANCEL); break;
    case ACT_VOICE_SEND:
        if (!orb_voice_flow_pending(w->voice_flow) && w->model->voice.phase == ORB_VOICE_CONFIRMING)
            if (voice_command(w, ORB_VOICE_CONFIRM)) {
                orb_voice_flow_sent(w->voice_flow, lv_tick_get());
                if (strcmp(w->model->voice.target, "time")) orb_sound_play(ORB_SOUND_SENT);
                delight_wake(w);
            }
        break;
    case ACT_UNDO_CLEAR: orb_notification_history_undo(w->notifications); break;
    case ACT_CLEAR: orb_notification_history_clear(w->notifications); break;
    case ACT_SILENT: orb_workspace_set_silent(w, !w->silent); break;
    default: return;
    }
    if (w->surface == ORB_SHELL_CONTROL_CENTER && !w->page_index &&
        (act == ACT_SILENT || act == ACT_QUIET || act == ACT_FLASHLIGHT)) {
        orb_control_model_t model = orb_control_project(&w->status, orb_sound_volume(), w->silent,
            w->ctx && orb_shell_quiet(w->ctx), w->flashlight, w->control_volume);
        (void)orb_control_host_update(w->control_host, &model);
        return;
    }
    render(w);
}

static void clicked(orb_workspace_t *w, int act, lv_obj_t *target, const orb_launch_token_t *launch)
{
    if (w->opening && act != ACT_BACK) return;
    if (nowplaying_clicked(w, act) || agents_result_action(w, act)) return;
    if (act < ACT_BACK || act == ACT_TIME) {
        orb_shell_surface_t destination = act == ACT_TIME ? ORB_SHELL_TIME : (orb_shell_surface_t)act;
        if (act == ACT_TIME) {
            if (!orb_apps_find("time") || !orb_shell_app_request("time")) return;
            w->time_launched = w->surface == ORB_SHELL_APPS;
        }
        lv_area_t box = launch && launch->valid ? (lv_area_t){launch->rect.x, launch->rect.y,
            launch->rect.x + launch->rect.width - 1, launch->rect.y + launch->rect.height - 1} : (lv_area_t){0};
        if (target) lv_obj_get_coords(target, &box);
        /* Apps now supplies the actual disc bounds through its copied intent. */
        w->launch = (lv_point_t){(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
        /* Freeze the resting icon, not its transient pressed spring, for the
         * return iris. Otherwise closing an app flashes a shrunken token. */
        prepare_launch(w, target, act);
        if (!nav_button(w, destination, false))
            navigate(w, destination, true);
        return;
    }
    if (act >= ACT_FOCUS_SELECT && act < ACT_FOCUS_SELECT + ORB_MODEL_MAX_SESSIONS) {
        unsigned index = (unsigned)(act - ACT_FOCUS_SELECT);
        if (index < w->model->session_count) {
            snprintf(w->session, sizeof(w->session), "%s", w->model->sessions[index].id);
            orb_app_agents_select_focus(agents_screen(w), index); w->has_selection = false;
            voice_focus(w); agents_refresh(w);
        }
        return;
    }
    if (act == ACT_CONTROL_BRIGHTNESS || act == ACT_CONTROL_VOLUME) {
        w->control_volume = act == ACT_CONTROL_VOLUME;
        orb_nav_rotary_stop(&w->nav);
        render(w); return;
    }
    if (act == ACT_HOME_SCENE) {
        if (!orb_input_require_physical(w->root)) return;
        const orb_home_scene_t *scene = orb_control_home_scene(w->home, orb_system_home_hour(w->status.clock_time));
        if (!scene || strcmp(scene->id, w->home_scene_id)) { render(w); return; }
        if (orb_home_scene(scene->id)) orb_sound_play(ORB_SOUND_TAP);
        return;
    }
    if (act == ACT_HOME_FIRST || act == ACT_HOME_SECOND) {
        if (!orb_input_require_physical(w->root)) return;
        const orb_home_accessory_t *a = orb_control_home_favourite(w->home, act == ACT_HOME_SECOND);
        if (a && orb_home_toggle(a->id)) orb_sound_play(ORB_SOUND_TAP);
        return;
    }
    if (act == ACT_HOME_OFF) {
        if (!orb_input_require_physical(w->root)) return;
        const orb_home_room_t *room = orb_stack_home_room(w->home);
        /* The model drains a room batch through its four request slots.
         * A direct loop silently lost every write beyond those slots. */
        if (!room || !orb_home_room_off(room->id))
            snprintf(w->notice, sizeof(w->notice), "Try in Home");
        render(w); return;
    }
    if (act == ACT_STACK_SCENES) {
        w->stack_scenes = true;
        navigate(w, ORB_SHELL_HOME, true);
        return;
    }
    if (act == ACT_GAME_EXIT) { orb_shell_app_request(NULL); render(w); return; }
    if (act >= ACT_SESSION && act < ACT_SESSION + ORB_MODEL_MAX_SESSIONS) {
        w->has_selection = false;
        session_open_token(w, w->model->sessions[act - ACT_SESSION].id, launch && launch->valid ? &launch->rect : NULL); return;
    }
    if (act >= ACT_IDLE_SESSION && act < ACT_IDLE_SESSION + ORB_MODEL_MAX_IDLE) {
        unsigned i = act - ACT_IDLE_SESSION;
        if (!w->idle || i >= w->idle->count) return;
        w->selection = w->idle->sessions[i];
        w->has_selection = true;
        w->selection_external = true;
        session_open_token(w, w->selection.id, launch && launch->valid ? &launch->rect : NULL); return;
    }
    if (act >= ACT_CARD && act < ACT_CARD + ORB_MODEL_MAX_CARDS) {
        orb_workspace_card(w, w->model->cards[act - ACT_CARD].id); return;
    }
    if (act >= ACT_OPTION && act < ACT_OPTION + ORB_MODEL_MAX_OPTIONS) {
        if (!orb_input_require_physical(w->root)) return;
        const orb_card_t *card = selected_card(w);
        if (!card || card->answered || !ready(w)) return;
        unsigned index = (unsigned)(act - ACT_OPTION);
        if (index >= card->options_count) return;
        orb_answer_t answer = { 0 };
        snprintf(answer.card, sizeof(answer.card), "%s", card->id);
        snprintf(answer.option, sizeof(answer.option), "%s", card->options[index].id);
        if (!orb_model_answer(&answer)) snprintf(w->notice, sizeof(w->notice), "Could not queue. Try again.");
        else {
            /* Local queue acceptance is not delivery (HIG §§5/8). The pressed
             * control already supplies neutral feedback; no outcome cue here. */
            snprintf(w->card, sizeof(w->card), "%s", card->id);
            orb_notification_history_mark_answer(w->notifications, card->id,
                answer.option, card->options[index].label);
        }
        orb_workspace_poll(w);
        render(w);
        return;
    }
    if (act >= ACT_RECENT && act < ACT_RECENT + EVENT_COUNT) {
        orb_notification_history_select(w->notifications, act - ACT_RECENT);
        const orb_notification_record_t *record = orb_notification_history_at(w->notifications, act - ACT_RECENT);
        if (record && record->approval) { orb_workspace_card(w, record->id); return; }
        snprintf(w->card, sizeof(w->card), "%s", record ? record->id : "");
        navigate(w, ORB_SHELL_NOTIFICATION_DETAIL, true); return;
    }
    clicked_command(w, act);
}

static bool apps_active(void *context)
{
    return ((orb_workspace_t *)context)->surface == ORB_SHELL_APPS;
}

static bool apps_command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    orb_workspace_t *w = context;
    if (intent->kind != ORB_INTENT_APP_COMMAND || intent->value.command.target != ORB_SHELL_APPS ||
        w->surface != ORB_SHELL_APPS) return false;
    unsigned action = intent->value.command.action;
    if (action == ORB_APPS_FOCUS || action == ORB_APPS_CANCEL) {
        if (action == ORB_APPS_CANCEL) w->suppress_click = true;
        if (intent->value.command.value != intent->value.command.extra && w->compositor) {
            /* Local focus must retire the same frozen Apps planes before an iris. */
            w->nav_apps_warm = false;
            for (unsigned slot = 0; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++) {
                if (w->nav_cached[slot] != ORB_SHELL_APPS) continue;
                w->nav_apps_preview[slot] = false;
                orb_nav_compositor_invalidate(w->compositor, slot);
            }
            lv_timer_resume(w->nav_timer);
        }
        return true;
    }
    (void)instance;
    if (w->fixture || w->suppress_click || w->opening) return false;
    clicked(w, action, NULL, &intent->launch);
    return true;
}

static lv_obj_t *arrival_action(orb_workspace_t *w, const char *text, int act)
{
    lv_obj_t *obj = orb_kit_button_secondary_create(w->page, text);
    lv_obj_update_layout(obj);
    lv_obj_set_pos(obj, 233 - lv_obj_get_width(obj) / 2, 54 - lv_obj_get_height(obj) / 2);
    lv_obj_set_user_data(obj, (void *)(intptr_t)act);
    lv_obj_set_event_bubble(obj, true);
    lv_obj_add_event_cb(obj, clicked_event, LV_EVENT_CLICKED, w);
    return obj;
}

/* Only Home card actions and the active-game fallback retain this composition.
 * Their original capsule and fitting differ from the standard kit button. */
static lv_obj_t *button(orb_workspace_t *w, const char *caption, int act)
{
    lv_obj_t *obj = orb_kit_card_action_create(w->page);
    lv_obj_set_user_data(obj, (void *)(intptr_t)act);
    lv_obj_set_event_bubble(obj, true);
    orb_feedback_press(obj, false);
    lv_obj_add_event_cb(obj, clicked_event, LV_EVENT_CLICKED, w);
    bool disabled = (act == ACT_OPTION || act == ACT_REPLY) && !ready(w);
    bool primary = (act == ACT_REPLY || act == ACT_GAME_EXIT) && !disabled;
    {
        orb_shell_surface_t identity = w->surface;
        if (act == ACT_REPLY && (w->surface == ORB_SHELL_CARD || w->surface == ORB_SHELL_STACK)) {
            const orb_card_t *card = selected_card(w);
            identity = ORB_SHELL_DOT;
            if (card) for (unsigned i = 0; i < w->model->session_count; i++)
                if (!strcmp(w->model->sessions[i].id, card->session))
                    identity = w->model->sessions[i].agent == ORB_AGENT_DOT ? ORB_SHELL_DOT : ORB_SHELL_AGENTS;
        }
        orb_kit_card_action_finish(obj, caption, primary, disabled,
            accent(w, orb_system_surface_accent(identity)),
            w->navigation_pending && !w->nav_applying);
    }
    return obj;
}

static void header(orb_workspace_t *w, const char *title)
{
    /* HIG §7: even the opening scaffold uses the shared rim identity. */
    if (w->heading) lv_obj_delete(w->heading);
    lv_color_t colour = w->surface == ORB_SHELL_AGENTS ? ORB_KIT_CLAUDE :
        w->surface == ORB_SHELL_DOT ? ORB_KIT_DOT : ORB_KIT_SECONDARY;
    w->heading = orb_kit_rim_title_create(w->page, title, colour);
}

/* The projection reads the workspace's immutable, refcounted generations;
 * the app owns all interpretation of those service values. */
bool orb_workspace_face_snapshot(const orb_workspace_t *w, orb_faces_snapshot_t *snapshot)
{
    if (!w || !snapshot) return false;
    orb_faces_host_project(snapshot, w->model, w->home, &w->status);
    return true;
}

static void face_snapshot(orb_workspace_t *w)
{
    if (!faces_screen(w) || !w->shown || !w->display_active) return;
    orb_faces_snapshot_t snapshot;
    if (!orb_workspace_face_snapshot(w, &snapshot)) return;
    orb_faces_host_update(w->faces_host, &snapshot);
}

static void face_open(orb_faces_target_t target, void *context)
{
    orb_workspace_t *w = context;
    static const orb_shell_surface_t routes[] = {
        ORB_SHELL_APPS, ORB_SHELL_USAGE, ORB_SHELL_USAGE, ORB_SHELL_HOME,
        ORB_SHELL_BATTERY, ORB_SHELL_WEATHER, ORB_SHELL_TIME, ORB_SHELL_TIME,
    };
    if ((unsigned)target >= sizeof(routes) / sizeof(routes[0])) return;
    /* Date and next-event providers open the existing Time utility. */
    if (routes[target] == ORB_SHELL_TIME) {
        if (!orb_apps_find("time") || !orb_shell_app_request("time")) return;
        w->time_launched = true;
    }
    w->face_usage_page = target == ORB_FACES_OPEN_CODEX;
    w->face_launch = true;
    lv_obj_t *control = lv_indev_get_active_obj();
    lv_area_t bounds = {193, 276, 272, 355};
    if (control) {
        orb_kit_press_cancel(control);
        lv_obj_update_layout(control);
        lv_obj_get_coords(control, &bounds);
    }
    orb_nav_token_rect_t token = {bounds.x1, bounds.y1,
        lv_area_get_width(&bounds), lv_area_get_height(&bounds)};
    w->launch = (lv_point_t){bounds.x1 + token.width / 2, bounds.y1 + token.height / 2};
    if (!nav_start(w, routes[target], ORB_NAV_TRANSITION_IRIS,
                   ORB_NAV_DIRECTION_LEFT, false, &token, false))
        navigate(w, routes[target], true);
}

static bool face_command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    if (intent->value.command.action == ORB_APP_FACES_PREFERENCES)
        return orb_faces_host_preferences(instance, intent);
    if (intent->kind != ORB_INTENT_APP_COMMAND || intent->value.command.action != ORB_APP_FACES_OPEN ||
        intent->value.command.value < ORB_FACES_OPEN_APPS || intent->value.command.value > ORB_FACES_OPEN_DATE)
        return false;
    face_open((orb_faces_target_t)intent->value.command.value, context);
    return true;
}

static void face(orb_workspace_t *w)
{
    if (!w->faces_host) w->faces_host = orb_faces_host_create(w->fixture, face_command, w);
    (void)orb_faces_host_attach(w->faces_host, w->page);
    orb_app_faces_active(faces_screen(w), w->shown && w->display_active && !w->always_on && !w->face_down, false);
    face_snapshot(w);
}

static void scroll_position(lv_event_t *event)
{
    orb_workspace_t *w = lv_event_get_user_data(event);
    lv_obj_t *list = lv_event_get_current_target_obj(event);
    int top = lv_obj_get_scroll_y(list);
    bool moved = !w->nav_scroll_observed || top != w->nav_scroll_position;
    w->nav_scroll_position = top;
    w->nav_scroll_observed = true;
    /* A cached current place must follow its final scroll position. Admission
     * waits for momentum/crown ownership to end; never snapshot mid-motion. */
    if (moved && w->compositor && !w->nav_presenting && !w->nav_pending && !w->nav_applying) {
        for (unsigned i = 0; i < ORB_NAV_SNAPSHOT_SLOTS; i++)
            if (w->nav_cached[i] == w->surface) orb_nav_compositor_invalidate(w->compositor, i);
        lv_timer_resume(w->nav_timer);
    }
}

static void workspace_chrome_clear(orb_workspace_t *w);

static bool apps(orb_workspace_t *w)
{
    workspace_chrome_clear(w);
    return orb_apps_view_build(w->apps_view, w->page, w->opening, orb_apps_find("time") != NULL);
}

/* Scroll prose inside a safe central chord; the primary action never becomes
 * a reading control. Complete text remains selectable by finger or the rim. */
static lv_obj_t *prose(orb_workspace_t *w, int y, int h)
{
    lv_obj_t *area = orb_kit_detail_page(w->page, &(orb_kit_detail_page_config_t){
        .top = y, .height = h, .system = orb_system_surface_uses_system_chrome(w->surface),
        .arc = orb_system_surface_uses_system_chrome(w->surface) && w->surface != ORB_SHELL_STACK,
    });
    w->list = area; w->curved = false;
    return area;
}

#include "orb_voice_notifications.inc"
#include "orb_workspace_agents.inc"

static const orb_session_t *card_session(orb_workspace_t *w, const orb_card_t *c)
{
    for (size_t i = 0; i < w->model->session_count; i++)
        if (!strcmp(c->session, w->model->sessions[i].id)) return &w->model->sessions[i];
    return NULL;
}

static void card_receipt(orb_workspace_t *w, const orb_notification_record_t *r)
{
    orb_home_receipt_copy_t copy = orb_home_event_receipt(r);
    lv_obj_t *area = prose(w, 154, 236);
    if (copy.delivered)
        orb_feedback_check(orb_kit_receipt_mark(area), accent(w, ORB_DELIGHT_GREEN), false);
    orb_kit_receipt_content(area, &(orb_kit_receipt_config_t){
        .title = copy.title, .decision = copy.decision, .explanation = copy.explanation,
        .answer = copy.answer, .secondary = SECONDARY,
    });
}

static void card(orb_workspace_t *w)
{
    const orb_card_t *c = selected_card(w);
    if (!home_doorbell(c) && w->surface == ORB_SHELL_CARD) { agents_adapter(w); return; }
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    bool stack = w->surface == ORB_SHELL_STACK;
    const char *kind = !c ? "Card" : c->kind == ORB_CARD_APPROVAL ? "Approval" :
        c->kind == ORB_CARD_CHOICE ? "Choices" : c->kind == ORB_CARD_MESSAGE ? "Dot" : "Attention";
    header(w, w->notice[0] ? "Not queued" : !w->model->online ? "Offline" : !ready(w) ? "Syncing" : stack ? "Stack" : kind);
    const orb_notification_record_t *receipt = orb_notification_history_find(w->notifications, c ? c->id : w->card);
    if ((receipt && (receipt->queued || receipt->resolved)) || (c && c->answered)) {
        card_receipt(w, receipt);
        return;
    }
    if (home_doorbell(c) && receipt && !receipt->doorbell_shown) {
        /* HIG §8: the visible Front Door card is the cue's visual pair. The
         * sound service drops muted/busy cues; rebuilds never replay them. */
        orb_notification_history_mark_doorbell_shown(w->notifications, receipt->id);
        orb_sound_play(ORB_SOUND_NOTIFICATION);
    }
    const orb_session_t *s = card_session(w, c);
    /* Home events retain their house identity without impersonating Dot. */
    bool doorbell = home_doorbell(c);
    lv_color_t source_color = doorbell ? accent(w, ORB_DELIGHT_YELLOW) :
        s ? orb_place_agent_accent(s->agent, orb_system_surface_uses_system_chrome(w->surface), w->contrast) : accent(w, ORB_DELIGHT_INDIGO);
    orb_kit_page_label(w->page, doorbell ? ORB_ICON_HOUSE : s ? agents_icon(s->agent) : ORB_ICON_CHAT, ORB_TEXT_FOOTNOTE2, true,
          233, 100, 60, 1, source_color);
    /* All agent cards were handled above; this is the Home event body. */
    {
        orb_kit_page_label(w->page, c->title, ORB_TEXT_HEADLINE, true, 233, 142, 336, 1, ORB_LABEL);
        bool dismiss = home_doorbell(c) && c->options_count == 1 && !strcmp(c->options[0].id, "dismiss");
        lv_obj_t *area = prose(w, ax ? 201 : 192, c->reply || dismiss ? (ax ? 142 : 152) : 215);
        const char *body = c->body;
        char doorbell_body[sizeof(c->body)];
        size_t length = strlen(body);
        if (home_doorbell(c) && length >= 6 && body[length - 6] == ' ' && body[length - 3] == ':' &&
            body[length - 5] >= '0' && body[length - 5] <= '2' &&
            body[length - 4] >= '0' && body[length - 4] <= '9' &&
            body[length - 2] >= '0' && body[length - 2] <= '5' &&
            body[length - 1] >= '0' && body[length - 1] <= '9') {
            /* HIG §4: keep the local time together at enlarged text sizes.
             * This is a display break; the admitted card body stays exact. */
            memcpy(doorbell_body, body, length + 1);
            doorbell_body[length - 6] = '\n';
            body = doorbell_body;
        }
        orb_kit_paragraph(area, body, ORB_TEXT_BODY, false, 0, ORB_LABEL, false);
        lv_obj_update_layout(area);
        if (c->reply) button(w, ORB_ICON_MIC " Reply", ACT_REPLY);
        else if (dismiss) button(w, c->options[0].label, ACT_OPTION);
    }
}

static bool workspace_command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent);
static void notifications(orb_workspace_t *w)
{
    if (w->surface == ORB_SHELL_NOTIFICATION_DETAIL && !selected_card(w) &&
        !w->card[0] && w->model->card_count)
        snprintf(w->card, sizeof(w->card), "%s", w->model->cards[0].id);
    orb_notifications_view_config_t config = {.page = w->page, .title = w->heading,
        .history = w->notifications, .model = w->model, .card = w->card, .now = lv_tick_get(),
        .clicked = clicked_event, .scrolled = scroll_position, .context = w,
        .restore = notification_restore, .actions = {.recent = ACT_RECENT, .clear = ACT_CLEAR,
            .undo = ACT_UNDO_CLEAR, .reply = ACT_NOTICE_REPLY, .open_session = ACT_NOTICE_OPEN,
            .dismiss = ACT_NOTICE_DISMISS, .home = ORB_SHELL_HOME, .updates = ORB_SHELL_UPDATES}};
    orb_notifications_page_t page = orb_notifications_host_compose(w->notifications_host, &config,
        w->surface == ORB_SHELL_NOTIFICATION_DETAIL, workspace_command);
    w->heading = page.title;
    w->list = page.viewport; w->curved = page.curved;
}

static void voice_synthetic_refused(void *context, const orb_intent_t *intent)
{
    if (intent->kind == ORB_INTENT_SEND) (void)voice_physical(context);
}
static bool voice_accept(void *context, const orb_intent_t *intent)
{
    orb_workspace_t *w = context;
    unsigned action = intent->value.command.action;
    if (action == ORB_APP_VOICE_TOUCH_START) {
        if (orb_voice_flow_held(w->voice_flow) || orb_voice_flow_pending(w->voice_flow)) return false;
        if (voice_command(w, ORB_VOICE_START)) orb_voice_flow_touch(w->voice_flow, true);
        else render(w);
    } else if (action == ORB_APP_VOICE_TOUCH_RELEASE) {
        if (orb_voice_flow_held(w->voice_flow)) {
            orb_voice_flow_touch(w->voice_flow, false);
            voice_command(w, ORB_VOICE_STOP); render(w);
        }
    } else clicked(w, (int)action, NULL, NULL);
    return true;
}
static void voice_adapter(orb_workspace_t *w)
{
    workspace_chrome_clear(w);
    if (!w->voice_host) w->voice_host = orb_voice_host_create(w->fixture,
        &(orb_voice_host_config_t){.context = w, .project = voice_project,
            .accept = voice_accept, .synthetic_refused = voice_synthetic_refused});
    if (!orb_voice_host_attach(w->voice_host, w->page, ORB_SHELL_VOICE)) return;
    orb_screen_instance_t *screen = orb_voice_host_current(w->voice_host);
    w->list = orb_screen_page_viewport(screen); w->heading = orb_screen_page_title(screen);
    if (w->heading) lv_obj_set_parent(w->heading, w->page);
    w->restore_place = orb_app_voice_restore_scroll(screen, &w->restore_scroll);
    if (w->model->voice.phase == ORB_VOICE_CONFIRMING) delight_wake(w);
}

static bool play_input_allowed(void *context)
{
    orb_workspace_t *w = context;
    return !w->fixture && !w->suppress_click && !w->nav_pending && !w->nav_presenting;
}

static bool play_command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    orb_workspace_t *w = context;
    (void)instance;
    if (!play_input_allowed(w) || intent->kind != ORB_INTENT_APP_COMMAND ||
        intent->value.command.target != ORB_APP_PLAY_SCREEN_ID ||
        intent->value.command.action != ORB_APP_PLAY_LAUNCH ||
        (unsigned)intent->value.command.value >= ORB_GAME_COUNT) return false;
    if (!orb_shell_app_request(orb_game_id(intent->value.command.value)))
        snprintf(w->notice, sizeof(w->notice), "Launch pending");
    render(w);
    return true;
}

static orb_screen_instance_t *play_screen(orb_workspace_t *w)
{
    return orb_app_host_current(w->play_host);
}

static void play_sync_selection(orb_workspace_t *w)
{
    if (!orb_app_play_selection_changed(play_screen(w))) return;
    /* Peek taps change the picker locally. Retire only its stale snapshots;
     * an unchanged picker and its retained Apps return need no new capture. */
    if (w->compositor) {
        for (unsigned slot = 0; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++)
            if (w->nav_cached[slot] == ORB_SHELL_PLAY)
                orb_nav_compositor_invalidate(w->compositor, slot);
    }
    if (w->nav_timer) lv_timer_resume(w->nav_timer);
}

static void play_select(orb_workspace_t *w, int delta)
{
    if (!orb_app_play_picker(play_screen(w))) return;
    orb_app_play_select(play_screen(w), delta);
    play_sync_selection(w);
}

static void play(orb_workspace_t *w)
{
    /* Stream F: Time is a utility destination, not a game placeholder. */
    if (w->time_open) {
        /* HIG §§1/7: the mode title belongs to Time. System left-rim Back
         * and PWR already provide exits; avoid a second toolbar on the dial. */
        if (w->heading) { lv_obj_delete(w->heading); w->heading = NULL; }
        if (!w->time_host) w->time_host = orb_time_host_create(w->fixture);
        (void)orb_time_host_attach(w->time_host, w->page, &(orb_app_time_model_t){.contrast = w->contrast,
            .system_input = w->compositor != NULL}, w->time.alarm_ringing, w->time.reminder_due);
        return;
    }
    /* Retire the staged-open receipt before showing the frame's miniature. */
    if (w->heading) { lv_obj_delete(w->heading); w->heading = NULL; }
    /* HIG §§1/7: Play is the horizontal miniature picker, with BOOT as Back. */
    orb_shell_app_state_t state; orb_shell_apps_snapshot(&state);
    if (!orb_apps_count()) {
        orb_kit_page_label(w->page, "Play", ORB_TEXT_HEADLINE, true, 233, 38, 280, 1, ORB_LABEL);
        orb_kit_page_label(w->page, "No games installed", ORB_TEXT_HEADLINE, true, 233, 260, 346, 2, ORB_LABEL);
        return;
    }
    if (state.active[0]) {
        const orb_app_manifest_t *app = orb_apps_find(state.active);
        orb_kit_page_label(w->page, app ? app->name : "Game", ORB_TEXT_TITLE3, true, 233, 175, 340, 2, ORB_LABEL);
        orb_kit_page_label(w->page, "Hold PWR to exit", ORB_TEXT_BODY, false, 233, 272, 340, 2, SECONDARY);
        button(w, "Close", ACT_GAME_EXIT);
        return;
    }
    if (!w->play_host) w->play_host = orb_play_host_create(w->fixture, w, play_command);
    (void)orb_play_host_attach(w->play_host, w->page, w->compositor != NULL, play_input_allowed, w);
    if (state.pending || state.error)
        orb_kit_page_label(w->page, state.pending ? "Opening…" : "Couldn't open. Try again.",
              ORB_TEXT_FOOTNOTE2, false, 233, 38, 280, 1, SECONDARY);
}

static bool system_event(void *context, orb_system_view_event_t event)
{
    orb_workspace_t *w = context;
    switch (event) {
    case ORB_SYSTEM_VIEW_INPUT: return !w->fixture && !w->suppress_click && !w->opening;
    case ORB_SYSTEM_VIEW_PHYSICAL: return lv_indev_active() && orb_input_require_physical(w->root);
    case ORB_SYSTEM_VIEW_DONE: navigate(w, ORB_SHELL_FACE, false); break;
    case ORB_SYSTEM_VIEW_CHECK: if (w->action) w->action(ORB_SHELL_ACTION_CHECK_UPDATE, w->ctx); break;
    case ORB_SYSTEM_VIEW_CHANGED: render(w); break;
    }
    return true;
}

static orb_system_views_t *system_host(orb_workspace_t *w)
{
    if (!w->system_views) w->system_views = orb_system_views_create(w->fixture, system_event, w);
    return w->system_views;
}

static void system_view(orb_workspace_t *w)
{
    if (w->surface == ORB_SHELL_ONBOARDING) workspace_chrome_clear(w);
    else if (w->heading) { lv_obj_delete(w->heading); w->heading = NULL; }
    if (!orb_system_views_attach(system_host(w), w->page, w->surface, &w->status)) return;
    orb_screen_instance_t *instance = orb_system_views_current(w->system_views, w->surface);
    w->heading = orb_screen_page_title(instance); w->list = orb_screen_page_viewport(instance);
    w->curved = w->surface == ORB_SHELL_ONBOARDING;
}

static void settings_changed(void *context)
{
    orb_workspace_t *w = context;
    if (w->surface == ORB_SHELL_ACCESSIBILITY) { w->notice[0] = '\0'; w->text_notice_pending = false; }
    w->navigation_pending = true; render(w);
}

static bool settings_sound_action(orb_workspace_t *w, const orb_intent_t *intent)
{
    if (intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_SETTINGS_SOUND_SCREEN_ID &&
        intent->value.command.action == ORB_SETTINGS_SOUND_COMMAND_PREVIEW &&
        (unsigned)intent->value.command.value < ORB_SOUND_CUE_COUNT) {
        snprintf(w->notice, sizeof(w->notice), "%s", orb_sound_preview(intent->value.command.value) ?
            "Previewing" : "Silent or unavailable");
        render(w); return true;
    }
    if (intent->kind != ORB_INTENT_SETTING_SET) return false;
    uint32_t key = intent->value.setting.key;
    if (key == ORB_SETTING_SOUND_VOLUME) {
        orb_sound_set_volume(intent->value.setting.value);
        if (intent->origin == ORB_INPUT_TOUCH) orb_sound_play(ORB_SOUND_DETENT);
        return true;
    }
    if (key == ORB_SETTING_SOUND_SILENT) {
        w->silent = !w->silent; orb_sound_set_silent(w->silent); return true;
    }
    orb_sound_preferences_t prefs; orb_sound_preferences(&prefs);
    if (key == ORB_SETTING_SOUND_SPEECH) {
        prefs.spoken_replies = !prefs.spoken_replies;
        orb_sound_set_preferences(&prefs); return true;
    }
    if (key < ORB_SETTING_SOUND_QUIET_ENABLED || key > ORB_SETTING_SOUND_QUIET_END) return false;
    if (intent->origin == ORB_INPUT_CROWN) {
        unsigned *hour = key == ORB_SETTING_SOUND_QUIET_START ? &prefs.quiet_start : &prefs.quiet_end;
        unsigned value = (unsigned)LV_CLAMP(0, intent->value.setting.value, 23);
        if (*hour != value) { *hour = value; orb_sound_set_preferences(&prefs); render(w); }
        return true;
    }
    w->notice[0] = '\0';
    if (key == ORB_SETTING_SOUND_QUIET_ENABLED) prefs.quiet_enabled = !prefs.quiet_enabled;
    else if (key == ORB_SETTING_SOUND_QUIET_START) prefs.quiet_start = intent->value.setting.value;
    else prefs.quiet_end = intent->value.setting.value;
    if (!orb_sound_set_preferences(&prefs)) snprintf(w->notice, sizeof(w->notice), "Not saved");
    w->navigation_pending = true; render(w); return true;
}

static bool settings_accept(void *context, const orb_intent_t *intent)
{
    orb_workspace_t *w = context;
    bool direct_level = intent->kind == ORB_INTENT_SETTING_SET &&
        (intent->value.setting.key == ORB_SETTING_DISPLAY_BRIGHTNESS || intent->value.setting.key == ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS || intent->value.setting.key == ORB_SETTING_SOUND_VOLUME ||
         (intent->origin == ORB_INPUT_CROWN && (intent->value.setting.key == ORB_SETTING_SOUND_QUIET_START ||
          intent->value.setting.key == ORB_SETTING_SOUND_QUIET_END)));
    if (w->fixture || (!direct_level && intent->origin != ORB_INPUT_BUTTON && (w->suppress_click || w->opening))) return false;
    if (w->surface == ORB_SHELL_SOUND && settings_sound_action(w, intent)) return true;
    if (w->surface == ORB_SHELL_DISPLAY && intent->kind == ORB_INTENT_SETTING_SET) {
        uint32_t key = intent->value.setting.key;
        if (key == ORB_SETTING_DISPLAY_BRIGHTNESS) {
            w->status.brightness = intent->value.setting.value; w->brightness_pending = true; return true;
        }
        if (key == ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS) {
            orb_settings_host_setting_queue(w->settings, key, intent->value.setting.value); return true;
        }
        if (key == ORB_SETTING_DISPLAY_ALWAYS_ON || key == ORB_SETTING_DISPLAY_PICKUP) {
            if (!w->ctx) return false;
            orb_power_preferences_t power = orb_shell_power_preferences(w->ctx);
            if (key == ORB_SETTING_DISPLAY_ALWAYS_ON) power.always_on = !power.always_on;
            else power.motion_wake = !power.motion_wake;
            orb_shell_set_power_preferences(w->ctx, &power);
        } else if (key == ORB_SETTING_DISPLAY_QUIET) {
            if (w->ctx) orb_shell_set_quiet(w->ctx, !orb_shell_quiet(w->ctx));
        } else return false;
        render(w); return true;
    }
    if (w->surface == ORB_SHELL_BATTERY && intent->kind == ORB_INTENT_SETTING_SET &&
        intent->value.setting.key == ORB_SETTINGS_KEY_DESK_LIMIT) {
        if (!intent->physical) {
            if (lv_indev_active()) (void)orb_input_require_physical(w->root);
            return false;
        }
        if (!lv_indev_active() || !orb_input_require_physical(w->root)) return false;
        if (w->action) w->action(ORB_SHELL_ACTION_DESK_LIMIT, w->ctx);
        return true;
    }
    if (w->surface == ORB_SHELL_ACCESSIBILITY && orb_settings_host_accessibility_apply(intent,
        &w->contrast, w->notice, sizeof(w->notice), w->ctx)) { w->text_notice_pending = intent->value.setting.key <= ORB_SETTING_ACCESS_BOLD && orb_fonts_pending(); render(w); return true; }
    if (intent->kind == ORB_INTENT_NAVIGATE) {
        if (intent->value.navigate.screen == w->surface) {
            if (w->surface == ORB_SHELL_DISPLAY && intent->value.navigate.args.page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE && w->ctx)
                orb_shell_power_enable(w->ctx, true);
            return true;
        }
        if (intent->value.navigate.screen > ORB_SHELL_ONBOARDING) return false;
        const orb_nav_token_t *token = &intent->value.navigate.launch.rect;
        if (intent->value.navigate.launch.valid)
            w->launch = (lv_point_t){token->x + (token->width - 1) / 2, token->y + (token->height - 1) / 2};
        if (!nav_button(w, intent->value.navigate.screen, false)) navigate(w, intent->value.navigate.screen, true);
        return true;
    }
    if (w->surface == ORB_SHELL_SETTINGS && intent->kind == ORB_INTENT_APP_COMMAND &&
        intent->value.command.target == 0 && intent->value.command.action == ORB_SETTINGS_COMMAND_CONNECTION) {
        if (w->service) w->service(w->ctx, true);
        return true;
    }
    if (intent->kind == ORB_INTENT_SETTING_SET && intent->value.setting.key == ORB_SETTINGS_SETTING_GLASS) {
        orb_glass_set_preference(intent->value.setting.value);
        cache_clear(w); w->nav_revision++;
        for (unsigned i = 0; i < ORB_NAV_SNAPSHOT_SLOTS; i++) orb_nav_compositor_invalidate(w->compositor, i);
        render(w);
        return true;
    }
    if (intent->kind != ORB_INTENT_SETTING_SET) return false;
    uint32_t key = intent->value.setting.key;
    if (key >= ORB_SETTING_SOURCE_CLAUDE && key <= ORB_SETTING_SOURCE_RECENT_MINUTES &&
        (!ready(w) || w->model->prefs_pending)) return false;
    orb_prefs_patch_t patch = {0};
    if (key == ORB_SETTINGS_SETTING_TEMPERATURE) {
        patch.has_temperature = true;
        patch.temperature = intent->value.setting.value == ORB_SETTINGS_TEMPERATURE_F ? ORB_TEMPERATURE_F :
            intent->value.setting.value == ORB_SETTINGS_TEMPERATURE_C ? ORB_TEMPERATURE_C : ORB_TEMPERATURE_UNKNOWN;
    } else if (key >= ORB_SETTING_SOURCE_CLAUDE && key <= ORB_SETTING_SOURCE_DOT) {
        unsigned source = key - ORB_SETTING_SOURCE_CLAUDE;
        patch.sources_mask = 1u << source;
        patch.sources[source] = !w->model->prefs.sources[source];
    } else if (key == ORB_SETTING_SOURCE_RECENT_MINUTES) {
        patch.has_idle_minutes = true;
        unsigned current = w->model->prefs.idle_minutes;
        patch.idle_minutes = current == 0 ? 5 : current == 5 ? 10 : current == 10 ? 30 : 0;
    } else return false;
    if (!orb_model_prefs_request(&patch)) snprintf(w->notice, sizeof(w->notice), "Could not save. Try again.");
    orb_workspace_poll(w);
    render(w);
    return true;
}

static bool settings_surface(orb_shell_surface_t surface)
{
    return surface == ORB_SHELL_SETTINGS || surface == ORB_SHELL_ABOUT || surface == ORB_SHELL_ACCESSIBILITY ||
        surface == ORB_SHELL_UNITS || surface == ORB_SHELL_TEMPERATURE || surface == ORB_SHELL_SESSION_SETTINGS ||
        surface == ORB_SHELL_BATTERY || surface == ORB_SHELL_DISPLAY || surface == ORB_SHELL_SOUND || surface == ORB_SHELL_WEATHER;
}

static bool settings_prepare(orb_workspace_t *w, orb_shell_surface_t surface, bool arrival)
{
    if (!orb_settings_host_can_depart(w->settings)) return false;
    if (!settings_surface(surface)) return true;
    bool created = !w->settings;
    if (created) w->settings = orb_settings_host_create(w->fixture, settings_changed, settings_accept, w);
    bool prepared = surface == ORB_SHELL_ABOUT ?
        orb_settings_host_stage_about(w->settings, w->root, &w->status, orb_system_connection_summary(&w->status, w->model->online, w->model->synced), ready(w)) :
        surface == ORB_SHELL_SETTINGS ? orb_settings_host_stage_root(w->settings, w->root, &w->status, w->silent) :
        surface == ORB_SHELL_SOUND ? orb_settings_host_stage_sound(w->settings, w->root, w->silent, w->model->speech_voice, arrival ? "" : w->notice) :
        surface == ORB_SHELL_DISPLAY ? orb_settings_host_stage_display(w->settings, w->root, w->ctx, w->status.brightness) :
        surface == ORB_SHELL_BATTERY ? orb_settings_host_stage_battery(w->settings, w->root, &w->status) :
        surface == ORB_SHELL_WEATHER ? orb_settings_host_stage_weather(w->settings, w->root, w->model) :
        surface == ORB_SHELL_SESSION_SETTINGS ? orb_settings_host_stage_sources(w->settings, w->root, w->model, arrival ? "" : w->notice) :
        surface == ORB_SHELL_ACCESSIBILITY ?
            orb_settings_host_stage_accessibility(w->settings, w->root, w->contrast, arrival ? "" : w->notice) :
        orb_settings_host_stage_units(w->settings, w->root, surface, w->model, arrival ? "" : w->notice);
    if (!prepared && created) { orb_settings_host_destroy(w->settings); w->settings = NULL; }
    return prepared;
}

static lv_obj_t *workspace_level(const orb_workspace_t *w)
{
    return w->surface == ORB_SHELL_DISPLAY ? orb_settings_host_display_level(w->settings) :
        w->surface == ORB_SHELL_SOUND ? orb_settings_host_sound_level(w->settings) :
        orb_control_level(orb_app_host_current(w->control_host));
}

static void settings_adapter(orb_workspace_t *w)
{
    if (w->heading) { lv_obj_delete(w->heading); w->heading = NULL; }
    orb_screen_visibility(orb_settings_host_current(w->settings), ORB_VISIBILITY_ACTIVE);
    w->list = orb_settings_host_list(w->settings);
    w->heading = orb_settings_host_title(w->settings);
    w->curved = w->list != NULL;
    if (w->list) {
        lv_obj_add_event_cb(w->list, scroll_position, LV_EVENT_SCROLL, w);
        orb_kit_list_refresh(w->list);
    }
}

static int64_t open_now(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return (int64_t)lv_tick_get() * 1000;
#endif
}

static bool cacheable(orb_shell_surface_t surface)
{
    /* No approval, recording, answer receipt or other consequential surface is
     * ever replaced by a frozen image, even for the first navigation frame. */
    return surface == ORB_SHELL_APPS ||
           surface == ORB_SHELL_AGENTS || surface == ORB_SHELL_DOT || surface == ORB_SHELL_PLAY;
}

static bool cache_matches(orb_workspace_t *w, open_cache_t *c)
{
    bool static_content = w->surface == ORB_SHELL_APPS || w->surface == ORB_SHELL_PLAY;
    return !w->time_open && c->valid && c->surface == w->surface &&
           (static_content || (c->revision == w->model->revision &&
            !memcmp(&c->status, &w->status, sizeof(w->status)))) &&
           c->reduced_transparency == orb_ui_reduce_transparency() &&
           c->reduced_motion == orb_ui_reduce_motion() && c->contrast == w->contrast;
}

static void cache_clear(orb_workspace_t *w)
{
    if (w->preview) { lv_obj_delete(w->preview); w->preview = NULL; }
    w->capture = -1;
    for (unsigned i = 0; i < OPEN_CACHE_COUNT; i++) {
        lv_image_cache_drop(&w->cache[i].image);
        orb_workspace_payload_free((void *)w->cache[i].image.data);
        memset(&w->cache[i], 0, sizeof(w->cache[i]));
    }
}

static void cache_prepare(orb_workspace_t *w)
{
    /* orb_nav owns the only three screen planes. Do not retain a second cache. */
    if (w->compositor) return;
    if (!OPEN_FIXES || w->time_open || !cacheable(w->surface) || w->capture >= 0 || !w->shown) return;
    int slot = -1;
    for (unsigned i = 0; i < OPEN_CACHE_COUNT; i++) {
        if (cache_matches(w, &w->cache[i])) return;
        if (slot < 0 || !w->cache[i].image.data ||
            (w->cache[slot].image.data && w->cache[i].used < w->cache[slot].used)) slot = i;
    }
    open_cache_t *c = &w->cache[slot];
    lv_image_cache_drop(&c->image);
    /* Exactly two RGB565 planes, 868,624 payload bytes. Never retain hidden
     * LVGL trees or a third full-screen scratch plane (architecture §transitions). */
    if (!c->image.data) c->image.data = orb_workspace_payload_alloc(OPEN_PIXELS);
    if (!c->image.data) return; /* Skeleton/live rendering remains available. */
    c->image.header = (lv_image_header_t){.magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565, .w = 466, .h = 466, .stride = 932};
    c->image.data_size = OPEN_PIXELS;
    c->valid = false;
    c->surface = w->surface;
    c->status = w->status;
    c->revision = w->model->revision;
    c->used = w->open.sequence;
    c->contrast = w->contrast;
    c->reduced_motion = orb_ui_reduce_motion();
    c->reduced_transparency = orb_ui_reduce_transparency();
    w->capture = slot;
    w->capture_next_y = 0;
    /* Capture a complete settled refresh through existing internal bands.
     * No second raster pass, no full-screen PSRAM render target. */
    lv_obj_invalidate(w->root);
}

static void page_pointers_clear(orb_workspace_t *w)
{
    w->list = NULL;
    w->heading = w->preview = w->content = NULL;
    orb_notifications_host_reset(w->notifications_host);
    w->curved = false;
    w->launch_disc = NULL;
    w->notification = NULL;
    w->nav_scroll_observed = false;
}

static bool workspace_page_delete(orb_workspace_t *w)
{
    workspace_time_save(w);
    if (!orb_settings_host_detach_page(w->settings, w->page)) return false;
    orb_app_host_detach_page(w->home_host, w->page);
    orb_app_host_detach_page(w->time_host, w->page);
    orb_app_host_detach_page(w->play_host, w->page);
    orb_app_host_detach_page(w->charging_host, w->page);
    orb_app_host_detach_page(w->control_host, w->page);
    orb_app_host_detach_page(w->stack_host, w->page);
    orb_apps_view_detach(w->apps_view, w->page);
    orb_notifications_host_detach(w->notifications_host, w->page);
    orb_system_views_detach(w->system_views, w->page);
    if (!orb_agents_host_detach_page(w->agents_host, w->page)) return false;
    if (!orb_voice_host_detach_page(w->voice_host, w->page)) return false;
    (void)orb_app_faces_retain(faces_screen(w), w->root);
    orb_app_host_detach_page(w->faces_host, w->page);
    if (w->page) lv_obj_delete(w->page);
    page_pointers_clear(w);
    return true;
}

static void nav_preview_stop(orb_workspace_t *w)
{
    if (!w->nav_open_preview) return;
    w->nav_open_preview = false;
    orb_display_set_snapshot_transition(false);
    orb_nav_compositor_cancel(w->compositor);
}

static void open_refresh(lv_event_t *e)
{
    orb_workspace_t *w = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_REFR_READY && w->brightness_pending) {
        /* Keep panel/PMU work on the existing action mailbox, at most once per
         * displayed frame even when several input samples arrive together. */
        w->brightness_pending = false;
        if (w->action) w->action(ORB_SHELL_ACTION_BRIGHTNESS, w->ctx);
    }
    if (lv_event_get_code(e) == LV_EVENT_REFR_READY) orb_settings_host_flush_settings(w->settings, w->ctx);
    if (!w->shown || !w->display_active) return;
    if (lv_event_get_code(e) == LV_EVENT_RENDER_BAND && (w->nav_presenting || w->nav_open_preview)) {
        lv_display_render_band_t *band = lv_event_get_param(e);
        const lv_area_t *area = band->area;
        lv_draw_buf_t *buf = band->draw_buf;
        if (buf && !band->handled) band->handled = orb_nav_compositor_render_band(w->compositor, area->x1, area->y1,
            lv_area_get_width(area), lv_area_get_height(area), buf->data,
            buf->header.stride, buf->header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED);
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_FLUSH_START && w->nav_settle_capture) {
        const lv_area_t *area = lv_event_get_param(e);
        lv_draw_buf_t *buf = lv_display_get_buf_active(w->display);
        if (!w->nav_settle_failed && (!buf || !orb_nav_compositor_capture_band(w->compositor, 0,
            area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area),
            buf->data, buf->header.stride, buf->header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED))) {
            w->nav_settle_failed = true;
            orb_nav_compositor_invalidate(w->compositor, 0);
        }
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_FLUSH_START && w->nav_presenting) return;
    if (lv_event_get_code(e) == LV_EVENT_FLUSH_START && w->capture >= 0) {
        const lv_area_t *area = lv_event_get_param(e);
        if (area->y1 != w->capture_next_y || area->y2 >= 466 ||
            area->x1 < 0 || area->x2 >= 466) {
            w->capture = -1; return;
        }
        w->capture_next_y = area->y2 + 1;
        lv_draw_buf_t *buf = lv_display_get_buf_active(w->display);
        if (!buf || (buf->header.cf != LV_COLOR_FORMAT_RGB565 &&
                     buf->header.cf != LV_COLOR_FORMAT_RGB565_SWAPPED)) {
            w->capture = -1; return;
        }
        uint8_t *dst = (uint8_t *)w->cache[w->capture].image.data;
        const uint8_t *src = buf->data;
        size_t width = lv_area_get_width(area);
        for (int y = area->y1; y <= area->y2; y++) {
            uint8_t *row = dst + ((size_t)y * 466 + area->x1) * 2;
            if (buf->header.cf == LV_COLOR_FORMAT_RGB565) memcpy(row, src, width * 2);
            else for (size_t x = 0; x < width; x++) {
                row[x * 2] = src[x * 2 + 1]; row[x * 2 + 1] = src[x * 2];
            }
            src += buf->header.stride;
        }
    } else if (lv_event_get_code(e) == LV_EVENT_REFR_READY) {
        if (w->nav_presenting) w->nav_first_presented = true;
        if (w->nav_settle_capture) {
            w->nav_settle_capture = false;
            if (!w->nav_settle_failed && orb_nav_compositor_capture_end(w->compositor, 0)) {
                w->nav_cached[0] = w->surface;
                w->nav_apps_preview[0] &= w->surface == ORB_SHELL_APPS && !w->page_index &&
                    (!w->list || !lv_obj_get_scroll_y(w->list));
                if (w->surface == ORB_SHELL_APPS) w->nav_apps_warm = true;
            } else orb_nav_compositor_invalidate(w->compositor, 0);
            /* Retention is optional. A rejected band must finish this normal
             * open after its live frame, not restart a full redraw forever. */
            open_finish_settled(w);
        }
        if (w->capture >= 0) {
            open_cache_t *c = &w->cache[w->capture];
            if (w->heading && !orb_system_surface_uses_system_chrome(w->surface)) {
                lv_area_t area; lv_obj_get_coords(w->heading, &area);
                area.x1 = LV_MAX(0, area.x1); area.y1 = LV_MAX(0, area.y1);
                area.x2 = LV_MIN(465, area.x2); area.y2 = LV_MIN(465, area.y2);
                for (int y = area.y1; y <= area.y2; y++)
                    memset((uint8_t *)c->image.data + (y * 466 + area.x1) * 2, 0,
                           (area.x2 - area.x1 + 1) * 2);
            }
            c->valid = w->capture_next_y == 466;
            w->capture = -1;
        }
        if (w->opening && w->scaffolded) {
            w->first_presented = true;
            w->build_due = true;
            lv_timer_resume(w->open_timer);
        }
    }
}

/* Prepare while the old tree is alive: a rejected descriptor must never turn
 * the canonical page into an empty destination. Commit transfers saved context. */
static bool workspace_page_replace(orb_workspace_t *w)
{
    if (!settings_surface(w->surface)) orb_settings_host_cancel_staged(w->settings);
    lv_obj_t *page = orb_place_layer_create(w->root);
    lv_obj_set_size(page, 466, 466);
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_hidden(page, true);
    if (settings_surface(w->surface) && !orb_settings_host_commit_staged(w->settings, page)) {
        orb_settings_host_cancel_staged(w->settings);
        lv_obj_delete(page);
        return false;
    }
    if (!workspace_page_delete(w)) { lv_obj_delete(page); return false; }
    w->page = page;
    lv_obj_set_hidden(page, false);
    return true;
}

static void open_scaffold(orb_workspace_t *w)
{
    if (!workspace_page_replace(w)) return;
    if (settings_surface(w->surface))
        orb_screen_visibility(orb_settings_host_current(w->settings), ORB_VISIBILITY_COVERED);
    w->capture = -1;
    if (w->surface != ORB_SHELL_FACE && w->surface != ORB_SHELL_OFFLINE) {
        w->content = orb_place_layer_create(w->page);
        lv_obj_set_size(w->content, 466, 466);
    }
    for (unsigned i = 0; i < OPEN_CACHE_COUNT; i++) {
        if (!cache_matches(w, &w->cache[i])) continue;
        w->preview = lv_image_create(w->content);
        lv_image_set_src(w->preview, &w->cache[i].image);
        lv_obj_set_clickable(w->preview, false);
        w->cache[i].used = w->open.sequence;
        break;
    }
    /* HIG §§1/6/7: a destination heading and Back respond before any list,
     * font read, row cache or service result. No invented loading progress. */
    if (w->surface == ORB_SHELL_FACE || w->surface == ORB_SHELL_OFFLINE) {
        /* Home has no list/source preparation. Its existing handful of labels
         * is the cheap first frame, including the always-visible Apps control. */
        face(w);
        w->heading = orb_app_faces_root(faces_screen(w));
    } else if (w->time_open) {
        /* Time's cheap input receipt follows the same uncluttered dial as
         * its final view, including during the Apps iris. */
        w->heading = orb_kit_page_label(w->page, w->time.alarm_ringing ? "Alarm" : "Timer",
            ORB_TEXT_CAPTION1, true, 233, 64, 260, 1, ORB_LABEL);
    } else header(w, orb_system_surface_title(w->surface));
    lv_obj_update_layout(w->page);
    if (w->heading) lv_obj_get_coords(w->heading, &w->open.feedback_area);
    w->scaffolded = true;
    w->first_presented = w->build_due = false;
    lv_obj_invalidate(w->root);
    animate_page(w);
    if (w->surface == ORB_SHELL_APPS && w->nav_apps_warm && w->compositor && !orb_ui_reduce_motion() && !w->reverse_launch) {
        for (unsigned slot = 0; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++) {
            if (w->nav_cached[slot] != ORB_SHELL_APPS || w->nav_apps_preview[slot] ||
                !orb_nav_compositor_valid(w->compositor, slot, w->nav_revision)) continue;
            /* Reuse the retained arrival plane while the Apps tree builds.
             * Reverse and Reduce Motion keep their established final poses. */
            if (orb_nav_compositor_begin(w->compositor, ORB_NAV_TRANSITION_BOUNDARY,
                slot, slot, ORB_NAV_DIRECTION_LEFT, NULL, false, false)) {
                w->nav_open_preview = true;
                orb_display_set_snapshot_transition(true);
            }
            break;
        }
    }
}

static void open_finish_settled(orb_workspace_t *w)
{
    if (w->compositor && !orb_ui_reduce_motion() &&
        orb_nav_compositor_valid(w->compositor, 0, w->nav_revision)) {
        orb_nav_back_entry_t previous;
        if (orb_nav_back_peek(&w->nav, &previous) && previous.id != w->surface) {
            for (unsigned slot = 1; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++) {
                if (w->nav_cached[slot] != previous.id ||
                    !orb_nav_compositor_valid(w->compositor, slot, w->nav_revision)) continue;
                orb_nav_token_rect_t token = {previous.token.x, previous.token.y,
                    previous.token.width, previous.token.height};
                /* Settle prepares the retained return's dim/scale in the
                 * existing spare plane. Back admission then only reads it;
                 * its first response never pays a full PSRAM preparation. */
                if (orb_nav_compositor_begin(w->compositor,
                    previous.iris ? ORB_NAV_TRANSITION_IRIS : ORB_NAV_TRANSITION_BACK,
                    0, slot, ORB_NAV_DIRECTION_RIGHT, &token, previous.iris, false)) {
                    unsigned spare = 1;
                    while (spare == slot) spare++;
                    w->nav_apps_preview[spare] = false;
                    orb_nav_compositor_cancel(w->compositor);
                }
                break;
            }
        }
    }
    nav_arrived(w, !w->reverse_launch && !w->restore_place);
    cache_prepare(w);
    if (w->nav_timer) lv_timer_resume(w->nav_timer);
}

static void open_settled(orb_workspace_t *w)
{
    if (!w->open.active || !w->open.content_ready) return;
    uint32_t duration = orb_ui_reduce_motion() ? 150 : 550;
    if (open_now() - w->open.input_us < (int64_t)duration * 1000) return;
    if (w->content) {
        lv_obj_set_style_transform_scale(w->content, 256, 0);
        lv_obj_set_style_opa(w->content, LV_OPA_COVER, 0);
    }
    if (w->launch_disc) { lv_obj_delete(w->launch_disc); w->launch_disc = NULL; }
    bool restored_apps = w->surface == ORB_SHELL_APPS && w->nav_open_preview;
    nav_preview_stop(w);
    if (w->nav_settle_capture) return;
    bool apps_arrival = w->surface == ORB_SHELL_APPS &&
        orb_apps_arrival_prepare(orb_apps_view_current(w->apps_view), !w->reverse_launch && !w->restore_place && !restored_apps);
    lv_obj_invalidate(w->page);
    if (w->compositor && orb_nav_compositor_capture_begin(w->compositor, 0, w->nav_revision)) {
        /* This is the normal final settled redraw, not another off-screen
         * rasterization. Retain its internal bands before declaring the open
         * settled, so the next Back never races an idle source capture. */
        w->nav_settle_capture = true;
        w->nav_settle_failed = false;
        w->nav_apps_preview[0] = apps_arrival;
        lv_obj_invalidate(w->root);
    } else open_finish_settled(w);
}

static void open_tick(lv_timer_t *timer)
{
    orb_workspace_t *w = lv_timer_get_user_data(timer);
    if (!w->shown || !w->display_active) { lv_timer_pause(timer); return; }
    if (w->render_retry) {
        w->render_retry = false;
        render_now(w);
    }
    if (!w->page) { lv_timer_pause(timer); return; }
    uint32_t elapsed = (uint32_t)((open_now() - w->open.input_us) / 1000);
    if (w->open.active) {
        launch_frame(w, elapsed);
        if (orb_ui_reduce_motion() && w->content) {
            lv_obj_set_style_transform_scale(w->content, 256, 0);
            lv_obj_set_style_opa(w->content, LV_MIN(255, elapsed * 255 / 150), 0);
        } else if (w->launch_pending && w->content) {
            /* HIG §6: .5s, zero bounce, 240Hz substeps. Retargeting navigation
             * keeps velocity; elapsed time rather than frame count drives it. */
            float remaining = (LV_MIN(elapsed, 500) - w->open_elapsed) / 1000.f;
            const float omega = 2.f * (float)M_PI / .5f;
            while (remaining > 0) {
                float dt = LV_MIN(remaining, 1.f / 240.f);
                w->open_velocity += (omega * omega * (256.f - w->open_position) -
                                    2.f * omega * w->open_velocity) * dt;
                w->open_position += w->open_velocity * dt;
                remaining -= dt;
            }
            w->open_elapsed = LV_MIN(elapsed, 500);
            lv_obj_set_style_transform_scale(w->content, elapsed >= 500 ? 256 : (int)w->open_position, 0);
        }
    }
    if (w->opening && w->first_presented && w->build_due) {
        w->build_due = false;
        render_now(w);
    }
    open_settled(w);
    if (!w->opening && !w->open.active && !w->render_retry) lv_timer_pause(timer);
}

/* Never delete the page from an input/observer event. LVGL may still be
 * walking an event list owned by a child or a bubbling ancestor. Queue one
 * UI-thread rebuild after that dispatch unwinds (also coalesces model bursts). */
static void render_async(void *ctx)
{
    orb_workspace_t *w = ctx;
    w->render_pending = false;
    w->render_retry = false;
    render_now(w);
}
static void render(orb_workspace_t *w)
{
    if (!w) return;
    nav_preview_stop(w);
    if (w->nav_presenting || w->nav_pending) {
        if (!w->nav_applying && !w->nav_benchmark) {
            /* A released Back is committed even before async admission. A
             * model update in that gap used to cancel it, leaving a receipt
             * for the destination with content_ready permanently false.
             * Materialize the committed place before retiring the transition;
             * pending admission has no destination snapshot to swap. */
            if (!w->nav_live && w->nav_back && w->nav_released && w->nav_commit)
                nav_build_live(w);
            if (w->nav_live) nav_accept(w);
            else nav_cancel(w);
        }
        else if (!w->nav_applying) return;
    }
    /* Routing changes the live tree, not the model/font/status epoch. Keeping
     * that epoch lets Back reuse the plane retained on the forward journey. */
    if (!w->nav_routing) w->nav_revision++;
    /* A model/status/font revision must never finish an obsolete partial
     * tree. Continue the same input receipt and spring, with fresh content. */
    if (w->opening) {
        w->scaffolded = false;
    }
    if (w->render_pending) return;
    w->render_pending = true;
    if (lv_async_call(render_async, w) != LV_RESULT_OK) {
        /* The persistent timer needs no new allocation and runs after event
         * dispatch, so OOM cannot strand navigation or delete its input target. */
        w->render_pending = false;
        w->render_retry = true;
        lv_timer_resume(w->open_timer);
    }
}

/* Home owns its local hierarchy. Reuse the shell's admitted snapshot planes
 * and iris vocabulary without adding a shell place or another compositor. */
static void home_transition(void *ctx, const lv_area_t *token, bool reverse, bool before)
{
    orb_workspace_t *w = ctx;
    /* A service alert/lock can arrive between the source capture and Home's
     * deferred rebuild. Hidden content must not start a new presentation. */
    if (!w->shown || !w->display_active || w->surface != ORB_SHELL_HOME) {
        w->nav_local_object = false;
        return;
    }
    if (before) {
        /* A tap can select an already revealed Home token before the inbound
         * Face sheet/slide has settled. Keep that destination and its input
         * tree; cancelling here would restore Face beneath the callback. */
        if (w->nav_presenting && w->nav_live) nav_accept(w);
        else if (w->nav_pending || w->nav_presenting || w->nav_fallback) nav_cancel(w);
        nav_preview_stop(w);
        w->nav_settle_capture = false;
        w->nav_local_object = nav_capture(w, ORB_SHELL_HOME, 0);
        return;
    }
    if (!w->nav_local_object) return;
    w->nav_token = (orb_nav_token_rect_t){token->x1, token->y1,
        token->x2 - token->x1 + 1, token->y2 - token->y1 + 1};
    if (!nav_capture(w, ORB_SHELL_HOME, 1) ||
        !orb_nav_compositor_begin(w->compositor, ORB_NAV_TRANSITION_IRIS, 0, 1,
            reverse ? ORB_NAV_DIRECTION_RIGHT : ORB_NAV_DIRECTION_LEFT,
            &w->nav_token, reverse, orb_ui_reduce_motion())) {
        w->nav_local_object = false;
        orb_kit_arrival_complete(w->page, !reverse);
        return;
    }
    w->nav_source = w->nav_target = ORB_SHELL_HOME;
    w->nav_origin_state = w->nav;
    w->nav_kind = ORB_NAV_TRANSITION_IRIS;
    w->nav_direction = reverse ? ORB_NAV_DIRECTION_RIGHT : ORB_NAV_DIRECTION_LEFT;
    w->nav_back = w->nav_reverse = reverse;
    w->nav_live = w->nav_presenting = w->nav_released = w->nav_commit = true;
    w->nav_boundary = w->nav_first_presented = false;
    w->nav_live_x = lv_obj_get_style_translate_x(w->page, 0);
    w->nav_live_y = lv_obj_get_style_translate_y(w->page, 0);
    w->nav_to = 1;
    w->nav_last_drag = 0;
    w->nav_reverse_prefix = orb_nav_compositor_reverse_iris_round_start(w->compositor);
    w->open = (orb_shell_open_metrics_t){.sequence = w->open.sequence + 1,
        .surface = ORB_SHELL_HOME, .active = true, .content_ready = true, .input_us = open_now()};
    w->nav_overlay = orb_place_layer_create(w->root);
    lv_obj_set_size(w->nav_overlay, 466, 466);
    lv_obj_set_style_bg_color(w->nav_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(w->nav_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(w->page, LV_OPA_TRANSP, 0);
    orb_display_set_snapshot_transition(true);
    orb_nav_compositor_drag(w->compositor, w->nav_reverse_prefix);
    orb_nav_compositor_release(w->compositor, true, 0);
    w->nav_at = lv_tick_get();
    lv_timer_resume(w->nav_timer);
    lv_obj_invalidate(w->root);
}
static void home_page(orb_workspace_t *w)
{
    if (w->heading) { lv_obj_delete(w->heading); w->heading = NULL; }
    /* The app owns its Rooms entry and nested Back stack (Design 1.0).
     * Stack/Face favourites remain shortcuts, never Home's entry surface. */
    if (!w->home_host) w->home_host = orb_home_host_create(w->fixture, &(orb_home_host_shell_t){
        .transition = home_transition, .nav = &w->nav, .input = &w->nav_input, .context = w});
    if (orb_home_host_attach(w->home_host, w->page, &(orb_app_home_model_t){.hour = orb_system_home_hour(w->status.clock_time),
        .contrast = w->contrast, .system_navigation = !w->fixture, .fixture = w->fixture ? w->home : NULL})) {
        orb_home_host_enter(w->home_host, &w->home_fresh_entry);
        if (w->stack_scenes) orb_app_home_show_scenes(home_screen(w));
        w->stack_scenes = false;
    }
}

static void home_status_refresh(orb_workspace_t *w)
{
    if (faces_screen(w)) { face_snapshot(w); return; }
    if (w->surface != ORB_SHELL_CONTROL_CENTER) return;
    if (w->page_index != 1) return;
    /* Identity, scene, reachability and pending controls are one snapshot.
     * Updating only the glyph/state left old names and old action labels. */
    render(w);
}
static bool workspace_command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    orb_workspace_t *w = context;
    if (intent->kind != ORB_INTENT_APP_COMMAND || (intent->value.command.target != ORB_SHELL_STACK &&
        intent->value.command.target != ORB_SHELL_NOTIFICATIONS && intent->value.command.target != ORB_SHELL_NOTIFICATION_DETAIL) ||
        w->fixture || w->suppress_click || w->opening) return false;
    unsigned action = intent->value.command.action;
    clicked(w, action, NULL, &intent->launch);
    return true;
}
static void stack_refresh(orb_workspace_t *w, orb_stack_refresh_t refresh)
{
    if (!orb_stack_screen_refresh(orb_app_host_current(w->stack_host), refresh)) render(w);
}
static void stack_page(orb_workspace_t *w)
{
    if (w->heading) { lv_obj_delete(w->heading); w->heading = NULL; }
    if (!w->stack_host) w->stack_host = orb_stack_host_create(w->fixture, workspace_command, w);
    if (orb_stack_host_attach(w->stack_host, w->page, &(orb_stack_source_t){.read = stack_read, .context = w})) {
        orb_screen_instance_t *instance = orb_app_host_current(w->stack_host);
        w->heading = orb_screen_page_title(instance); w->list = orb_screen_page_viewport(instance);
        if (w->list) w->curved = false;
    }
}

static void control_value_set(orb_workspace_t *w, int percent)
{
    percent = LV_CLAMP(0, percent, 100);
    if (w->control_volume) orb_sound_set_volume(percent);
    else if (percent != w->status.brightness) {
        w->status.brightness = percent;
        w->brightness_pending = true;
    }
    orb_control_value(orb_app_host_current(w->control_host), w->control_volume, percent);
}

static bool control_command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    orb_workspace_t *w = context;
    if (intent->kind == ORB_INTENT_SETTING_SET) {
        if (intent->value.setting.key != ORB_CONTROL_BRIGHTNESS && intent->value.setting.key != ORB_CONTROL_VOLUME)
            return false;
        /* Direct level callbacks never used the tap/opening guard. Keep the
         * service write before the rotary anchor, without a second detent cue. */
        control_value_set(w, intent->value.setting.value);
        if (intent->origin == ORB_INPUT_TOUCH) orb_nav_rotary_set(&w->nav, intent->value.setting.value);
        return true;
    }
    if (intent->kind != ORB_INTENT_APP_COMMAND || intent->value.command.target != ORB_SHELL_CONTROL_CENTER ||
        w->fixture || w->suppress_click || w->opening) return false;
    unsigned action = intent->value.command.action;
    if (action == ORB_SHELL_VOICE || action == ORB_SHELL_SETTINGS || action == ORB_SHELL_HOME)
        orb_control_prepare_launch(instance, action);
    clicked(w, action, NULL, &intent->launch);
    return true;
}

#include "orb_nowplaying.inc"

static void charging(orb_workspace_t *w)
{
    workspace_chrome_clear(w);
    if (!w->charging_host) w->charging_host = orb_charging_host_create(w->fixture);
    orb_charging_model_t model = orb_charging_project(&w->status, w->contrast);
    if (orb_charging_host_attach(w->charging_host, w->page, &model))
        w->heading = orb_screen_page_title(orb_app_host_current(w->charging_host));
}

static void control_center(orb_workspace_t *w)
{
    if (w->heading) { lv_obj_delete(w->heading); w->heading = NULL; }
    if (!w->control_host) w->control_host = orb_control_host_create(w->fixture, control_command, w);
    orb_control_model_t model = orb_control_project(&w->status, orb_sound_volume(), w->silent, w->ctx && orb_shell_quiet(w->ctx), w->flashlight, w->control_volume);
    if (w->page_index == 1) {
        orb_control_home_project(&model, w->home, orb_system_home_hour(w->status.clock_time));
        model.home.wait_started = orb_control_home_wait(&w->control_home_loading, &w->control_home_wait_at, w->home);
        snprintf(w->home_scene_id, sizeof(w->home_scene_id), "%s", model.home.has_scene ? model.home.scene.id : "");
    }
    if (orb_control_host_attach(w->control_host, w->page, &model))
        w->heading = orb_screen_page_title(orb_app_host_current(w->control_host));
}

static bool render_compose(orb_workspace_t *w, int previous_scroll, bool control_open)
{
    bool complete = true;
    if (w->surface == ORB_SHELL_STACK)
        w->page_index = LV_MIN(w->page_index, LV_MAX(1u, stack_count(w)) - 1);
    switch (w->surface) {
    case ORB_SHELL_FACE:
    case ORB_SHELL_OFFLINE: if (!w->opening) face(w); break;
    case ORB_SHELL_APPS: complete = apps(w); break;
    case ORB_SHELL_AGENTS: agents_adapter(w); break;
    case ORB_SHELL_SESSIONS: agents_adapter(w); break;
    case ORB_SHELL_USAGE: agents_adapter(w); break;
    case ORB_SHELL_IDLE_SESSIONS: agents_adapter(w); break;
    case ORB_SHELL_SESSION_SETTINGS: settings_adapter(w); break;
    case ORB_SHELL_UNITS: settings_adapter(w); break;
    case ORB_SHELL_TEMPERATURE: settings_adapter(w); break;
    case ORB_SHELL_DOT: agents_adapter(w); break;
    case ORB_SHELL_SESSION: agents_adapter(w); break;
    case ORB_SHELL_STACK: stack_page(w); break;
    case ORB_SHELL_CARD: card(w); break;
    case ORB_SHELL_HOME: home_page(w); break;
    case ORB_SHELL_NOTIFICATIONS: notifications(w); break;
    case ORB_SHELL_NOTIFICATION_DETAIL: notifications(w); break;
    case ORB_SHELL_CHARGING: charging(w); break;
    case ORB_SHELL_COMPASS: nav_compass_show(w); break;
    case ORB_SHELL_SETTINGS: settings_adapter(w); break;
    case ORB_SHELL_CONTROL_CENTER: control_center(w); break;
    case ORB_SHELL_DISPLAY: settings_adapter(w); break;
    case ORB_SHELL_ACCESSIBILITY: settings_adapter(w); break;
    case ORB_SHELL_SOUND: settings_adapter(w); break;
    case ORB_SHELL_BATTERY: case ORB_SHELL_WEATHER: settings_adapter(w); break;
    case ORB_SHELL_ONBOARDING: system_view(w); break;
    case ORB_SHELL_ABOUT: settings_adapter(w); break;
    case ORB_SHELL_VOICE: voice_adapter(w); break;
    case ORB_SHELL_PLAY:
    case ORB_SHELL_TIME: play(w); break;
    case ORB_SHELL_UPDATES: system_view(w); break;
    }
    if ((w->opening || control_open) && w->content && w->surface != ORB_SHELL_FACE && w->surface != ORB_SHELL_OFFLINE) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(w->page);) {
            lv_obj_t *obj = lv_obj_get_child(w->page, i);
            if (obj == w->heading || obj == w->content || obj == w->launch_disc) i++;
            else lv_obj_set_parent(obj, w->content);
        }
    }
    if (w->list && (previous_scroll || w->restore_place)) {
        lv_obj_scroll_to_y(w->list, w->restore_place ? w->restore_scroll : previous_scroll, LV_ANIM_OFF);
        orb_round_list_update(w->list);
    }
    if (!w->opening || complete) w->restore_place = false;
    /* Scoped to shell consumers: orb_ui owns the material renderer and exposes
     * no contrast setter. Rebuilding from normal styles makes turning this off
     * restore the original palette exactly, without modifying shared styles. */
    if (w->contrast) orb_place_increase_contrast(w->page);
    if (w->new_card_at && lv_tick_get()-w->new_card_at < 2000 && !w->notification &&
        w->surface != ORB_SHELL_CARD && w->surface != ORB_SHELL_NOTIFICATIONS &&
        w->surface != ORB_SHELL_FACE && w->surface != ORB_SHELL_OFFLINE) {
        w->notification = agents_arrival(w); /* Review #8: labelled, replaces the bare hand. */
        if (w->notification_sound_pending) {
            orb_sound_play(ORB_SOUND_NOTIFICATION); w->notification_sound_pending = false;
        }
        delight_wake(w); /* The transient did not exist when its arrival was recorded. */
    }
    orb_place_title_lane(w->heading, w->notification);
    /* A replacement page may reuse the addresses of old transparent objects.
     * Invalidate its full extent after layout so no newly exposed navigation
     * region depends on the previous page's partial dirty rectangles. */
    lv_obj_update_layout(w->page);
    if (settings_surface(w->surface)) orb_kit_list_refresh(w->list);
    for (uint32_t i = 0; i < lv_obj_get_child_count(w->page); i++) {
        lv_obj_t *obj = lv_obj_get_child(w->page, i);
        if (lv_obj_check_type(obj, &lv_button_class) &&
            (int)(intptr_t)lv_obj_get_user_data(obj) == ACT_BACK) {
            lv_obj_move_foreground(obj); break;
        }
    }
    lv_obj_invalidate(w->root);
    w->rebuilding = false;
    return complete;
}

static void render_now(orb_workspace_t *w)
{
    /* Input dispatch must unwind before deleting its release target. */
    if (!w || w->rebuilding || orb_voice_flow_held(w->voice_flow)) return;
    if (!w->opening && !w->navigation_pending && workspace_level(w) &&
        lv_obj_has_state(workspace_level(w), LV_STATE_PRESSED)) {
        /* An unrelated arrival cannot delete the slider under a held finger.
         * Retained telemetry still updates; structural changes wait for lift. */
        w->render_retry = true;
        if (w->open_timer) lv_timer_resume(w->open_timer);
        return;
    }
    orb_round_list_cache_stats_t cache;
    if (!w->opening && !w->navigation_pending && w->shown && w->list &&
        orb_round_list_cache_stats(w->list, &cache) && cache.width_table_bytes &&
        (w->nav.touch.down || w->nav.rotary.flinging || lv_obj_is_scrolling(w->list) ||
         orb_round_list_scroll_active(w->list))) {
        /* Model membership/notification arrivals must not replace the live
         * scroll owner or prepare new row sources during its drag/throw.
         * Coalesce them on the existing timer; navigation still wins. */
        w->render_retry = true;
        if (w->open_timer) lv_timer_resume(w->open_timer);
        return;
    }
    if (!w->shown) {
        if (workspace_page_delete(w)) w->page = NULL;
        return;
    }
    if ((!w->opening || !w->scaffolded) && !settings_prepare(w, w->surface, false)) {
        w->navigation_pending = false;
        return;
    }
    int64_t started = open_now();
    if (w->opening && !w->scaffolded) {
        open_scaffold(w);
        uint64_t elapsed = open_now() - started;
        w->open.object_us += elapsed;
        w->open.max_slice_us = LV_MAX(w->open.max_slice_us, elapsed);
        w->open.build_slices++;
        return;
    }
    if (w->opening && !w->first_presented) return;
    play_sync_selection(w);
    w->rebuilding = true;
    if (w->compass) {
        lv_obj_delete(w->compass); w->compass = NULL;
    }
    bool control_open = !OPEN_FIXES && w->open.active && !w->open.content_ready;
    int previous_scroll = w->list && !w->navigation_pending && !w->opening ? lv_obj_get_scroll_y(w->list) : 0;
    if (!w->opening) {
        if (!workspace_page_replace(w)) { w->rebuilding = false; return; }
        if (control_open && w->surface != ORB_SHELL_FACE) {
            w->content = orb_place_layer_create(w->page);
            lv_obj_set_size(w->content, 466, 466);
        }
    }
    bool complete = render_compose(w, previous_scroll, control_open);
    if (w->nav_timer && !w->fixture) {
        nav_bind_rotary(w);
        lv_timer_resume(w->nav_timer);
    }
    if (w->opening) {
        uint64_t elapsed = open_now() - started;
        w->open.object_us += elapsed;
        w->open.max_slice_us = LV_MAX(w->open.max_slice_us, elapsed);
        w->open.build_slices++;
        if (complete) {
            w->opening = false;
            w->open.content_ready = true;
            nav_preview_stop(w);
            if (w->preview) { lv_obj_delete(w->preview); w->preview = NULL; }
            open_settled(w);
        } else if (w->preview) {
            lv_obj_move_foreground(w->preview);
            if (w->heading) lv_obj_move_foreground(w->heading);
        }
    } else {
        w->navigation_pending = false;
        if (control_open) {
            uint64_t elapsed = open_now() - started;
            w->open.object_us += elapsed;
            w->open.max_slice_us = elapsed;
            w->open.build_slices++;
            w->open.content_ready = true;
            lv_obj_t *feedback = w->heading;
            if (feedback) lv_obj_get_coords(feedback, &w->open.feedback_area);
            animate_page(w);
        }
    }
    /* Deferred Back/model rebuilds can create controls after the fence was consumed.
     * Delight §6: restore final ink without replay or a staged hidden pose. */
    if (!w->opening && !w->open.active && !w->nav_applying && !w->nav_presenting && !w->nav_pending && !orb_settings_host_arrive_local(w->settings))
        orb_kit_arrival_complete(w->page, false);
}

/* Navigation depth is deliberately bounded. Retarget immediately; no animation
 * may delay an approval or service interruption (HIG §6). */
static void navigate(orb_workspace_t *w, orb_shell_surface_t surface, bool push)
{
    if (!w || surface > ORB_SHELL_SURFACE_LAST) return;
    int64_t input_us = open_now();
    if (w->nav_presenting && !w->nav_applying) nav_cancel(w);
    if (!settings_prepare(w, surface, true)) return;
    if (!w->nav_speculative) orb_home_host_departure(w->home_host, surface,
        !w->fixture && !w->nav_benchmark, &w->home_fresh_entry);
    if (surface != w->surface && w->surface == ORB_SHELL_NOTIFICATION_DETAIL)
        orb_notification_history_remember_detail(w->notifications, w->card);
    if (surface != w->surface && orb_system_surface_uses_system_chrome(w->surface)) {
        orb_route_context_save(&w->places[w->surface], orb_page_save(w->list).scroll,
            w->page_index, orb_apps_view_focus(w->apps_view));
    }
    if (w->surface == ORB_SHELL_VOICE && surface != ORB_SHELL_VOICE) {
        orb_voice_flow_depart(w->voice_flow);
    }
    play_sync_selection(w);
    workspace_time_save(w);
    /* Selecting another session/card can keep the same surface enum. Its old
     * pixels must not survive the context change under a route-only epoch. */
    if (!w->nav_applying && (surface == w->surface || surface == ORB_SHELL_SESSION ||
        surface == ORB_SHELL_CARD || surface == ORB_SHELL_VOICE)) {
        for (unsigned slot = 0; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++)
            if (w->nav_cached[slot] == surface) orb_nav_compositor_invalidate(w->compositor, slot);
    }
    /* Stream F: leaving relinquishes only app admission. Timer/Alarm state
     * survives in orb_time; main consumes exit even after a queued entry. */
    if (w->time_open && surface != ORB_SHELL_TIME) {
        if (!w->nav_speculative && !w->nav_benchmark) orb_apps_request_exit();
        w->time_open = false;
    }
    w->time_open = surface == ORB_SHELL_TIME;
    bool face_launch = w->face_launch && (w->surface == ORB_SHELL_FACE || w->surface == ORB_SHELL_OFFLINE);
    bool launch = (w->surface == ORB_SHELL_APPS || face_launch) && surface != ORB_SHELL_FACE;
    if (launch && !w->time_open) w->time_launched = false;
    /* Face is enum zero, also the empty launch record: Apps from Face is always fresh. */
    w->reverse_launch = surface == ORB_SHELL_APPS && w->surface != ORB_SHELL_FACE && w->surface == w->launched_app;
    if (launch) { w->icon_origin = w->launch; w->launched_app = surface; }
    w->page_motion = false;
    if (!w->fixture && (surface == ORB_SHELL_IDLE_SESSIONS || surface == ORB_SHELL_SESSIONS ||
        surface == ORB_SHELL_DOT) && !w->idle) {
        w->idle = orb_workspace_payload_alloc(sizeof(*w->idle));
        if (!w->idle || !orb_model_idle_open()) { orb_workspace_payload_free(w->idle); w->idle = NULL; }
    }
    if (push && surface != w->surface) {
        bool face_neighbour = surface == ORB_SHELL_AGENTS || surface == ORB_SHELL_HOME;
        if (face_neighbour) {
            /* Agents and Home are Face's places, even when reached through
             * Apps or a Stack shortcut. Keep that canonical Back parent for
             * every route. Reset only history: the finger still owns an
             * interactive transition and must deliver its eventual lift. */
            w->nav.back_count = w->nav.layer_count = 0;
            orb_nav_set_current(&w->nav, ORB_SHELL_FACE);
        }
        int side = face_launch ? w->nav_token.width : launch ? ORB_KIT_DISC : 72;
        orb_nav_token_t token = {w->launch.x - side / 2, w->launch.y - side / 2, side, side};
        bool object_iris = w->nav_applying && w->nav_kind == ORB_NAV_TRANSITION_IRIS &&
            !w->nav_back && w->nav_target == surface;
        if (object_iris) token = (orb_nav_token_t){w->nav_token.x, w->nav_token.y,
            w->nav_token.width, w->nav_token.height};
        /* Polish #34: Back from a Face place launched in Apps lands on the
         * Face, so its iris closes onto the Face's Apps control, not onto a
         * launcher disc position where the Face shows nothing. */
        if (object_iris && face_neighbour && !face_launch)
            token = (orb_nav_token_t){233 - ORB_KIT_DISC_MINI / 2, 408 - ORB_KIT_DISC_MINI / 2,
                ORB_KIT_DISC_MINI, ORB_KIT_DISC_MINI};
        orb_nav_push(&w->nav, surface, object_iris || (launch && (!face_neighbour || face_launch)) ? &token : NULL);
    } else orb_nav_set_current(&w->nav, surface);
    bool was_active = w->open.active;
    float position = w->open_position, velocity = w->open_velocity;
    uint32_t sequence = w->open.sequence + 1;
    w->open = (orb_shell_open_metrics_t){.sequence = sequence, .surface = surface,
        .cold = !(w->visited & (1u << surface)), .active = true, .input_us = input_us};
    w->visited |= 1u << surface;
    w->opening = OPEN_FIXES && !w->nav_applying;
    w->scaffolded = w->first_presented = w->build_due = false;
    w->open_position = launch ? (was_active ? LV_MAX(OPEN_MIN_SCALE, position) : OPEN_MIN_SCALE) : 256.f;
    w->open_velocity = was_active ? velocity : 0;
    w->open_elapsed = 0;
    w->capture = -1;
    w->surface = surface; w->page_index = face_launch && surface == ORB_SHELL_USAGE ? w->face_usage_page : 0;
    w->face_launch = false;
    if (orb_system_surface_uses_system_chrome(w->surface)) {
        orb_route_location_t place = orb_route_context_read(&w->places[surface]);
        if (place.valid) {
            w->page_index = place.page;
            orb_apps_view_select(w->apps_view, place.focus);
            w->restore_scroll = place.scroll; w->restore_place = true;
        }
    }
    if (!push && surface == ORB_SHELL_NOTIFICATION_DETAIL && orb_notification_history_detail(w->notifications)[0])
        snprintf(w->card, sizeof(w->card), "%s", orb_notification_history_detail(w->notifications));
    if (surface == ORB_SHELL_STACK) w->card[0] = '\0';
    voice_focus(w);
    w->notice[0] = '\0'; w->text_notice_pending = false;
    w->navigation_pending = true; w->launch_pending = launch && !w->nav_applying;
    if (w->nav_applying) {
        w->open.active = false;
        w->reverse_launch = false;
    }
    w->nav_routing = true;
    render(w);
    w->nav_routing = false;
}

static void launch_frame(orb_workspace_t *w, uint32_t elapsed)
{
    if (!w->launch_disc || orb_ui_reduce_motion()) return;
    float t = LV_MIN(1.f, elapsed / 250.f);
    t = (1-(1+6.2831853f*t)*expf(-6.2831853f*t)) / (1-7.2831853f*expf(-6.2831853f));
    if (w->reverse_launch) t = 1.f - t;
    int radius = 42 + (int)(550 * t);
    int cx = w->icon_origin.x + (int)((233 - w->icon_origin.x) * t);
    int cy = w->icon_origin.y + (int)((233 - w->icon_origin.y) * t);
    lv_obj_set_size(w->launch_disc, radius * 2, radius * 2);
    lv_obj_set_pos(w->launch_disc, cx - radius, cy - radius);
    /* Transient launch disc, never a resting app background (delight §2). */
    unsigned fade = elapsed > 32 ? LV_MIN(255, (elapsed - 32) * 255 / 118) : 0;
    if (w->reverse_launch) fade = LV_MIN(255, elapsed * 255 / 250);
    lv_obj_set_style_opa(w->launch_disc, 255 - fade, 0);
    if (w->content) lv_obj_set_style_opa(w->content, LV_MIN(255, elapsed * 255 / 150), 0);
    lv_obj_invalidate(w->root);
}

static void animate_page(orb_workspace_t *w)
{
    w->navigation_pending = false;
    if (!w->page) return;
    /* Keep the heading and Back at their readable, tappable resting geometry
     * from the first frame. Only content zooms or cross-fades (HIG §§3/6/8). */
    if ((w->launch_pending || w->reverse_launch) && !orb_ui_reduce_motion()) {
        w->launch_disc = orb_place_layer_create(w->page);
        lv_obj_set_style_radius(w->launch_disc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(w->launch_disc, accent(w, w->time_launched ? ORB_DELIGHT_TEAL : orb_system_surface_accent(w->launched_app)), 0);
        lv_obj_set_style_bg_opa(w->launch_disc, LV_OPA_COVER, 0);
        lv_obj_move_foreground(w->launch_disc);
        if (w->heading) lv_obj_move_foreground(w->heading);
        launch_frame(w, 0);
    }
    if (orb_ui_reduce_motion() && w->content) {
        lv_obj_set_style_opa(w->content, 1, 0);
    } else if (w->launch_pending && w->content) {
        lv_obj_set_style_transform_pivot_x(w->content, w->launch.x, 0);
        lv_obj_set_style_transform_pivot_y(w->content, w->launch.y, 0);
        lv_obj_set_style_transform_scale(w->content, (int)w->open_position, 0);
    }
    lv_timer_reset(w->open_timer);
    lv_timer_resume(w->open_timer);
}

static void neighbour_prepare(void *context)
{
    workspace_time_save(context);
}

static orb_neighbour_t *neighbour_create(orb_workspace_t *w, orb_shell_surface_t surface)
{
    orb_neighbour_t *scene = orb_neighbour_create(lv_obj_get_parent(w->root), surface,
        &(orb_neighbour_source_t){.snapshots = &w->snapshots, .prepare = neighbour_prepare, .prepare_context = w,
            .context = surface == ORB_SHELL_HOME ? w->home_host : surface == ORB_SHELL_TIME ? w->time_host : NULL,
            .status = &w->status, .place = &w->places[surface], .silent = w->silent, .control_volume = w->control_volume,
            .apps = w->apps_view, .time = &w->time, .card = w->card, .hour = orb_system_home_hour(w->status.clock_time), .contrast = w->contrast,
            .alarm_ringing = w->time.alarm_ringing, .reminder_due = w->time.reminder_due,
            .home_fresh_entry = w->home_fresh_entry});
    if (scene && w->contrast) orb_place_increase_contrast(orb_neighbour_page(scene));
    orb_neighbour_settle(scene);
    return scene;
}

#include "orb_workspace_nav.inc"

static void fonts_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    LV_UNUSED(subject);
    orb_workspace_t *w = lv_observer_get_user_data(observer);
    if (w->text_notice_pending) snprintf(w->notice, sizeof(w->notice), "%s", orb_fonts_last_result() == ORB_FONTS_OK ? "" : "Not saved");
    cache_clear(w); w->text_notice_pending = false;
    if (w->opening) w->scaffolded = false;
    render(w);
}

static void delight_wake(orb_workspace_t *w)
{
    if (!w->delight_timer || !w->shown || !w->display_active) {
        if (w->delight_timer) lv_timer_pause(w->delight_timer);
        return;
    }
    bool transient = (w->surface == ORB_SHELL_FACE && w->all_clear) ||
                     w->page_motion || w->arrival ||
                     w->notification;
    if (!transient) { lv_timer_pause(w->delight_timer); return; }
    lv_timer_set_period(w->delight_timer, 16);
    lv_timer_resume(w->delight_timer);
}

static void delight_tick(lv_timer_t *timer)
{
    orb_workspace_t *w = lv_timer_get_user_data(timer);
    if (!w->shown || !w->page || !w->display_active) { lv_timer_pause(timer); return; }
    uint32_t now = lv_tick_get();
    float dt = w->delight_at ? (now - w->delight_at) / 1000.f : .016f;
    w->delight_at = now;
    bool active = false, rm = orb_ui_reduce_motion();
    if (w->surface == ORB_SHELL_FACE) {
        if (w->all_clear && now - w->clear_at >= 2000) w->all_clear = false;
        active |= w->all_clear;
    }
    if (w->page_motion || w->arrival) {
        if (w->arrival) {
            orb_delight_spring_reset(&w->page_spring, 28); orb_delight_spring_target(&w->page_spring, 0);
            w->arrival = false; w->page_motion = true; w->page_at = w->new_card_at;
        }
        orb_delight_spring_step(&w->page_spring, dt, .5f, rm ? 0 : .15f);
        if (w->list) lv_obj_set_style_translate_y(w->list, rm ? 0 : (int)w->page_spring.value, 0);
        else if (w->content) lv_obj_set_style_translate_y(w->content, rm ? 0 : (int)w->page_spring.value, 0);
        /* Review #10: the Stack adds its list after frame one; never strand that offset. */
        if (w->list && w->content) lv_obj_set_style_translate_y(w->content, 0, 0);
        lv_obj_t *moving = w->list ? w->list : w->content;
        if (rm && moving) lv_obj_set_style_opa(moving, (lv_opa_t)(255 * orb_delight_crossfade(now-w->page_at)), 0);

        if (orb_delight_spring_settled(&w->page_spring)) w->page_motion = false; else active = true;
    }
    if (w->notification) {
        if (now-w->new_card_at >= 2000) {
            lv_obj_delete(w->notification); w->notification = NULL;
            if (w->heading) lv_obj_set_hidden(w->heading, false);
            lv_obj_invalidate(w->root);
        }
        else active = true;
    }
    if (active) lv_timer_set_period(timer, 16);
    else lv_timer_pause(timer);
}

void orb_workspace_display_active(orb_workspace_t *w, bool active)
{
    if (!w) return;
    orb_power_snapshot_t power = orb_shell_power_snapshot(w->ctx);
    orb_sound_set_muted(w->silent, w->face_down || (power.revision && (power.silent || power.redact)));
    if (w->display_active == active) return;
    w->display_active = active;
    orb_faces_host_motion(w->root, faces_screen(w), active && !w->always_on && !w->face_down, w->shown && !w->covered, !active);
    if (!active) {
        nav_suspend(w);
        if (w->open_timer) lv_timer_pause(w->open_timer);
        orb_kit_material_finish(w->page);
    } else if (w->open_timer && (w->opening || w->open.active || w->render_retry)) {
        lv_timer_resume(w->open_timer);
        if (w->opening && !w->first_presented) lv_obj_invalidate(w->root);
    }
    delight_wake(w);
    /* Deliberate wake clears Quiet. Refresh its retained preference value even
     * when owner brightness and the ordinary status snapshot did not change. */
    if (active && w->shown && (w->surface == ORB_SHELL_DISPLAY || w->surface == ORB_SHELL_CONTROL_CENTER))
        render(w);
}

void orb_workspace_covered(orb_workspace_t *w, bool covered)
{
    if (!w || w->covered == covered) return;
    w->covered = covered;
    /* HIG §6: a modal retires presentation, not the retained task. Its return
     * resumes only a named wait's remaining budget, never a fresh arrival. */
    orb_kit_motion_set_active(w->root, w->shown && w->display_active && !w->always_on && !w->face_down && !covered);
}

void orb_workspace_environment(orb_workspace_t *w, float x, float y, float z, bool always_on)
{
    if (!w || !isfinite(x) || !isfinite(y) || !isfinite(z)) return;
    bool idle_changed = w->always_on != always_on;
    w->face_down = z < -.5f; w->always_on = always_on;
    orb_faces_host_motion(w->root, faces_screen(w), w->display_active && !always_on && !w->face_down, w->shown && !w->covered, false);
    orb_power_snapshot_t power = orb_shell_power_snapshot(w->ctx);
    orb_sound_set_muted(w->silent, w->face_down || (power.revision && (power.silent || power.redact)));
    if (idle_changed) orb_sound_set_always_on(always_on);
    delight_wake(w);
}

static void workspace_payload_release(orb_workspace_t *w)
{
    orb_apps_view_destroy(w->apps_view);
    orb_apps_view_destroy(w->apps_bench);
    orb_voice_flow_destroy(w->voice_flow);
    orb_notifications_host_destroy(w->notifications_host);
    orb_notification_history_destroy(w->notifications);
    orb_workspace_snapshots_release(&w->snapshots);
    orb_workspace_payload_free(w);
}

static void deleted(lv_event_t *e)
{
    orb_workspace_t *w = lv_event_get_user_data(e);
    /* A live workspace can be torn down mid-drag. Retire its no-blur
     * certificate before removing the compositor's flush callback. */
    if (w->nav_presenting || w->nav_open_preview) orb_display_set_snapshot_transition(false);
    lv_async_call_cancel(render_async, w);
    lv_display_remove_event_cb_with_user_data(w->display, open_refresh, w);
    if (w->open_timer) lv_timer_delete(w->open_timer);
    if (w->delight_timer) lv_timer_delete(w->delight_timer);
    if (w->nav_timer) lv_timer_delete(w->nav_timer);
    lv_async_call_cancel(nav_prepare_async, w);
    if (w->compass) lv_obj_delete(w->compass); /* Its delete callback borrows w. */
    orb_nav_compositor_destroy(w->compositor);
    cache_clear(w);
    if (w->idle) {
        if (!w->fixture) orb_model_idle_close();
        orb_workspace_payload_free(w->idle);
    }
    orb_settings_host_destroy(w->settings);
    orb_settings_host_destroy(w->settings_bench);
    orb_app_host_destroy(w->faces_host);
    orb_app_host_destroy(w->home_host);
    orb_app_host_destroy(w->home_bench);
    orb_app_host_destroy(w->time_host);
    orb_app_host_destroy(w->time_bench);
    orb_app_host_destroy(w->play_host);
    orb_app_host_destroy(w->play_bench);
    orb_app_host_destroy(w->charging_host);
    orb_app_host_destroy(w->control_host);
    orb_app_host_destroy(w->stack_host);
    orb_system_views_destroy(w->system_views);
    orb_agents_host_destroy(w->agents_host);
    orb_agents_host_destroy(w->agents_bench);
    orb_voice_host_destroy(w->voice_host);
    workspace_payload_release(w);
}

static orb_workspace_t *workspace_create(lv_obj_t *parent, orb_shell_action_cb_t action,
                                         orb_workspace_service_cb_t service, void *ctx,
                                         const orb_model_snapshot_t *model,
                                         const orb_home_snapshot_t *home, const orb_workspace_t *source)
{
    if (!orb_model_init()) return NULL;
    /* Recent receipts include exact option IDs/labels after card removal.
     * Keep their bounded storage out of internal RAM (HIG content lifetime). */
    orb_workspace_t *w = orb_workspace_payload_alloc(sizeof(*w));
    if (!w) return NULL;
    orb_time_snapshot(orb_time_now_us(), &w->time);
    w->fixture = source || (model && home);
    w->home_fresh_entry = source && source->home_fresh_entry;
    if (!orb_workspace_snapshots_init(&w->snapshots, source ? &source->snapshots : NULL, model, home)) {
        orb_workspace_payload_free(w); return NULL;
    }
    w->apps_view = orb_apps_view_create(w->fixture, apps_command, apps_active, w);
    w->voice_flow = orb_voice_flow_create();
    w->notifications = orb_notification_history_create();
    w->notifications_host = orb_notifications_host_create(w->fixture);
    if (!w->apps_view || !w->voice_flow || !w->notifications || !w->notifications_host) { workspace_payload_release(w); return NULL; }
    w->model = &w->snapshots.model->value;
    w->home = &w->snapshots.home->value;
    w->action = action; w->service = service; w->ctx = ctx;
    orb_nav_init(&w->nav, ORB_SHELL_FACE);
    for (unsigned i = 0; i <= ORB_SHELL_SURFACE_LAST; i++) {
        orb_nav_role_t role = i == ORB_SHELL_FACE ? ORB_NAV_ROLE_FACE :
            i == ORB_SHELL_AGENTS ? ORB_NAV_ROLE_AGENTS : i == ORB_SHELL_HOME ? ORB_NAV_ROLE_HOME :
            i == ORB_SHELL_STACK ? ORB_NAV_ROLE_STACK : i == ORB_SHELL_NOTIFICATIONS ? ORB_NAV_ROLE_NOTIFICATIONS :
            i == ORB_SHELL_CONTROL_CENTER ? ORB_NAV_ROLE_CONTROL : ORB_NAV_ROLE_SCREEN;
        orb_nav_register(&w->nav, (orb_nav_place_t){i, role, i != ORB_SHELL_FACE});
    }

    w->launch = (lv_point_t){233, 233};
    orb_sound_init();
    w->delight_timer = lv_timer_create(delight_tick, 16, w);
    if (!w->delight_timer) { workspace_payload_release(w); return NULL; }
    lv_timer_pause(w->delight_timer);
    w->capture = -1;
    w->visited = 1u << ORB_SHELL_FACE;
    w->display = lv_obj_get_display(parent);
    w->open_timer = lv_timer_create(open_tick, 1, w);
    if (!w->open_timer) { lv_timer_delete(w->delight_timer); workspace_payload_release(w); return NULL; }
    lv_timer_pause(w->open_timer);
    lv_event_dsc_t *flush = lv_display_add_event_cb(w->display, open_refresh, LV_EVENT_FLUSH_START, w);
    lv_event_dsc_t *ready = lv_display_add_event_cb(w->display, open_refresh, LV_EVENT_REFR_READY, w);
    lv_event_dsc_t *band = lv_display_add_event_cb(w->display, open_refresh, LV_EVENT_RENDER_BAND, w);
    if (!flush || !ready || !band) {
        lv_display_remove_event_cb_with_user_data(w->display, open_refresh, w);
        lv_timer_delete(w->open_timer);
        lv_timer_delete(w->delight_timer);
        workspace_payload_release(w);
        return NULL;
    }
    w->root = orb_place_layer_create(parent); lv_obj_set_size(w->root, 466, 466);
    lv_obj_set_clickable(w->root, true);
    lv_obj_add_event_cb(w->root, gesture, LV_EVENT_PRESSED, w);
    lv_obj_add_event_cb(w->root, gesture, LV_EVENT_RELEASED, w);
    lv_obj_add_event_cb(w->root, gesture, LV_EVENT_PRESSING, w);
    lv_obj_add_event_cb(w->root, gesture, LV_EVENT_PRESS_LOST, w);
    if (!w->fixture) {
        w->compositor = orb_nav_compositor_create(466, 466);
        w->nav_timer = lv_timer_create(nav_tick, 16, w);
        if (w->nav_timer) lv_timer_pause(w->nav_timer);
        if (!w->nav_timer) { orb_nav_compositor_destroy(w->compositor); w->compositor = NULL; }
        for (unsigned i = 0; i < ORB_NAV_SNAPSHOT_SLOTS; i++) w->nav_cached[i] = ORB_SHELL_UPDATES;
    }
    lv_obj_add_event_cb(w->root, deleted, LV_EVENT_DELETE, w);
    lv_subject_add_observer_obj(orb_fonts_subject(), fonts_changed, w->root, w);
    w->shown = w->display_active = true;
    if (!w->fixture) w->snapshots.model->value.revision = UINT64_MAX;
    orb_workspace_poll(w);
    render(w);
    return w;
}

orb_workspace_t *orb_workspace_create(lv_obj_t *parent, orb_shell_action_cb_t action,
                                     orb_workspace_service_cb_t service, void *ctx)
{
    return workspace_create(parent, action, service, ctx, NULL, NULL, NULL);
}

/* Explicit private model: a developer capture cannot replace the live link
 * store, change its voice focus, or queue actions through a synthetic row.
 * External fixture inputs may be stack/temporary storage, so keep copying. */
orb_workspace_t *orb_workspace_create_fixture(lv_obj_t *parent, const orb_model_snapshot_t *model,
                                             const orb_home_snapshot_t *home)
{
    if (!model || !home) return NULL;
    orb_workspace_t *w = workspace_create(parent, NULL, NULL, NULL, model, home, NULL);
    if (!w) return NULL;
    orb_notification_history_seed(w->notifications, model);
    return w;
}

/* Navigation captures are synchronous today, but retaining each generation
 * also makes a delayed preview safe across live polling or source teardown. */
static orb_workspace_t *workspace_create_preview(lv_obj_t *parent, const orb_workspace_t *source)
{
    if (!source) return NULL;
    return workspace_create(parent, NULL, NULL, NULL, NULL, NULL, source);
}

void orb_workspace_fixture_notification_age(orb_workspace_t *w, const char *id, unsigned age_ms)
{
    if (!w || !id) return;
    orb_notification_history_fixture_age(w->notifications, id, true, age_ms, lv_tick_get());
    if (w->surface == ORB_SHELL_NOTIFICATIONS || w->surface == ORB_SHELL_NOTIFICATION_DETAIL) render(w);
}

lv_obj_t *orb_workspace_fixture_scroll(orb_workspace_t *w)
{
    if (!w || !w->fixture) return NULL;
    return home_screen(w) ? orb_app_home_scroll(home_screen(w)) : w->list;
}

orb_screen_instance_t *orb_workspace_fixture_home(const orb_workspace_t *w)
{ return w && w->fixture ? home_screen(w) : NULL; }

void orb_workspace_destroy(orb_workspace_t *w) { if (w) lv_obj_delete(w->root); }
void orb_workspace_show(orb_workspace_t *w, bool show)
{
    if (!w) return;
    if (!show && orb_voice_flow_held(w->voice_flow)) {
        orb_voice_flow_touch(w->voice_flow, false);
        voice_command(w, ORB_VOICE_STOP);
    }
    bool changed = w->shown != show;
    if (changed && !show && w->idle) {
        orb_model_idle_close(); orb_workspace_payload_free(w->idle); w->idle = NULL;
    } else if (changed && show && (w->surface == ORB_SHELL_IDLE_SESSIONS || w->surface == ORB_SHELL_SESSIONS)) {
        w->idle = orb_workspace_payload_alloc(sizeof(*w->idle));
        if (!w->idle || !orb_model_idle_open()) { orb_workspace_payload_free(w->idle); w->idle = NULL; }
    }
    w->shown = show; lv_obj_set_hidden(w->root, !show);
    orb_kit_motion_set_active(w->root, show && w->display_active && !w->always_on && !w->face_down && !w->covered);
    if (changed && !show) {
        nav_cancel(w);
        w->opening = w->open.active = false;
        w->capture = -1;
        lv_timer_pause(w->open_timer);
        lv_timer_pause(w->delight_timer);
    }
    if (changed) render(w);
}

void orb_workspace_update(orb_workspace_t *w, const orb_shell_snapshot_t *snapshot)
{
    if (!w || !snapshot) return;
    uint32_t previous_stack = w->surface == ORB_SHELL_STACK ? stack_signature(w) : 0;
    orb_shell_snapshot_t old = w->status;
    w->status = *snapshot;
    bool updates_changed = orb_system_views_update(w->system_views, snapshot, &old);
    if (w->surface == ORB_SHELL_UPDATES && updates_changed) render(w);
    if (w->brightness_pending ||
        (workspace_level(w) && lv_obj_has_state(workspace_level(w), LV_STATE_PRESSED)))
        w->status.brightness = old.brightness;
    orb_home_host_environment(w->home_host, orb_system_home_hour(w->status.clock_time), w->contrast);
    bool level_page = w->surface == ORB_SHELL_DISPLAY ||
        (w->surface == ORB_SHELL_CONTROL_CENTER && !w->page_index) ||
        (w->surface == ORB_SHELL_SOUND && orb_settings_host_page_for(w->settings, w->surface) == 1);
    if (!w->time_open && !level_page && (old.net != snapshot->net || old.link != snapshot->link)) render(w);
    if (w->surface == ORB_SHELL_STACK &&
        (strcmp(old.clock_time, snapshot->clock_time) || strcmp(old.clock_date, snapshot->clock_date))) {
        if (previous_stack != stack_signature(w)) render(w);
        else {
            stack_refresh(w, ORB_STACK_REFRESH_CLOCK);
        }
    }
    if ((w->surface == ORB_SHELL_FACE || w->surface == ORB_SHELL_OFFLINE) && w->page) {
        face_snapshot(w);
    } else if (w->surface == ORB_SHELL_CHARGING) {
        orb_charging_model_t model = orb_charging_project(snapshot, w->contrast);
        orb_charging_host_update(w->charging_host, &model);
    } else if (w->surface == ORB_SHELL_ABOUT) {
        if (orb_system_about_changed(&old, snapshot)) render(w);
    }
    if (w->surface == ORB_SHELL_CONTROL_CENTER && w->page_index) {
        const orb_home_scene_t *scene = (w->home && w->home->online && w->home->synced && w->home->available) ? orb_control_home_scene(w->home, orb_system_home_hour(w->status.clock_time)) : NULL;
        if (strcmp(w->home_scene_id, scene ? scene->id : "")) render(w);
    }
    if (w->surface == ORB_SHELL_BATTERY) {
        if (old.desk_charge_limit != snapshot->desk_charge_limit || old.battery_setting_error != snapshot->battery_setting_error)
            render(w);
        else if (!orb_settings_host_update_battery(w->settings, snapshot)) render(w);
    }
    orb_workspace_poll(w);
}

/* The shell owns Silent: its environment tick re-applies it to orb_sound, so
 * any other writer (Control Center, Settings, devtools) must come through here. */
void orb_workspace_set_silent(orb_workspace_t *w, bool silent)
{
    if (!w) return;
    w->silent = silent;
    orb_sound_set_silent(silent);
    orb_power_snapshot_t p = orb_shell_power_snapshot(w->ctx);
    orb_sound_set_muted(w->silent, w->face_down || (p.revision && (p.silent || p.redact)));
    if (w->surface != ORB_SHELL_CONTROL_CENTER || w->page_index) render(w);
}

bool orb_workspace_volume_control_visible(const orb_workspace_t *w)
{
    return w && w->shown && w->display_active &&
        ((w->surface == ORB_SHELL_CONTROL_CENTER && !w->page_index) ||
         (w->surface == ORB_SHELL_SOUND && (orb_settings_host_page_for(w->settings, w->surface) == 1 || !orb_settings_host_page_for(w->settings, w->surface))));
}

bool orb_workspace_twist(orb_workspace_t *w, int detents)
{
    if (w && w->shown && w->display_active && w->surface == ORB_SHELL_DISPLAY &&
        orb_settings_host_display_twist(w->settings, detents)) return true;
    if (!w || !w->shown || !w->display_active || w->nav_pending || w->nav_presenting || w->opening)
        return false;
    nav_bind_rotary(w);
    bool changed;
    if (!orb_app_host_twist(w->home_host, detents, &changed)) changed = orb_nav_rotary_step(&w->nav, detents);
    /* A case twist has no pointer release. Retire the cached-list ownership
     * too, otherwise render retries wait forever for a lift that cannot come. */
    orb_round_list_scroll_finish(nav_scroll_owner(w));
    return changed;
}

bool orb_workspace_dismiss_banner(orb_workspace_t *w)
{
    if (!w || !w->shown || !w->notification) return false;
    /* Presentation only: leave the waiting card and its receipt untouched. */
    lv_obj_delete(w->notification);
    w->notification = NULL;
    w->new_card_at = 0;
    w->notification_sound_pending = false;
    return true;
}

static void time_poll(orb_workspace_t *w)
{
    uint32_t now = lv_tick_get();
    orb_time_snapshot_t state; orb_time_snapshot(orb_time_now_us(), &state);
    bool changed = state.generation != w->time.generation;
    if (!changed && !w->time_nav_dirty && now - w->time_polled_at < 250) return;
    uint32_t was_stack = stack_signature(w);
    bool timer_tick = (state.timer_remaining_us + 999999) / 1000000 !=
        (w->time.timer_remaining_us + 999999) / 1000000;
    bool stopwatch_tick = state.stopwatch_elapsed_us / 1000000 != w->time.stopwatch_elapsed_us / 1000000;
    bool page_changed = orb_app_time_page_changed(time_screen(w));
    /* Local utility state has no hub revision. Retire stale planes after a
     * captured drag settles: invalidating its source/target would cancel it. */
    if (changed || timer_tick || stopwatch_tick || page_changed)
        w->time_nav_dirty |= 1u << ORB_SHELL_TIME;
    if (changed || timer_tick || stopwatch_tick)
        w->time_nav_dirty |= (1u << ORB_SHELL_FACE) | (1u << ORB_SHELL_STACK);
    if (!w->nav_pending && !w->nav_presenting) {
        for (unsigned slot = 0; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++)
            if (w->time_nav_dirty & (1u << w->nav_cached[slot]))
                orb_nav_compositor_invalidate(w->compositor, slot);
        w->time_nav_dirty = 0;
    }
    w->time = state; w->time_polled_at = now;
    if (was_stack != stack_signature(w) && w->surface == ORB_SHELL_STACK) render(w);
    if (w->shown && w->display_active) {
        orb_app_host_poll(w->time_host);
        stack_refresh(w, ORB_STACK_REFRESH_TIME);
    }
}

/* Poll these owners before observing the model generation, as before. */
static void poll_apps(orb_workspace_t *w)
{
    play_sync_selection(w);
    time_poll(w);
    face_snapshot(w);
    if (w->surface == ORB_SHELL_NOTIFICATIONS || w->surface == ORB_SHELL_NOTIFICATION_DETAIL) {
        /* Age ticks retain the viewport and its throw direction. Only stale
         * cached captures retire; a presenting transition keeps frozen planes. */
        if (orb_notifications_host_tick(w->notifications_host, w->notifications,
            w->surface == ORB_SHELL_NOTIFICATION_DETAIL, lv_tick_get())) {
            w->nav_revision++;
            if (w->nav_timer) lv_timer_resume(w->nav_timer);
        }
    }
    orb_app_host_poll(w->home_host);
    if (orb_workspace_home_refresh(&w->snapshots.home, &w->home)) {
        (void)orb_control_home_wait(&w->control_home_loading, &w->control_home_wait_at, w->home);
        home_status_refresh(w);
        if (w->surface == ORB_SHELL_STACK) render(w);
    }
    orb_shell_app_state_t app; orb_shell_apps_snapshot(&app);
    if (app.revision != w->app_revision) {
        w->app_revision = app.revision;
        if (w->time_open && !app.pending && strcmp(app.active, "time"))
            navigate(w, ORB_SHELL_APPS, false);
        if (w->surface == ORB_SHELL_PLAY && !w->time_open) render(w);
    }
}

void orb_workspace_poll(orb_workspace_t *w)
{
    if (w && w->surface == ORB_SHELL_CONTROL_CENTER && !w->page_index && orb_control_level(orb_app_host_current(w->control_host))) {
        orb_control_model_t model = orb_control_project(&w->status, orb_sound_volume(), w->silent, w->ctx && orb_shell_quiet(w->ctx), w->flashlight, w->control_volume);
        (void)orb_control_host_update(w->control_host, &model);
    }
    if (w && w->surface == ORB_SHELL_DISPLAY && !orb_settings_host_update_display(w->settings, w->ctx, w->status.brightness)) render(w);
    if (w && w->surface == ORB_SHELL_SOUND &&
        !orb_settings_host_update_sound(w->settings, w->silent, w->model->speech_voice, w->notice)) render(w);
    if (w && w->surface == ORB_SHELL_ONBOARDING) orb_system_views_poll(system_host(w), true);
    if (!w || w->fixture) return;
    /* Closing the pager advances the model epoch. Do it after an object iris
     * settles; its own lifecycle update must not immediately retire that iris.
     * Retaining it during speculation also preserves cancelled Back/entry. */
    if (w->idle && !w->nav_pending && !w->nav_presenting && !workspace_idle_retained(w)) {
        orb_model_idle_close();
        orb_workspace_payload_free(w->idle); w->idle = NULL;
    }
    poll_apps(w);
    if (w->page && w->nav_timer && orb_kit_motion_settling(w->page)) lv_timer_resume(w->nav_timer);
    bool was_synced = w->model->online && w->model->synced;
    orb_voice_phase_t old_phase = w->model->voice.phase;
    /* Level-only revisions update the meter in place: capture does not tear
     * down a live LVGL tree 50 times a second. */
    orb_voice_state_t old_voice = w->model->voice;
    bool old_media_valid = w->model->nowplaying_valid;
    int old_media_volume = w->model->nowplaying.volume;
    unsigned old_card_count = w->model->card_count;
    orb_stack_entry_t previous_stack = w->surface == ORB_SHELL_STACK ? stack_selected(w) : (orb_stack_entry_t){0};
    if (!orb_workspace_model_refresh(&w->snapshots.model, &w->model)) return;
    if (!ready(w)) w->selection_external = false;
    /* Removing an earlier request should keep the viewed live activity under
     * the crown. A new request still rises to the current priority position. */
    if (w->surface == ORB_SHELL_STACK && !w->card[0] && old_card_count > w->model->card_count &&
        previous_stack.kind != ORB_STACK_CARD) stack_restore_entry(w, previous_stack);
    /* Only confirmed Mac telemetry may drive the volume HUD. The initial
     * snapshot is baseline, and an unacknowledged twist never invents a value. */
    if (w->ctx && old_media_valid && w->model->nowplaying_valid &&
        w->model->nowplaying_supported && old_media_volume != w->model->nowplaying.volume)
        orb_shell_media_volume(w->ctx, (unsigned)w->model->nowplaying.volume);
    bool had_history = w->counts_known;
    orb_notification_arrivals_t arrivals = orb_notification_history_arrivals(w->notifications, w->model);
    bool new_card = arrivals.new_card, new_generic_card = arrivals.new_generic;
    if (w->counts_known && new_card) {
        w->all_clear = false;
        w->new_card_at = lv_tick_get(); w->notification_sound_pending |= new_generic_card;
        w->arrival = w->surface == ORB_SHELL_STACK;
        /* Paired non-sensitive badge appears in render_now without stealing focus. */
        delight_wake(w);
    }
    w->counts_known = true;
    for (unsigned i = 0; i < w->model->session_count; i++)
        if (w->has_selection && !strcmp(w->selection.id, w->model->sessions[i].id)) {
            w->selection = w->model->sessions[i];
            w->selection_external = false;
        }
    if ((w->surface == ORB_SHELL_IDLE_SESSIONS || w->surface == ORB_SHELL_SESSIONS) && w->idle) {
        bool loading = w->idle->pending;
        orb_model_idle_snapshot(w->idle);
        /* Keep the More footer visible through pending/error. A confirmed
         * replacement starts at its first session, never an old page offset. */
        if (loading && !w->idle->pending && !w->idle->error[0]) w->navigation_pending = true;
    }
    uint8_t level = old_voice.level;
    old_voice.level = w->model->voice.level;
    bool level_only = level != old_voice.level && !memcmp(&old_voice, &w->model->voice, sizeof(old_voice));
    orb_voice_flow_poll(w->voice_flow, w->model->voice.phase);
    if (old_phase != w->model->voice.phase && w->surface == ORB_SHELL_VOICE) {
        if (w->model->voice.phase == ORB_VOICE_RECORDING) orb_sound_play(ORB_SOUND_LISTENING_START);
        if (old_phase == ORB_VOICE_RECORDING) orb_sound_play(ORB_SOUND_LISTENING_STOP);
    }
    orb_notification_effects_t effects = orb_notification_history_reconcile(w->notifications,
        w->model, had_history, new_card, was_synced, lv_tick_get());
    if (effects.all_clear) {
        w->all_clear = true; w->clear_at = lv_tick_get();
        delight_wake(w);
    }
    if (effects.approval_cue != ORB_NOTIFICATION_CUE_NONE)
        orb_sound_play(effects.approval_cue == ORB_NOTIFICATION_CUE_DENY ? ORB_SOUND_DENY : ORB_SOUND_APPROVE);
    /* A fast card confirmation may already be IDLE when the UI next polls.
     * The model's answered flag owns the receipt; idle never proves delivery. */
    const char *voice_card = old_voice.card[0] ? old_voice.card : w->model->voice.card;
    const orb_notification_record_t *spoken = orb_notification_history_find(w->notifications, voice_card);
    if (w->surface == ORB_SHELL_VOICE && spoken && (spoken->queued || spoken->resolved)) {
        snprintf(w->card, sizeof(w->card), "%s", spoken->id);
        navigate(w, ORB_SHELL_CARD, false);
        return;
    }
    if (old_phase != w->model->voice.phase && w->model->voice.phase != ORB_VOICE_IDLE &&
        w->surface != ORB_SHELL_VOICE && !orb_voice_flow_held(w->voice_flow)) {
        navigate(w, ORB_SHELL_VOICE, true);
        return;
    }
    if (orb_voice_flow_held(w->voice_flow) || (level_only && w->surface == ORB_SHELL_VOICE &&
        old_phase == ORB_VOICE_RECORDING && ready(w) == was_synced)) { orb_voice_host_meter(w->voice_host); return; }
    voice_focus(w);
    if (!w->opening && !w->navigation_pending && !w->nav_pending && !w->nav_presenting) {
        w->nav_revision++;
        if (agents_refresh(w)) return;
    }
    if (faces_screen(w)) { face_snapshot(w); return; }
    if (w->surface == ORB_SHELL_HOME || w->time_open) return;
    if (!new_card && (w->surface == ORB_SHELL_DISPLAY || (w->surface == ORB_SHELL_WEATHER && orb_settings_host_update_weather(w->settings, w->model)) ||
        (w->surface == ORB_SHELL_CONTROL_CENTER && !w->page_index) ||
        (w->surface == ORB_SHELL_SOUND && orb_settings_host_page_for(w->settings, w->surface) == 1))) return;
    if (w->surface == ORB_SHELL_STACK && orb_stack_screen_has_metrics(orb_app_host_current(w->stack_host)) && old_card_count == w->model->card_count &&
        previous_stack.kind == stack_selected(w).kind && previous_stack.index == stack_selected(w).index) {
        if (previous_stack.kind == ORB_STACK_HOME) { stack_refresh(w, ORB_STACK_REFRESH_HOME); return; }
        if (previous_stack.kind == ORB_STACK_USAGE) { stack_refresh(w, ORB_STACK_REFRESH_USAGE); return; }
    }
    if (w->opening) {
        w->scaffolded = false;
    }
    render(w);
}

void orb_workspace_navigate(orb_workspace_t *w, orb_shell_surface_t surface)
{
    if (!w || surface > ORB_SHELL_SURFACE_LAST) return;
    /* Physical navigation must retire even an asynchronously pending snapshot.
     * Otherwise its later prepare callback can replace the button's new page. */
    if (w->nav_pending || w->nav_presenting || w->nav_fallback) nav_cancel(w);
    nav_preview_stop(w);
    w->nav_settle_capture = false;
    if (surface != ORB_SHELL_NOTIFICATIONS && surface != ORB_SHELL_STACK &&
        surface != ORB_SHELL_CONTROL_CENTER) {
        /* A direct place selection dismisses the open layer stack. Record
         * the underlying place as its Back parent, rather than resurrecting
         * the dismissed shade on Back. Keep the displayed source tree/cache
         * intact until the ordinary route replaces it below. */
        while (w->nav.current == ORB_SHELL_NOTIFICATIONS || w->nav.current == ORB_SHELL_STACK ||
               w->nav.current == ORB_SHELL_CONTROL_CENTER) {
            if (!orb_nav_back(&w->nav, NULL)) {
                orb_nav_set_current(&w->nav, ORB_SHELL_FACE);
                break;
            }
        }
    }
    /* Preserve a final return plane before direct selection changes the live
     * surface; Back must not rebuild it during its first visible frame. */
    if (w->compositor && surface != w->surface &&
        (orb_kit_motion_settling(w->page) || w->nav_apps_preview[0]) && !nav_capture(w, w->surface, 0))
        orb_nav_compositor_invalidate(w->compositor, 0);
    if (w->compositor && surface != w->surface && w->nav_cached[0] == w->surface &&
        orb_nav_compositor_valid(w->compositor, 0, w->nav_revision)) {
        unsigned retain = w->nav_cached[1] == surface &&
            orb_nav_compositor_valid(w->compositor, 1, w->nav_revision) ? 2 : 1;
        if (orb_nav_compositor_swap(w->compositor, 0, retain)) {
            orb_shell_surface_t previous = w->nav_cached[0];
            w->nav_cached[0] = w->nav_cached[retain];
            w->nav_cached[retain] = previous;
            bool preview = w->nav_apps_preview[0];
            w->nav_apps_preview[0] = w->nav_apps_preview[retain];
            w->nav_apps_preview[retain] = preview;
        }
    }
    navigate(w, surface, true);
}
orb_shell_surface_t orb_workspace_surface(const orb_workspace_t *w)
{
    return w ? (w->nav_pending || (w->nav_presenting && w->nav_commit) ?
        w->nav_target : w->surface) : ORB_SHELL_FACE;
}
orb_shell_surface_t orb_workspace_nav_current(const orb_workspace_t *w)
{
    return w ? (orb_shell_surface_t)w->nav.current : ORB_SHELL_FACE;
}
static void session_open_token(orb_workspace_t *w, const char *id, const orb_nav_token_t *source)
{
    if (!w || !id) return;
    orb_nav_token_rect_t token = source ? (orb_nav_token_rect_t){source->x, source->y, source->width, source->height} :
        (orb_nav_token_rect_t){197, 197, 72, 72};
    /* id may point inside the model snapshot; copy before a render/poll. */
    snprintf(w->session, sizeof(w->session), "%s", id);
    for (unsigned i = 0; i < w->model->session_count; i++)
        if (!strcmp(w->session, w->model->sessions[i].id)) {
            w->selection = w->model->sessions[i]; w->has_selection = true; w->selection_external = false; break;
        }
    w->launch = (lv_point_t){token.x + token.width / 2, token.y + token.height / 2};
    if (!nav_start(w, ORB_SHELL_SESSION, ORB_NAV_TRANSITION_IRIS,
        ORB_NAV_DIRECTION_LEFT, false, &token, false)) navigate(w, ORB_SHELL_SESSION, true);
}
void orb_workspace_session(orb_workspace_t *w, const char *id)
{
    orb_launch_token_t token = {0};
    if (w && id && w->surface == ORB_SHELL_SESSIONS)
        for (unsigned i = 0; i < w->model->session_count; i++)
            if (!strcmp(id, w->model->sessions[i].id)) { (void)orb_app_agents_session_token(agents_screen(w), i, &token); break; }
    session_open_token(w, id, token.valid ? &token.rect : NULL);
}
void orb_workspace_card(orb_workspace_t *w, const char *id)
{
    if (!w || !id) return;
    snprintf(w->card, sizeof(w->card), "%s", id); navigate(w, ORB_SHELL_CARD, true);
}
void orb_workspace_talk_origin(orb_workspace_t *w, bool synthetic)
{
    if (!w) return;
    LV_UNUSED(synthetic);
    orb_voice_flow_boot(w->voice_flow);
}
void orb_workspace_talk(orb_workspace_t *w, bool held)
{
    if (!w || held == w->held) return;
    w->held = held;
    /* main/orb_voice owns BOOT and the 350ms hold threshold. Shell observes
     * the published phase; a tap must neither record nor confirm twice. */
}
int orb_workspace_brightness(const orb_workspace_t *w) { return w ? w->status.brightness : 70; }

void orb_workspace_back(orb_workspace_t *w) { if (w) back(w); }

bool orb_workspace_face_local(const orb_workspace_t *w)
{
    return w && faces_screen(w) && orb_app_faces_mode(faces_screen(w)) != ORB_FACES_FACE;
}

void orb_workspace_open_metrics(const orb_workspace_t *w, orb_shell_open_metrics_t *out)
{
    if (!w || !out) return;
    *out = w->open;
    out->active |= orb_kit_motion_settling(w->root);
    out->active |= w->nav_cached[0] == w->surface && w->nav_apps_preview[0];
    out->retaining_snapshot = w->nav_settle_capture;
    if (w->compositor) {
        orb_nav_compositor_stats_t stats = {0};
        orb_nav_compositor_stats(w->compositor, &stats);
        out->cache_bytes = stats.snapshot_bytes;
        out->cache_entries = (uint32_t)(stats.snapshot_bytes / OPEN_PIXELS);
        out->snapshot_captures = stats.captures;
        out->background_prepares = stats.background_prepares;
        out->background_hits = stats.background_hits;
        return;
    }
    for (unsigned i = 0; i < OPEN_CACHE_COUNT; i++) {
        if (w->cache[i].image.data) {
            out->cache_entries++;
            out->cache_bytes += OPEN_PIXELS;
        }
    }
}

bool orb_workspace_increase_contrast(const orb_workspace_t *w) { return w && w->contrast; }

bool orb_workspace_speech_agent(const orb_workspace_t *w, orb_agent_t *agent)
{
    if (!w || !agent || !ready(w)) return false;
    orb_voice_state_t voice;
    return orb_model_get_voice(&voice) && orb_app_voice_speech_agent(w->model, &voice, agent);
}
