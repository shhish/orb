/* Gesture model v3 through the real shell and LVGL pointer read timer.
 * Fixtures are process-local; no board, network, or persisted settings. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_shell_services.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks, s_actions;
static const char *s_case = "setup";
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static lv_obj_t *s_pressed_target;
static unsigned s_probe_clicks;
static bool s_probe_active;
static uint8_t s_retained[SIM_D * SIM_D * 3], s_redrawn[sizeof(s_retained)];

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "gestures3 %s at %u ms: %s:%d: %s\n", \
        s_case, s_now, __FILE__, __LINE__, #value); exit(1); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_input; }
static void target_deleted(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == s_pressed_target) s_pressed_target = NULL;
}
static void probe_clicked(lv_event_t *event)
{
    (void)event;
    if (s_probe_active) ++s_probe_clicks;
}
static void input_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        s_pressed_target = lv_event_get_param(event);
        if (s_pressed_target) lv_obj_add_event_cb(s_pressed_target, target_deleted, LV_EVENT_DELETE, NULL);
    }
}
static void action(orb_shell_action_t value, void *ctx) { (void)value; (void)ctx; ++s_actions; }
static void advance(unsigned ms)
{
    uint32_t end = s_now + ms;
    while (s_now < end) {
        s_now += LV_MIN(4, end - s_now);
        orb_shell_apps_service();
        orb_apps_poll((int64_t)s_now * 1000, false, true, 0, 0, 1, false);
        orb_shell_apps_publish();
        orb_shell_poll(s_shell);
        lv_timer_handler();
    }
}
static void pointer(int x, int y, bool down, unsigned ms)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    advance(ms);
}
static void release(void) { pointer(s_input.point.x, s_input.point.y, false, 64); }
static void drag(int x, int y, int dx, int dy)
{
    pointer(x, y, true, 40);
    for (unsigned step = 1; step <= 12; ++step)
        pointer(x + dx * (int)step / 12, y + dy * (int)step / 12, true, 20);
    release(); advance(1100);
}
static void navigate(orb_shell_surface_t surface)
{
    release();
    orb_shell_navigate(s_shell, surface); advance(1400);
    CHECK(orb_shell_surface(s_shell) == surface);
}
static void boot(void)
{
    orb_button_event_t event = {.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_SHORT,
        .at_ms = s_now, .synthetic = true};
    orb_shell_button(s_shell, &event); advance(1400);
}
static lv_obj_t *text(lv_obj_t *root, const char *caption)
{
    if (lv_obj_is_hidden(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), caption)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = text(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}
/* An open accessory dial shows its value as one hero kit label such as "80%";
 * Design 1.x drew the percent sign as a separate label. */
static lv_obj_t *dial_value(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *t = lv_label_get_text(root);
        size_t n = strlen(t);
        /* Hero-sized only: room rows also show values such as "80%". */
        if (n >= 2 && t[n - 1] == '%' && t[n - 2] >= '0' && t[n - 2] <= '9' &&
            lv_obj_get_style_text_font(root, 0)->line_height >= 48) return root;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = dial_value(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *semantic(lv_obj_t *root, const char *id)
{
    const char *data = lv_obj_get_user_data(root);
    /* Local controls also encode negative integer actions in user_data. */
    if ((intptr_t)data > 4096 && !strcmp(data, id) && lv_obj_is_visible(root)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = semantic(lv_obj_get_child(root, i), id);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *scroll(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_is_scrollable(root) && (lv_obj_get_scroll_dir(root) & LV_DIR_VER) &&
        lv_obj_get_scroll_bottom(root) + lv_obj_get_scroll_y(root) > 0) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = scroll(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}
static lv_point_t target_point(lv_obj_t *obj)
{
    CHECK(obj);
    lv_obj_t *label = obj;
    while (obj && !lv_obj_is_clickable(obj)) obj = lv_obj_get_parent(obj);
    CHECK(obj);
    lv_obj_t *list = lv_obj_get_parent(obj);
    if (list && lv_obj_is_scrollable(list)) {
        lv_obj_stop_scroll_anim(list);
        lv_obj_scroll_to_y(list, LV_MAX(0, lv_obj_get_y(obj) -
            (lv_obj_get_height(list) - lv_obj_get_height(obj)) / 2), LV_ANIM_OFF);
        advance(400);
    }
    lv_area_t area;
    if (!orb_round_list_child_bounds(obj, label, &area)) lv_obj_get_coords(obj, &area);
    lv_point_t point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    CHECK(hypotf(point.x - 233, point.y - 233) < 193);
    return point;
}
static lv_point_t caption_point(const char *caption)
{
    lv_obj_t *label = text(orb_shell_root(s_shell), caption);
    if (!strcmp(caption, "Play")) {
        /* The picker has a passive title and a separate actionable caption. */
        lv_obj_t *root = orb_shell_root(s_shell);
        lv_obj_t *pending[256] = {root};
        unsigned count = 1;
        while (count) {
            lv_obj_t *obj = pending[--count];
            if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), caption) &&
                lv_obj_is_clickable(lv_obj_get_parent(obj))) { label = obj; break; }
            for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i) {
                CHECK(count < sizeof(pending) / sizeof(*pending));
                pending[count++] = lv_obj_get_child(obj, i);
            }
        }
    }
    if (!label) fprintf(stderr, "missing caption: %s\n", caption);
    return target_point(label);
}
static lv_point_t time_caption_point(const char *caption)
{
    lv_obj_t *label = text(orb_shell_root(s_shell), caption);
    CHECK(label);
    /* Design 2.0 preserves the lap offset. The action lives in a summary
     * nested inside that viewport, so the generic direct-parent row helper
     * cannot reveal it after returning from the lap list. */
    for (lv_obj_t *parent = lv_obj_get_parent(label); parent; parent = lv_obj_get_parent(parent)) {
        if (!lv_obj_is_scrollable(parent)) continue;
        lv_obj_stop_scroll_anim(parent);
        lv_obj_scroll_to_y(parent, 0, LV_ANIM_OFF);
    }
    advance(400);
    return target_point(label);
}
static void tap(lv_point_t point)
{
    pointer(point.x, point.y, true, 40); release(); advance(1400);
}
static void no_actions(void)
{
    orb_answer_t answer; orb_home_request_t home; orb_prefs_patch_t prefs;
    CHECK(!orb_model_peek_answer(&answer));
    CHECK(!orb_home_take_request(&home));
    CHECK(!orb_model_take_prefs_request(&prefs));
}
static void unpressed(lv_obj_t *obj)
{
    /* LVGL's validity query follows the parent chain; it cannot inspect a
     * freed pointer. A real model rebuild can retire the pressed control. */
    if (obj != s_pressed_target) { CHECK(s_pressed_target == NULL); return; }
    CHECK(obj && lv_obj_is_valid(obj));
    CHECK(!lv_obj_has_state(obj, LV_STATE_PRESSED));
    CHECK(lv_obj_get_style_transform_scale_x(obj, 0) == 256);
    CHECK(lv_obj_get_style_transform_scale_y(obj, 0) == 256);
}
static void fixtures(void)
{
    orb_session_t sessions[8] = {0};
    for (unsigned i = 0; i < 8; ++i) {
        snprintf(sessions[i].id, sizeof(sessions[i].id), "g3-session-%u", i);
        snprintf(sessions[i].title, sizeof(sessions[i].title), "Build project %u", i + 1);
        sessions[i].agent = i % 2 ? ORB_AGENT_CODEX : ORB_AGENT_CLAUDE;
        sessions[i].state = ORB_SESSION_BUSY;
        sessions[i].updated = 1000 - i;
    }
    CHECK(orb_model_sessions_chunk(sessions, 8, true, false));
    for (unsigned i = 0; i < 10; ++i) {
        orb_card_t card = {.kind = ORB_CARD_ATTENTION, .priority = ORB_PRIORITY_NORMAL};
        snprintf(card.id, sizeof(card.id), "g3-card-%u", i);
        snprintf(card.title, sizeof(card.title), "Project %u updated", i + 1);
        snprintf(card.body, sizeof(card.body), "Ready to review the changes.");
        CHECK(orb_model_upsert_card(&card));
        orb_shell_fixture_notification_age(s_shell, card.id, 3600000);
    }
}
static void honeycomb_back(void)
{
    static const struct { const char *name; int x, y; orb_shell_surface_t surface; } apps[] = {
        {"Agents", 233, 133, ORB_SHELL_AGENTS}, {"Dot", 320, 183, ORB_SHELL_DOT},
        {"Home", 146, 183, ORB_SHELL_HOME}, {"Time", 233, 233, ORB_SHELL_TIME},
        {"Play", 320, 283, ORB_SHELL_PLAY}, {"Updates", 146, 283, ORB_SHELL_UPDATES},
        {"Settings", 233, 333, ORB_SHELL_SETTINGS},
    };
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (unsigned i = 0; i < sizeof(apps) / sizeof(*apps); ++i) {
            s_case = apps[i].name;
            navigate(ORB_SHELL_APPS);
            pointer(apps[i].x, apps[i].y, true, 40);
            lv_obj_t *icon = s_pressed_target;
            pointer(apps[i].x + 11, apps[i].y, true, 40);
            unpressed(icon);
            pointer(apps[i].x, apps[i].y, true, 40); release(); advance(600);
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
            tap((lv_point_t){apps[i].x, apps[i].y});
            CHECK(orb_shell_surface(s_shell) == apps[i].surface);
            if (mode) boot(); else drag(12, 233, 200, 0);
            bool face_neighbour = apps[i].surface == ORB_SHELL_AGENTS || apps[i].surface == ORB_SHELL_HOME;
            CHECK(orb_shell_surface(s_shell) == (face_neighbour ? ORB_SHELL_FACE : ORB_SHELL_APPS));
            CHECK(!sim_display_snapshot_transition_active());
            orb_shell_open_metrics_t metrics;
            orb_shell_open_metrics(s_shell, &metrics);
            CHECK(metrics.content_ready && !metrics.active && !metrics.retaining_snapshot);
            /* A real icon press must not leave a shrunken token or an old
             * Apps caption in the reverse iris's final retained plane. */
            lv_refr_now(s_display); sim_display_rgb(s_retained);
            lv_obj_invalidate(orb_shell_root(s_shell)); lv_refr_now(s_display);
            sim_display_rgb(s_redrawn);
            CHECK(!memcmp(s_retained, s_redrawn, sizeof(s_retained)));
            no_actions();
        }
    }
    static const struct { const char *caption; orb_shell_surface_t surface; } leaves[] = {
        {"Display", ORB_SHELL_DISPLAY}, {"Sound", ORB_SHELL_SOUND},
        {"Accessibility", ORB_SHELL_ACCESSIBILITY}, {"About · 0.5.2", ORB_SHELL_ABOUT},
        {"Sessions", ORB_SHELL_SESSION_SETTINGS}, {"Updates", ORB_SHELL_UPDATES},
        {"Units", ORB_SHELL_UNITS}, {"Battery", ORB_SHELL_BATTERY},
    };
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (unsigned i = 0; i < sizeof(leaves) / sizeof(*leaves); ++i) {
            s_case = leaves[i].caption;
            navigate(ORB_SHELL_APPS); tap((lv_point_t){233, 333});
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
            tap(caption_point(leaves[i].caption));
            CHECK(orb_shell_surface(s_shell) == leaves[i].surface);
            if (mode) boot(); else drag(12, 233, 200, 0);
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
            if (mode) boot(); else drag(12, 233, 200, 0);
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
            no_actions();
        }
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        s_case = "visible Sessions and Usage routes preserve the strict Back chain";
        for (unsigned page = 0; page < 2; ++page) {
            navigate(ORB_SHELL_APPS); tap((lv_point_t){233, 133});
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_AGENTS);
            /* Design 2.0 removes the old header carousel: both centre
             * directions remain content-only under gesture model v3. */
            drag(300, 100, -140, 0);
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_AGENTS);
            drag(150, 100, 140, 0);
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_AGENTS);
            tap(caption_point(ORB_ICON_LIST));
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSIONS);
            if (page) {
                tap(caption_point("Usage"));
                CHECK(orb_shell_surface(s_shell) == ORB_SHELL_USAGE);
                if (mode) boot(); else drag(12, 233, 200, 0);
                CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSIONS);
            }
            if (mode) boot(); else drag(12, 233, 200, 0);
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_AGENTS);
            if (mode) boot(); else drag(12, 233, 200, 0);
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
            no_actions();
        }
    }
}
static void content_centres(void)
{
    static const orb_shell_surface_t surfaces[] = {
        ORB_SHELL_SETTINGS, ORB_SHELL_AGENTS, ORB_SHELL_SESSIONS, ORB_SHELL_DOT,
        ORB_SHELL_SESSION_SETTINGS, ORB_SHELL_NOTIFICATIONS, ORB_SHELL_STACK,
        ORB_SHELL_CONTROL_CENTER, ORB_SHELL_TIME, ORB_SHELL_PLAY, ORB_SHELL_HOME,
        ORB_SHELL_UPDATES, ORB_SHELL_SOUND, ORB_SHELL_BATTERY,
    };
    static const int deltas[][2] = {{0, 130}, {0, -130}, {130, 0}, {-130, 0}};
    for (unsigned i = 0; i < sizeof(surfaces) / sizeof(*surfaces); ++i) {
        s_case = "centre belongs to content on every surface";
        navigate(surfaces[i]);
        for (unsigned d = 0; d < 4; ++d) {
            unsigned actions = s_actions;
            drag(233, 233, deltas[d][0], deltas[d][1]);
            CHECK(orb_shell_surface(s_shell) == surfaces[i]);
            CHECK(!sim_display_snapshot_transition_active());
            CHECK(s_actions == actions); no_actions();
        }
    }
}
static void face_and_edges(void)
{
    static const struct { int dx, dy; orb_shell_surface_t surface; } face[] = {
        {-150, 0, ORB_SHELL_HOME}, {150, 0, ORB_SHELL_AGENTS},
        {0, -150, ORB_SHELL_STACK}, {0, 150, ORB_SHELL_NOTIFICATIONS},
    };
    for (unsigned i = 0; i < 4; ++i) {
        s_case = "Face centre exception";
        navigate(ORB_SHELL_FACE); drag(233, 233, face[i].dx, face[i].dy);
        CHECK(orb_shell_surface(s_shell) == face[i].surface);
    }
    s_case = "Notifications only dismisses upward from bottom or Back";
    navigate(ORB_SHELL_SETTINGS); drag(233, 12, 0, 160);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATIONS);
    drag(233, 330, 0, -140); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATIONS);
    drag(233, 454, 0, -180); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    s_case = "Stack only dismisses downward from top or Back";
    drag(233, 454, 0, -160); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    drag(233, 150, 0, 140); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    drag(233, 12, 0, 180); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    s_case = "Control Center dismisses from left edge";
    drag(454, 233, -160, 0); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    drag(150, 233, 150, 0); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    drag(12, 233, 180, 0); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    no_actions();
}
/* Every trace begins on the same real control and returns to that point.
 * This catches a recognizer that checks only lift displacement. */
static void rejected_contact(lv_point_t point, unsigned kind)
{
    unsigned actions = s_actions;
    orb_shell_surface_t surface = orb_shell_surface(s_shell);
    pointer(point.x, point.y, true, 40);
    lv_obj_t *original = s_pressed_target;
    CHECK(original);
    s_probe_clicks = 0; s_probe_active = true;
    lv_obj_add_event_cb(original, probe_clicked, LV_EVENT_CLICKED, NULL);
    if (kind == 0) {
        pointer(point.x + 11, point.y, true, 40);
        unpressed(original);
        pointer(point.x, point.y, true, 40);
    } else if (kind == 1) {
        pointer(point.x, point.y - 28, true, 40);
        unpressed(original);
        pointer(point.x, point.y, true, 40);
    } else { advance(520); unpressed(original); }
    release(); advance(1000);
    s_probe_active = false;
    CHECK(s_probe_clicks == 0);
    if (original == s_pressed_target)
        lv_obj_remove_event_cb(original, probe_clicked);
    CHECK(orb_shell_surface(s_shell) == surface);
    CHECK(s_actions == actions); no_actions();
}
static void control_started_scroll(void)
{
    static const struct { orb_shell_surface_t surface; const char *caption; } cases[] = {
        {ORB_SHELL_SETTINGS, "Display"}, {ORB_SHELL_SESSION_SETTINGS, "Claude"},
        {ORB_SHELL_HOME, "Living Room"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        s_case = "vertical drag starting on a button or toggle scrolls its native list";
        navigate(cases[i].surface);
        lv_point_t point = caption_point(cases[i].caption);
        lv_obj_t *list = scroll(orb_shell_root(s_shell)); CHECK(list);
        int before = lv_obj_get_scroll_y(list);
        unsigned actions = s_actions;
        pointer(point.x, point.y, true, 40);
        lv_obj_t *original = s_pressed_target;
        for (unsigned step = 1; step <= 8; ++step)
            pointer(point.x, point.y - (int)step * 12, true, 24);
        CHECK(lv_obj_is_valid(list));
        CHECK(lv_obj_get_scroll_y(list) > before + 20);
        unpressed(original);
        release(); advance(1000);
        CHECK(orb_shell_surface(s_shell) == cases[i].surface);
        CHECK(s_actions == actions); no_actions();
    }
    s_case = "Time scroll beginning on Resume retains the paused stopwatch";
    navigate(ORB_SHELL_APPS); tap((lv_point_t){233, 233});
    for (unsigned page = 0; page < 4 && !text(orb_shell_root(s_shell), "Timer"); ++page)
        drag(140, 240, 160, 0);
    CHECK(text(orb_shell_root(s_shell), "Timer"));
    orb_time_stopwatch_reset();
    int64_t now = (int64_t)s_now * 1000;
    CHECK(orb_time_stopwatch_start(now - INT64_C(144000000)));
    for (unsigned i = 1; i <= 12; ++i)
        CHECK(orb_time_stopwatch_lap(now - INT64_C(144000000) + INT64_C(1000000) * i * i));
    orb_time_stopwatch_pause(now);
    advance(200); drag(320, 240, -160, 0);
    CHECK(text(orb_shell_root(s_shell), "Stopwatch"));
    lv_point_t point = time_caption_point("Resume");
    lv_obj_t *list = scroll(orb_shell_root(s_shell)); CHECK(list);
    int before = lv_obj_get_scroll_y(list);
    pointer(point.x, point.y, true, 40);
    lv_obj_t *original = s_pressed_target;
    for (unsigned step = 1; step <= 8; ++step)
        pointer(point.x, point.y - (int)step * 12, true, 24);
    CHECK(lv_obj_get_scroll_y(list) > before + 20);
    unpressed(original);
    release(); advance(1000);
    orb_time_snapshot_t time;
    orb_time_snapshot((int64_t)s_now * 1000, &time);
    CHECK(time.stopwatch_state == ORB_TIME_PAUSED);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME);
    no_actions(); orb_time_stopwatch_reset(); advance(200);
}
static void tap_versus_scroll(void)
{
    for (unsigned kind = 0; kind < 3; ++kind) {
        s_case = "Settings row movement and hold never open Display";
        navigate(ORB_SHELL_SETTINGS); rejected_contact(caption_point("Display"), kind);
        tap(caption_point("Display")); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_DISPLAY);

        s_case = "Settings source toggle movement and hold never changes preferences";
        navigate(ORB_SHELL_SESSION_SETTINGS);
        rejected_contact(caption_point("Claude"), kind);
        tap(caption_point("Claude"));
        orb_prefs_patch_t patch;
        CHECK(orb_model_take_prefs_request(&patch));
        CHECK(patch.sources_mask & (1u << ORB_AGENT_CLAUDE));
        orb_prefs_t defaults; orb_model_prefs_defaults(&defaults);
        orb_model_complete_prefs(&defaults, NULL); advance(300);

        s_case = "Sessions row movement and hold never open a session";
        navigate(ORB_SHELL_SESSIONS);
        lv_obj_t *list = scroll(orb_shell_root(s_shell)); CHECK(list);
        lv_obj_t *row = lv_obj_get_child(list, 0); CHECK(row);
        rejected_contact(target_point(row), kind);
        list = scroll(orb_shell_root(s_shell)); CHECK(list);
        tap(target_point(lv_obj_get_child(list, 0))); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSION);

        s_case = "Notifications row movement and hold never open a notice";
        navigate(ORB_SHELL_NOTIFICATIONS);
        list = scroll(orb_shell_root(s_shell)); CHECK(list);
        rejected_contact(target_point(lv_obj_get_child(list, 0)), kind);
        list = scroll(orb_shell_root(s_shell)); CHECK(list);
        tap(target_point(lv_obj_get_child(list, 0)));
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATION_DETAIL);

        s_case = "Time primary movement and hold never starts timer";
        navigate(ORB_SHELL_APPS); tap((lv_point_t){233, 233});
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME);
        for (unsigned page = 0; page < 4 && !text(orb_shell_root(s_shell), "Timer"); ++page)
            drag(140, 240, 160, 0);
        CHECK(text(orb_shell_root(s_shell), "Timer"));
        orb_time_timer_reset(); advance(400);
        rejected_contact(time_caption_point("Start"), kind);
        orb_time_snapshot_t time;
        orb_time_snapshot((int64_t)s_now * 1000, &time); CHECK(time.timer_state == ORB_TIME_IDLE);
        tap(time_caption_point("Start"));
        orb_time_snapshot((int64_t)s_now * 1000, &time); CHECK(time.timer_state == ORB_TIME_RUNNING);
        orb_time_timer_reset(); advance(400);

        s_case = "Play primary movement and hold never launches a game";
        orb_apps_exit(); orb_shell_apps_publish();
        navigate(ORB_SHELL_PLAY);
        rejected_contact(caption_point("Play"), kind);
        CHECK(!orb_apps_active());
        tap(caption_point("Play")); CHECK(orb_apps_active());
        orb_apps_exit(); orb_shell_apps_publish(); advance(300);

        s_case = "Home room list movement and hold never opens the room";
        navigate(ORB_SHELL_HOME);
        for (unsigned depth = 0; depth < 3 && !semantic(orb_shell_root(s_shell), "rooms:list"); ++depth)
            boot();
        lv_obj_t *rooms = semantic(orb_shell_root(s_shell), "rooms:list"); CHECK(rooms);
        lv_obj_stop_scroll_anim(rooms); lv_obj_scroll_to_y(rooms, 0, LV_ANIM_OFF); advance(400);
        lv_obj_t *room = semantic(orb_shell_root(s_shell), "room:Living Room"); CHECK(room);
        rejected_contact(target_point(room), kind);
        CHECK(semantic(orb_shell_root(s_shell), "room:Living Room"));
        room = semantic(orb_shell_root(s_shell), "room:Living Room"); CHECK(room);
        tap(target_point(room)); CHECK(semantic(orb_shell_root(s_shell), "floor-lamp"));
        s_case = "Home room row movement and hold never opens its dial";
        lv_obj_t *lamp = semantic(orb_shell_root(s_shell), "floor-lamp"); CHECK(lamp);
        rejected_contact(target_point(lamp), kind);
        if (kind < 2) CHECK(!dial_value(orb_shell_root(s_shell)));
        else if (dial_value(orb_shell_root(s_shell))) {
            /* Home intentionally supports hold-to-open accessory detail.
             * That local hold is allowed; its release must not actuate. */
            boot();
        }
        lamp = semantic(orb_shell_root(s_shell), "floor-lamp"); CHECK(lamp);
        tap(target_point(lamp)); CHECK(dial_value(orb_shell_root(s_shell)));
        s_case = "Home accessory power movement and hold never actuates";
        /* The template's identity disc at (233,140) is the light's power. */
        rejected_contact((lv_point_t){233, 140}, kind);
    }
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); orb_time_init(); orb_apps_init(ORB_APP_NEEDS_IMU);
    CHECK(orb_model_init() && orb_home_init());
    orb_model_load_demo(); orb_home_load_demo(); orb_model_set_connection(true, true);
    s_pointer = lv_indev_create(); CHECK(s_pointer);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER); lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    lv_indev_add_event_cb(s_pointer, input_event, LV_EVENT_PRESSED, NULL);
    s_shell = orb_shell_create(lv_screen_active(), action, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .brightness = 70, .clock_time = "10:09", .clock_date = "Tuesday, October 27",
        .hub_name = "Synthetic Mac", .firmware_version = "0.5.2"};
    orb_shell_update(s_shell, &status); fixtures(); advance(1400);
    honeycomb_back(); content_centres(); face_and_edges(); control_started_scroll(); tap_versus_scroll();
    orb_shell_destroy(s_shell); lv_indev_delete(s_pointer); lv_deinit();
    printf("gestures3: %u checks passed through real shell pointer sequences\n", s_checks);
    return 0;
}
