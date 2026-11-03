/* Full production shell input arbitration at the devtools' 10 ms glide cadence.
 * Input is sampled only by LVGL's timer, never forced by the test. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static const char *s_case = "setup", *s_out;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static bool s_absolute_trace, s_draw_stall;
static uint32_t s_trace_start;
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "list scroll %s at %u ms: %s:%d: %s\n", s_case, s_now, __FILE__, __LINE__, #value); \
    exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *pointer, lv_indev_data_t *data)
{
    (void)pointer; *data = s_input;
    if (s_absolute_trace) {
        unsigned elapsed = s_now - s_trace_start;
        unsigned sample = LV_MIN(400, elapsed / 10 * 10);
        data->point = (lv_point_t){233, 360 - (int)sample * 160 / 400};
        data->state = elapsed < 410 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
}
static void advance(unsigned ms)
{
    uint32_t end = s_now + ms;
    while (s_now < end) {
        s_now += LV_MIN(2, end - s_now);
        orb_shell_poll(s_shell);
        lv_timer_handler();
        if (s_draw_stall) {
            /* The injector task continues while a synchronous refresh blocks
             * LVGL. Its next read observes the endpoint after physical lift. */
            s_draw_stall = false;
            s_now = s_trace_start + 410;
        }
    }
}
static void pointer(int x, int y, bool down, unsigned ms)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    advance(ms);
}
static void release(void) { pointer(s_input.point.x, s_input.point.y, false, 64); }
static lv_obj_t *text(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *shown = lv_label_get_text(root);
        if (!strcmp(shown, caption)) return root;
        /* Caption2 remains readable at the shoulder; legal long names can
         * therefore use the kit's terminal ellipsis instead of smaller ink. */
        const char *dots = strstr(shown, "...");
        if (!dots) dots = strstr(shown, "…");
        if (dots && dots - shown >= 4 && !strncmp(shown, caption, dots - shown)) return root;
    }
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
static lv_obj_t *list(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_is_scrollable(root) && lv_obj_get_scroll_dir(root) & LV_DIR_VER &&
        lv_obj_get_scroll_bottom(root) + lv_obj_get_scroll_y(root) > 0) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = list(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}
static bool rooms_root(lv_obj_t *root)
{
    const char *id = lv_obj_get_user_data(root);
    if ((uintptr_t)id > 4096 && !strcmp(id, "rooms:list") && lv_obj_is_visible(root)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        if (rooms_root(lv_obj_get_child(root, i))) return true;
    return false;
}
static void unwind_home(void)
{
    /* A repeated pointer-cadence run resumes the room exercised previously.
     * Use local Back explicitly; ordinary navigation no longer resets Home. */
    for (unsigned i = 0; i < 4 && !rooms_root(orb_shell_root(s_shell)); i++) {
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
        orb_shell_back(s_shell); advance(1400);
    }
    CHECK(rooms_root(orb_shell_root(s_shell)));
    lv_obj_t *rooms = list(orb_shell_root(s_shell)); CHECK(rooms);
    lv_obj_stop_scroll_anim(rooms); lv_obj_scroll_to_y(rooms, 0, LV_ANIM_OFF); advance(400);
}
static void tap_text(const char *caption)
{
    lv_obj_t *label = text(orb_shell_root(s_shell), caption);
    if (!label) fprintf(stderr, "Missing tap caption: %s\n", caption);
    CHECK(label);
    lv_obj_t *control = lv_obj_is_clickable(label) ? label : lv_obj_get_parent(label);
    lv_area_t bounds; lv_obj_get_coords(control, &bounds);
    pointer((bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2, true, 40);
    release(); advance(1000);
}
static void shot(const char *name)
{
    if (!s_out) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", s_out, name);
    lv_refr_now(s_display); CHECK(sim_display_save_png(path));
}
static void goto_surface(orb_shell_surface_t surface)
{
    orb_shell_navigate(s_shell, surface);
    if (surface == ORB_SHELL_NOTIFICATIONS) {
        /* Isolate scroll ownership from the live seconds-age presentation.
         * Fresh notices currently rebuild once/second; each trace must retain
         * the same owner. Older synthetic notices change only once/minute. */
        for (unsigned i = 0; i < 10; ++i) {
            char id[32]; snprintf(id, sizeof(id), "scroll-card-%u", i);
            orb_shell_fixture_notification_age(s_shell, id, 3600000);
        }
    }
    advance(1400);
    CHECK(orb_shell_surface(s_shell) == surface);
}
static void drag(int x1, int y1, int x2, int y2, unsigned ms)
{
    pointer(x1, y1, true, 10);
    for (unsigned at = 10; at <= ms; at += 10)
        pointer(x1 + (x2 - x1) * (int)at / (int)ms,
                y1 + (y2 - y1) * (int)at / (int)ms, true, 10);
}
static unsigned s_actions;
static void action(orb_shell_action_t event, void *ctx) { (void)event; (void)ctx; ++s_actions; }
static void fixtures(void)
{
    orb_session_t sessions[8] = {0};
    for (unsigned i = 0; i < 8; ++i) {
        snprintf(sessions[i].id, sizeof(sessions[i].id), "scroll-session-%u", i);
        snprintf(sessions[i].title, sizeof(sessions[i].title), "Build project %u", i + 1);
        sessions[i].agent = i % 2 ? ORB_AGENT_CODEX : ORB_AGENT_CLAUDE;
        sessions[i].state = ORB_SESSION_BUSY;
        sessions[i].updated = 1000 - i;
    }
    CHECK(orb_model_sessions_chunk(sessions, 8, true, false));
    for (unsigned i = 0; i < 10; ++i) {
        orb_card_t card = {.kind = ORB_CARD_ATTENTION, .priority = ORB_PRIORITY_NORMAL};
        snprintf(card.id, sizeof(card.id), "scroll-card-%u", i);
        snprintf(card.title, sizeof(card.title), "Project %u updated", i + 1);
        snprintf(card.body, sizeof(card.body), "The latest changes are ready to review.");
        CHECK(orb_model_upsert_card(&card));
    }
    CHECK(orb_time_stopwatch_start(0));
    for (unsigned i = 1; i <= 12; ++i) CHECK(orb_time_stopwatch_lap(INT64_C(1000000) * i * i));
    orb_time_stopwatch_pause(INT64_C(144000000));
}
static void arc(float radius, float from, float to, unsigned duration)
{
    for (unsigned at = 0; at <= duration; at += 10) {
        float angle = (from + (to - from) * at / duration) * (float)M_PI / 180;
        pointer((int)lroundf(233 + radius * cosf(angle)),
                (int)lroundf(233 + radius * sinf(angle)), true, 10);
    }
}
static void named_shot(const char *surface, const char *stage)
{
    char name[120]; snprintf(name, sizeof(name), "%s-%s", surface, stage); shot(name);
}
static void no_actions(void)
{
    orb_answer_t answer; orb_home_request_t request;
    CHECK(!orb_model_peek_answer(&answer)); CHECK(!orb_home_take_request(&request));
}
static bool settled_on_row(lv_obj_t *scroll)
{
    int y = lv_obj_get_scroll_y(scroll), extent = y + lv_obj_get_scroll_bottom(scroll);
    if (y == 0 || y == extent) return true;
    int origin = 0; bool first = true;
    for (unsigned i = 0; i < lv_obj_get_child_count(scroll); ++i) {
        lv_obj_t *row = lv_obj_get_child(scroll, i);
        if (lv_obj_is_hidden(row) || lv_obj_is_floating(row)) continue;
        if (first) { origin = lv_obj_get_y(row); first = false; }
        if (y == LV_CLAMP(0, lv_obj_get_y(row) - origin, extent)) return true;
    }
    return false;
}
static int tracked_drag(lv_obj_t *scroll, int y1, int y2)
{
    bool anchored = false;
    int anchor_y = 0, anchor_scroll = 0, first_read = y1, last_read = y1;
    bool sampled = false;
    int initial_scroll = lv_obj_get_scroll_y(scroll);
    int extent = initial_scroll + lv_obj_get_scroll_bottom(scroll);
    pointer(233, y1, true, 10);
    if (lv_indev_get_state(s_pointer) == LV_INDEV_STATE_PRESSED) {
        lv_point_t read; lv_indev_get_point(s_pointer, &read);
        first_read = last_read = read.y;
        sampled = true;
    }
    for (unsigned at = 10; at <= 400; at += 10) {
        pointer(233, y1 + (y2 - y1) * (int)at / 400, true, 10);
        CHECK(lv_obj_is_valid(scroll));
        int offset = lv_obj_get_scroll_y(scroll);
        lv_point_t read; lv_indev_get_point(s_pointer, &read);
        if (lv_indev_get_state(s_pointer) == LV_INDEV_STATE_PRESSED) {
            if (!sampled) { first_read = read.y; sampled = true; }
            last_read = read.y;
        }
        if (lv_indev_get_state(s_pointer) != LV_INDEV_STATE_PRESSED || read.x != 233 ||
            offset <= 0 || offset >= extent || (!anchored && offset == initial_scroll)) continue;
        if (!anchored) { anchor_y = read.y; anchor_scroll = offset; anchored = true; }
        CHECK(abs(offset - (anchor_scroll + anchor_y - read.y)) <= 2);
    }
    CHECK(sampled && last_read != first_read);
    return abs(last_read - first_read);
}
static void stalled_drag(void);
static void exercise(const char *name, orb_shell_surface_t surface, bool room)
{
    s_case = name;
    goto_surface(surface);
    if (surface == ORB_SHELL_HOME) {
        unwind_home();
        if (room) tap_text("Living Room");
    }
    if (surface == ORB_SHELL_TIME && !text(orb_shell_root(s_shell), "Stopwatch")) {
        drag(330, 245, 130, 245, 400); release(); advance(1400);
        CHECK(text(orb_shell_root(s_shell), "Stopwatch"));
    }
    lv_obj_t *scroll = list(orb_shell_root(s_shell));
    named_shot(name, "before");
    if (!scroll) {
        /* Design 2.0 Updates is a bounded hero with one bottom action at
         * every text size. It deliberately has no scroll owner. A short
         * Battery fixture may also fit without overflowing its viewport. */
        CHECK(surface == ORB_SHELL_UPDATES || surface == ORB_SHELL_BATTERY);
        unsigned actions = s_actions;
        drag(233, 360, 233, 200, 400); release(); advance(1000);
        CHECK(orb_shell_surface(s_shell) == surface); CHECK(s_actions == actions);
        named_shot(name, "after"); no_actions(); return;
    }
    lv_obj_stop_scroll_anim(scroll); lv_obj_scroll_to_y(scroll, 0, LV_ANIM_OFF); advance(400);
    int extent = lv_obj_get_scroll_y(scroll) + lv_obj_get_scroll_bottom(scroll);
    CHECK(extent > 0);
    lv_area_t bounds; lv_obj_get_coords(scroll, &bounds);
    /* Updates'84px prose viewport needs a longer in-bounds trace than a
     * list's24px inset leaves; keep12px of clearance on that compact reader. */
    int inset = surface == ORB_SHELL_UPDATES ? 12 : 24;
    int y1 = LV_MIN(360, bounds.y2 - inset), y2 = LV_MAX(bounds.y1 + inset, y1 - 160);
    unsigned actions = s_actions;
    /* The injector's 10ms samples need not align with LVGL's 16/32ms reads.
     * Bound scrolling by the travel actually delivered to the recognizer;
     * tracked_drag also verifies every unconstrained sample tracks 1:1. */
    int distance = tracked_drag(scroll, y1, y2);
    CHECK(lv_obj_is_valid(scroll));
    int held = lv_obj_get_scroll_y(scroll);
    printf("%s drag: extent=%d held=%d distance=%d sampled=%d\n", name, extent, held, y1 - y2, distance); fflush(stdout);
    CHECK(held >= LV_MIN(extent, distance - 20));
    CHECK(held <= distance + 4);
    named_shot(name, "held");
    release(); advance(1400);
    CHECK(orb_shell_surface(s_shell) == surface); CHECK(lv_obj_is_valid(scroll));
    CHECK(lv_obj_get_scroll_y(scroll) >= LV_MIN(extent, held) - 2);
    CHECK(s_actions == actions); no_actions(); named_shot(name, "after");
    bool stepped = surface == ORB_SHELL_SETTINGS || surface == ORB_SHELL_SESSIONS ||
        surface == ORB_SHELL_NOTIFICATIONS || surface == ORB_SHELL_HOME;
    if (stepped) CHECK(settled_on_row(scroll));
    lv_obj_stop_scroll_anim(scroll); lv_obj_scroll_to_y(scroll, 0, LV_ANIM_OFF); advance(300);
    arc(215, 40, 140, 700);
    int clockwise = lv_obj_get_scroll_y(scroll);
    CHECK(clockwise >= LV_MIN(extent, 20)); release(); advance(1400);
    CHECK(lv_obj_get_scroll_y(scroll) >= clockwise - 2);
    if (stepped) CHECK(settled_on_row(scroll));
    named_shot(name, "rim-down");
    lv_obj_stop_scroll_anim(scroll);
    int start = LV_MIN(extent, 240); lv_obj_scroll_to_y(scroll, start, LV_ANIM_OFF); advance(400);
    arc(215, 140, 40, 700);
    int counterclockwise = lv_obj_get_scroll_y(scroll);
    if (surface == ORB_SHELL_TIME) {
        printf("%s rim-up: start=%d counterclockwise=%d\n", name, start, counterclockwise); fflush(stdout);
    }
    CHECK(counterclockwise <= start - LV_MIN(start, 20)); release(); advance(1400);
    CHECK(lv_obj_get_scroll_y(scroll) <= counterclockwise + 2);
    if (stepped) CHECK(settled_on_row(scroll));
    named_shot(name, "rim-up");
    lv_obj_stop_scroll_anim(scroll);
    start = LV_MIN(extent, 240); lv_obj_scroll_to_y(scroll, start, LV_ANIM_OFF); advance(400);
    int sampled_distance = tracked_drag(scroll, y2, y1);
    held = lv_obj_get_scroll_y(scroll);
    /* At32ms sampling the injector can move before the first DOWN read and
     * after the last MOVE read. Compare with the measured pointer travel;
     * tracked_drag also checks every unconstrained sample tracks1:1. */
    CHECK(held <= LV_MAX(0, start - sampled_distance + 20));
    release(); advance(1400);
    CHECK(lv_obj_get_scroll_y(scroll) <= LV_MAX(0, held) + 2);
    CHECK(orb_shell_surface(s_shell) == surface); CHECK(s_actions == actions); no_actions();
    if (extent > 200) {
        lv_obj_stop_scroll_anim(scroll);
        start = LV_MIN(extent - 100, 240);
        lv_obj_scroll_to_y(scroll, start, LV_ANIM_OFF); advance(400);
        drag(233, y1, 233, y1 - 80, 300);
        lv_point_t last_read; lv_indev_get_point(s_pointer, &last_read);
        int reversal = lv_obj_get_scroll_y(scroll) - (y1 - 48 - last_read.y);
        pointer(233, y1 - 48, false, 64); advance(1400);
        CHECK(lv_obj_is_valid(scroll));
        printf("%s reversal: expected<=%d settled=%d\n", name, LV_MAX(0, reversal) + 2, lv_obj_get_scroll_y(scroll)); fflush(stdout);
        CHECK(lv_obj_get_scroll_y(scroll) <= LV_MAX(0, reversal) + 2);
        CHECK(orb_shell_surface(s_shell) == surface); CHECK(s_actions == actions); no_actions();
    }
    /* Crossing the click threshold and returning to the pressed row must not
     * activate it. This catches cancellation based only on lift displacement. */
    lv_obj_stop_scroll_anim(scroll); lv_obj_scroll_to_y(scroll, 0, LV_ANIM_OFF); advance(400);
    drag(233, y1, 233, y1 - 20, 120);
    drag(233, y1 - 20, 233, y1, 120); release(); advance(1000);
    CHECK(orb_shell_surface(s_shell) == surface); CHECK(lv_obj_is_valid(scroll));
    CHECK(s_actions == actions); no_actions();
    lv_obj_scroll_to_y(scroll, 0, LV_ANIM_OFF); advance(400);
    drag(233, y1, 253, y1, 120);
    drag(253, y1, 233, y1, 120); release(); advance(1000);
    CHECK(orb_shell_surface(s_shell) == surface); CHECK(lv_obj_is_valid(scroll));
    CHECK(s_actions == actions); no_actions();
    /* A release may be the first sample beyond the threshold. */
    pointer(233, y1, true, 40);
    pointer(233, y1 - 24, false, 64); advance(1000);
    CHECK(orb_shell_surface(s_shell) == surface); CHECK(lv_obj_is_valid(scroll));
    CHECK(s_actions == actions); no_actions();
}
static void regressions(void)
{
    fixtures(); advance(1000);
    const struct { const char *name; orb_shell_surface_t surface; bool room; } cases[] = {
        {"settings", ORB_SHELL_SETTINGS, false}, {"sessions", ORB_SHELL_SESSIONS, false},
        {"notifications", ORB_SHELL_NOTIFICATIONS, false},
        {"home-rooms", ORB_SHELL_HOME, false}, {"home-room", ORB_SHELL_HOME, true},
        {"time-laps", ORB_SHELL_TIME, false},
        {"sound", ORB_SHELL_SOUND, false}, {"battery", ORB_SHELL_BATTERY, false},
        {"updates", ORB_SHELL_UPDATES, false},
    };
    for (unsigned p = 0; p < 2; ++p) {
        lv_timer_set_period(lv_indev_get_read_timer(s_pointer), p ? 32 : LV_DEF_REFR_PERIOD);
        stalled_drag();
        for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i)
            exercise(cases[i].name, cases[i].surface, cases[i].room);
    }
    s_case = "tap and place navigation positive controls";
    goto_surface(ORB_SHELL_SETTINGS);
    /* Re-entry now restores the scroll position exercised above. Reveal the
     * intended row before testing a positive navigation tap. */
    lv_obj_t *settings = list(orb_shell_root(s_shell)); CHECK(settings);
    lv_obj_stop_scroll_anim(settings); lv_obj_scroll_to_y(settings, 0, LV_ANIM_OFF); advance(400);
    tap_text("Display");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_DISPLAY);
    goto_surface(ORB_SHELL_HOME); unwind_home(); tap_text("Living Room"); tap_text("Floor Lamp");
    CHECK(dial_value(orb_shell_root(s_shell))); no_actions();
    pointer(17, 233, true, 40); drag(17, 233, 217, 233, 400); release(); advance(1400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    CHECK(text(orb_shell_root(s_shell), "Floor Lamp"));
    CHECK(!dial_value(orb_shell_root(s_shell)));
    goto_surface(ORB_SHELL_FACE); named_shot("face", "rest");
    drag(320, 240, 160, 240, 400); release(); advance(1400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    goto_surface(ORB_SHELL_NOTIFICATIONS);
    lv_obj_t *notifications = list(orb_shell_root(s_shell)); CHECK(notifications);
    lv_obj_stop_scroll_anim(notifications); lv_obj_scroll_to_y(notifications, 0, LV_ANIM_OFF); advance(400);
    lv_obj_t *row = lv_obj_get_child(notifications, 0); CHECK(row && lv_obj_is_clickable(row));
    lv_area_t bounds; lv_obj_get_coords(row, &bounds);
    pointer((bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2, true, 40);
    release(); advance(1400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATION_DETAIL);
    named_shot("card", "rest"); no_actions();
}
static void stall_scroll(lv_event_t *event)
{
    if (s_absolute_trace && s_now - s_trace_start < 100) s_draw_stall = true;
}
static void stalled_drag(void)
{
    s_case = "Settings injector progresses during stalled LVGL refresh";
    goto_surface(ORB_SHELL_SETTINGS);
    lv_obj_t *scroll = list(orb_shell_root(s_shell)); CHECK(scroll);
    shot("settings-stalled-before");
    lv_obj_add_event_cb(scroll, stall_scroll, LV_EVENT_SCROLL, NULL);
    s_trace_start = s_now; s_absolute_trace = true;
    advance(450); advance(1200);
    int moved = lv_obj_get_scroll_y(scroll);
    printf("stalled Settings: moved=%d for 160px absolute injector drag\n", moved); fflush(stdout);
    shot("settings-stalled-after");
    CHECK(moved >= 140);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS); no_actions();
    s_absolute_trace = false;
    lv_obj_remove_event_cb(scroll, stall_scroll);
}
int main(int argc, char **argv)
{
    bool ax = argc > 1 && !strcmp(argv[1], "ax3");
    if (argc > 2) s_out = argv[2];
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); orb_time_init(); orb_apps_init(ORB_APP_NEEDS_IMU);
    if (ax) { orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3); orb_fonts_set_bold_text(true); orb_ui_set_reduce_transparency(true); }
    CHECK(orb_model_init() && orb_home_init());
    orb_model_load_demo(); orb_model_set_connection(true, true); orb_home_load_demo();
    s_pointer = lv_indev_create(); CHECK(s_pointer);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER); lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer); orb_input_mark_synthetic(s_pointer);
    s_shell = orb_shell_create(lv_screen_active(), action, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .usb = true,
        .clock_time = "10:09", .clock_date = "Tuesday, October 27", .hub_name = "Synthetic Mac",
        .firmware_version = "0.5.1",
        .update_check_result = "New firmware includes improved navigation and list scrolling."};
    orb_shell_update(s_shell, &status); advance(1400);
    regressions();
    printf("list scroll: %u checks passed\n", s_checks);
    orb_shell_destroy(s_shell); lv_indev_delete(s_pointer); lv_deinit(); return 0;
}
