/* Repeatable real-surface workload; source state is private and synthetic.
 * HIG §6: a quick finger drag hands off to a decelerating fling, then rests.
 * Unlike the stress demo, the idle interval remains idle and is reported. */
#include "orb_scroll_fixture.h"
#include "orb_workspace.h"
#include "orb_app_home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define CYCLE_MS 1600u
#define DRAG_MS 240u

struct orb_scroll_fixture {
    orb_workspace_t *workspace;
    lv_obj_t *scroll;
    lv_indev_t *pointer;
    lv_indev_data_t input;
    orb_scroll_observer_t observer;
    void *observer_ctx;
    orb_scroll_surface_t surface;
    int32_t extent, position, drag_top, drag_bottom;
    uint32_t cycle, report_at;
    unsigned gestures;
    orb_scroll_fixture_diagnostics_t diagnostics;
    uint32_t previous_offset_at;
    bool offset_observed;
    bool reverse;
};

static void read_pointer(lv_indev_t *pointer, lv_indev_data_t *data)
{
    orb_scroll_fixture_t *fixture = lv_indev_get_user_data(pointer);
    data->point = fixture->input.point;
    data->state = fixture->input.state;
    if (fixture->observer)
        fixture->observer(data->state == LV_INDEV_STATE_PRESSED ? ORB_SCROLL_INPUT : ORB_SCROLL_RELEASE,
                          data->point.x, data->point.y, fixture->observer_ctx);
}

static void report(orb_scroll_fixture_t *fixture, int x, int y, bool pressed)
{
    fixture->input.point = (lv_point_t){x, y};
    fixture->input.state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (pressed) fixture->diagnostics.reports++;
    else fixture->diagnostics.releases++;
    lv_indev_read(fixture->pointer);
    /* LVGL resumes even EVENT-mode timers to service long presses. These
     * strokes finish before long-press recognition; retain exactly one input
     * read per report rather than introducing stale duplicate samples. */
    lv_timer_pause(lv_indev_get_read_timer(fixture->pointer));
}

static void scrolled(lv_event_t *event)
{
    orb_scroll_fixture_t *fixture = lv_event_get_user_data(event);
    int32_t position = lv_obj_get_scroll_y(fixture->scroll);
    fixture->diagnostics.scroll_events++;
    if (position != fixture->position) {
        fixture->diagnostics.offset_changes++;
        uint32_t now = lv_tick_get();
        uint32_t gap = fixture->offset_observed ? now - fixture->previous_offset_at : 0;
        if (gap > fixture->diagnostics.offset_gap_max_ms) fixture->diagnostics.offset_gap_max_ms = gap;
        fixture->previous_offset_at = now;
        fixture->offset_observed = true;
    }
    fixture->position = position;
    if (fixture->observer)
        fixture->observer(ORB_SCROLL_OFFSET, lv_obj_get_scroll_x(fixture->scroll),
                          fixture->position, fixture->observer_ctx);
}

static void *payload(size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(1, size);
#endif
}

static const char *const s_names[] = {"agents", "rooms", "settings", "notifications", "favourites"};

bool orb_scroll_surface_parse(const char *name, orb_scroll_surface_t *surface)
{
    if (!name || !surface) return false;
    for (unsigned i = 0; i < ORB_SCROLL_SURFACE_COUNT; i++) {
        if (!strcmp(name, s_names[i])) { *surface = i; return true; }
    }
    return false;
}

const char *orb_scroll_surface_name(orb_scroll_surface_t surface)
{ return surface >= 0 && surface < ORB_SCROLL_SURFACE_COUNT ? s_names[surface] : "invalid"; }

static void data(orb_model_snapshot_t *model, orb_home_snapshot_t *home)
{
    static const char *const projects[] = {"Orb", "Recipe", "PlexBot", "Home", "Library", "Intake", "Media", "Notes"};
    static const char *const rooms[] = {"Living Room", "Kitchen", "Bedroom", "Office", "Hallway", "Bathroom", "Guest Room", "Dining Room", "Studio", "Garage", "Patio", "Entry"};
    model->revision = 1; model->online = model->synced = true;
    orb_model_prefs_defaults(&model->prefs);
    model->idle_count = 12;
    model->session_count = ORB_MODEL_MAX_FOCUS;
    for (unsigned i = 0; i < model->session_count; i++) {
        orb_session_t *session = &model->sessions[i];
        snprintf(session->id, sizeof(session->id), "bench-session-%u", i);
        snprintf(session->title, sizeof(session->title), "%s", projects[i]);
        snprintf(session->project, sizeof(session->project), "%s", projects[i]);
        snprintf(session->summary, sizeof(session->summary), "%s", i % 3 ? "Reviewing changes" : "Tests passed");
        session->agent = i & 1 ? ORB_AGENT_CODEX : ORB_AGENT_CLAUDE;
        session->state = i % 3 ? ORB_SESSION_BUSY : ORB_SESSION_DONE;
        session->updated = 1000 - i;
    }
    model->card_count = 10;
    for (unsigned i = 0; i < model->card_count; i++) {
        orb_card_t *card = &model->cards[i];
        snprintf(card->id, sizeof(card->id), "bench-notice-%u", i);
        snprintf(card->session, sizeof(card->session), "bench-session-%u", i % ORB_MODEL_MAX_FOCUS);
        snprintf(card->title, sizeof(card->title), "%s updated", projects[i % ORB_MODEL_MAX_FOCUS]);
        snprintf(card->body, sizeof(card->body), "The latest changes are ready to review.");
        card->kind = ORB_CARD_ATTENTION;
        card->priority = ORB_PRIORITY_NORMAL;
    }
    home->revision = 1; home->online = home->synced = home->available = home->demo = true;
    home->room_count = sizeof(rooms) / sizeof(*rooms);
    home->accessory_count = 20;
    for (unsigned i = 0; i < home->room_count; i++) {
        snprintf(home->rooms[i].id, sizeof(home->rooms[i].id), "bench-room-%u", i);
        snprintf(home->rooms[i].name, sizeof(home->rooms[i].name), "%s", rooms[i]);
    }
    for (unsigned i = 0; i < home->accessory_count; i++) {
        orb_home_accessory_t *accessory = &home->accessories[i];
        snprintf(accessory->id, sizeof(accessory->id), "bench-light-%u", i);
        snprintf(accessory->room, sizeof(accessory->room), "bench-room-%u", i % (unsigned)home->room_count);
        snprintf(accessory->name, sizeof(accessory->name), "Light %u", i + 1);
        accessory->kind = ORB_HOME_LIGHT; accessory->reachable = true;
        accessory->favourite = i < 10;
        accessory->state.present = ORB_HOME_ON;
        accessory->state.on = i % 3 != 0;
    }
}

orb_scroll_fixture_t *orb_scroll_fixture_create(lv_obj_t *parent, orb_scroll_surface_t surface)
{
    if (!parent || surface < 0 || surface >= ORB_SCROLL_SURFACE_COUNT) return NULL;
    orb_scroll_fixture_t *fixture = payload(sizeof(*fixture));
    orb_model_snapshot_t *model = payload(sizeof(*model));
    orb_home_snapshot_t *home = payload(sizeof(*home));
    if (!fixture || !model || !home) { free(fixture); free(model); free(home); return NULL; }
    data(model, home);
    fixture->workspace = orb_workspace_create_fixture(parent, model, home);
    free(model); free(home);
    if (!fixture->workspace) { free(fixture); return NULL; }
    fixture->pointer = lv_indev_create();
    if (!fixture->pointer) { orb_workspace_destroy(fixture->workspace); free(fixture); return NULL; }
    lv_indev_set_type(fixture->pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(fixture->pointer, lv_obj_get_display(parent));
    lv_indev_set_read_cb(fixture->pointer, read_pointer);
    lv_indev_set_user_data(fixture->pointer, fixture);
    lv_indev_set_mode(fixture->pointer, LV_INDEV_MODE_EVENT);
    fixture->surface = surface;
    fixture->cycle = UINT32_MAX;
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 82, .rssi = -50, .brightness = 70, .demo = true};
    snprintf(status.clock_time, sizeof(status.clock_time), "10:09");
    snprintf(status.hub_name, sizeof(status.hub_name), "Scroll fixture");
    orb_workspace_update(fixture->workspace, &status);
    static const orb_shell_surface_t surfaces[] = {ORB_SHELL_SESSIONS, ORB_SHELL_HOME,
        ORB_SHELL_SETTINGS, ORB_SHELL_NOTIFICATIONS, ORB_SHELL_HOME};
    orb_workspace_navigate(fixture->workspace, surfaces[surface]);
    return fixture;
}

bool orb_scroll_fixture_prepare(orb_scroll_fixture_t *fixture)
{
    if (!fixture) return false;
    /* Keep the legacy favourites selector as a Home landing-list workload. */
    fixture->scroll = orb_workspace_fixture_scroll(fixture->workspace);
    if (!fixture->scroll) return false;
    lv_obj_update_layout(fixture->scroll);
    fixture->extent = lv_obj_get_scroll_bottom(fixture->scroll) + lv_obj_get_scroll_y(fixture->scroll);
    lv_area_t bounds;
    lv_obj_get_coords(fixture->scroll, &bounds);
    fixture->drag_top = LV_MAX(160, bounds.y1 + 20);
    fixture->drag_bottom = LV_MIN(350, bounds.y2 - 20);
    lv_obj_remove_event_cb_with_user_data(fixture->scroll, scrolled, fixture);
    lv_obj_add_event_cb(fixture->scroll, scrolled, LV_EVENT_SCROLL, fixture);
    return fixture->extent > 0;
}

void orb_scroll_fixture_tick(orb_scroll_fixture_t *fixture, uint32_t elapsed_ms)
{
    if (!fixture || !fixture->scroll) return;
    uint32_t cycle = elapsed_ms / CYCLE_MS, phase = elapsed_ms % CYCLE_MS;
    if (cycle != fixture->cycle) {
        if (fixture->input.state == LV_INDEV_STATE_PRESSED)
            report(fixture, fixture->input.point.x, fixture->input.point.y, false);
        fixture->gestures++;
        fixture->offset_observed = false;
        if (fixture->position >= fixture->extent - 2) fixture->reverse = true;
        else if (fixture->position <= 2) fixture->reverse = false;
        fixture->report_at = elapsed_ms;
        if (phase < DRAG_MS)
            report(fixture, 233, fixture->reverse ? fixture->drag_top : fixture->drag_bottom, true);
    }
    /* A 240ms accelerating finger stroke, sampled at the production path's
     * nominal 16ms poll interval, releases into LVGL's real momentum. No
     * scripted offsets or replay of missed samples hide input/renderer stalls. */
    if (phase < DRAG_MS && elapsed_ms - fixture->report_at >= ORB_SCROLL_REPORT_MS) {
        uint32_t gap = elapsed_ms - fixture->report_at;
        if (gap > fixture->diagnostics.report_gap_max_ms) fixture->diagnostics.report_gap_max_ms = gap;
        float t = phase / (float)DRAG_MS;
        float progress = t * (.35f + .65f * t);
        int distance = (int)((fixture->drag_bottom - fixture->drag_top) * progress);
        report(fixture, 233, fixture->reverse ? fixture->drag_top + distance : fixture->drag_bottom - distance, true);
        fixture->report_at = elapsed_ms;
    } else if (phase >= DRAG_MS && fixture->input.state == LV_INDEV_STATE_PRESSED) {
        report(fixture, 233, fixture->reverse ? fixture->drag_bottom : fixture->drag_top, false);
    }
    fixture->cycle = cycle;
    uint32_t nominal = cycle * (DRAG_MS / ORB_SCROLL_REPORT_MS) +
                       LV_MIN(phase / ORB_SCROLL_REPORT_MS + 1, DRAG_MS / ORB_SCROLL_REPORT_MS);
    fixture->diagnostics.skipped_reports = nominal > fixture->diagnostics.reports ? nominal - fixture->diagnostics.reports : 0;
}

void orb_scroll_fixture_diagnostics(const orb_scroll_fixture_t *fixture, orb_scroll_fixture_diagnostics_t *out)
{
    *out = fixture ? fixture->diagnostics : (orb_scroll_fixture_diagnostics_t){0};
}

void orb_scroll_fixture_destroy(orb_scroll_fixture_t *fixture)
{
    if (!fixture) return;
    /* LVGL's throw animation owns the indev as its var; indev_delete itself
     * leaves that animation alive. Arbitrary bench durations can end mid-fling. */
    lv_anim_delete(fixture->pointer, NULL);
    lv_indev_delete(fixture->pointer);
    orb_workspace_destroy(fixture->workspace);
    free(fixture);
}
bool orb_scroll_fixture_moving(const orb_scroll_fixture_t *fixture)
{ return fixture && (fixture->input.state == LV_INDEV_STATE_PRESSED ||
    (fixture->scroll && lv_obj_is_scrolling(fixture->scroll)) ||
    orb_app_home_moving(orb_workspace_fixture_home(fixture->workspace))); }
unsigned orb_scroll_fixture_gestures(const orb_scroll_fixture_t *fixture)
{ return fixture ? fixture->gestures : 0; }
int32_t orb_scroll_fixture_position(const orb_scroll_fixture_t *fixture)
{ return fixture && fixture->scroll ? lv_obj_get_scroll_y(fixture->scroll) : 0; }

uint32_t orb_scroll_fixture_viewport_pixels(const orb_scroll_fixture_t *fixture)
{
    if (!fixture || !fixture->pointer) return 0;
    lv_display_t *display = lv_indev_get_display(fixture->pointer);
    if (!display) return 0;
    lv_area_t bounds = {0, 0, lv_display_get_horizontal_resolution(display) - 1,
                       lv_display_get_vertical_resolution(display) - 1};
    if (fixture->scroll) {
        lv_area_t viewport;
        lv_obj_get_coords(fixture->scroll, &viewport);
        bounds.x1 = LV_MAX(bounds.x1, viewport.x1);
        bounds.y1 = LV_MAX(bounds.y1, viewport.y1);
        bounds.x2 = LV_MIN(bounds.x2, viewport.x2);
        bounds.y2 = LV_MIN(bounds.y2, viewport.y2);
        if (bounds.x1 > bounds.x2 || bounds.y1 > bounds.y2) return 0;
    }
    return (uint32_t)lv_area_get_width(&bounds) * (uint32_t)lv_area_get_height(&bounds);
}

void orb_scroll_fixture_observe(orb_scroll_fixture_t *fixture, orb_scroll_observer_t observer, void *ctx)
{
    if (fixture) { fixture->observer = observer; fixture->observer_ctx = ctx; }
}
