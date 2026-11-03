/* Board regression traces through LVGL's real pointer read timer. The board
 * keeps a physical pointer while developer commands use a second, synthetic
 * pointer. Both paths must own the place shown after every route. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_kit.h"
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
static lv_indev_t *s_pointers[2];
static lv_indev_data_t s_inputs[2];
static unsigned s_source;
static orb_shell_t *s_shell;
static orb_shell_snapshot_t s_status;
static uint8_t s_expected[SIM_D * SIM_D * 3], s_actual[sizeof(s_expected)];
static int s_home_rest;
static void save(const char *name);

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "gestures4 nav %s pointer=%u at %u ms: %s:%d: %s\n", \
        s_case, s_source, s_now, __FILE__, __LINE__, #value); exit(1); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{
    *data = s_inputs[input == s_pointers[1]];
}
static void action(orb_shell_action_t value, void *ctx) { (void)value; (void)ctx; ++s_actions; }
static void advance(unsigned ms)
{
    static const char *last_case;
    if (getenv("ORB_TRACE_NAV") && last_case != s_case) {
        fprintf(stderr, "gestures4 nav stage: %s pointer=%u at %u ms\n", s_case, s_source, s_now);
        last_case = s_case;
    }
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
    s_inputs[s_source].point = (lv_point_t){x, y};
    s_inputs[s_source].state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    advance(ms);
}
static void release(void)
{
    lv_point_t point = s_inputs[s_source].point;
    pointer(point.x, point.y, false, 64);
}
static void drag_trace(int x, int y, int dx, int dy, unsigned settle)
{
    /* Match devtools glide's 10ms linear samples over the lead's 300ms trace. */
    pointer(x, y, true, 10);
    for (unsigned step = 1; step <= 30; ++step)
        pointer(x + dx * (int)step / 30, y + dy * (int)step / 30, true, 10);
    release(); advance(settle);
}
static void drag(int x, int y, int dx, int dy) { drag_trace(x, y, dx, dy, 1100); }
static void tap(int x, int y)
{
    pointer(x, y, true, 60); release(); advance(1100);
}
static void place(orb_shell_surface_t expected)
{
    CHECK(orb_shell_surface(s_shell) == expected);
    CHECK(orb_shell_nav_current(s_shell) == expected);
}
static void navigate(orb_shell_surface_t surface)
{
    release();
    orb_shell_navigate(s_shell, surface); advance(1400);
    place(surface);
}
static void boot(void)
{
    orb_button_event_t event = {.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_SHORT,
        .at_ms = s_now, .synthetic = s_source == 1};
    orb_shell_button(s_shell, &event); advance(1400);
}
static lv_obj_t *semantic(lv_obj_t *root, const char *id)
{
    const char *data = lv_obj_get_user_data(root);
    if ((intptr_t)data > 4096 && !strcmp(data, id) && lv_obj_is_visible(root)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = semantic(lv_obj_get_child(root, i), id);
        if (found) return found;
    }
    return NULL;
}
static void home_root(void)
{
    /* Routing preserves nested Home context. These root-place traces must
     * explicitly close the retained accessory/room/Scenes pages with BOOT. */
    for (unsigned i = 0; i < 4 && !semantic(orb_shell_root(s_shell), "rooms:list"); i++) {
        place(ORB_SHELL_HOME); boot();
    }
    place(ORB_SHELL_HOME);
    lv_obj_t *list = semantic(orb_shell_root(s_shell), "rooms:list"); CHECK(list);
    /* Capture the first fresh entry's actual resting pose. A fresh Home now
     * fills the circle under its title; native zero pulls row one to centre. */
    if (!s_home_rest) { s_home_rest = lv_obj_get_scroll_y(list); CHECK(s_home_rest > 0); }
    lv_obj_stop_scroll_anim(list); lv_obj_scroll_to_y(list, s_home_rest, LV_ANIM_OFF); advance(400);
    CHECK(lv_obj_get_scroll_y(list) == s_home_rest);
}
static lv_obj_t *text(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), caption)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = text(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}
static void centre_stays(orb_shell_surface_t surface)
{
    place(surface);
    if (surface == ORB_SHELL_AGENTS) {
        /* The former header shortcut violated v3: even a horizontal contact
         * above the content belongs to Agents, never Sessions or Usage. */
        drag(180, 100, 140, 0); place(surface);
        drag(310, 100, -140, 0); place(surface);
    }
    drag(233, 190, 0, 200); place(surface);
    drag(233, 390, 0, -200); place(surface);
}
static void no_actions(void)
{
    orb_answer_t answer; orb_home_request_t home; orb_prefs_patch_t prefs;
    CHECK(!orb_model_peek_answer(&answer));
    CHECK(!orb_home_take_request(&home));
    CHECK(!orb_model_take_prefs_request(&prefs));
    CHECK(!s_actions);
}
static void face_swipes(void)
{
    const orb_shell_surface_t destinations[] = {ORB_SHELL_HOME, ORB_SHELL_AGENTS};
    for (unsigned i = 0; i < 2; ++i) {
        s_case = "Face swipe direction agrees with live destination and pixels";
        navigate(destinations[i]);
        if (destinations[i] == ORB_SHELL_HOME) home_root();
        sim_display_rgb(s_expected);
        save(i ? "agents-direct-reference" : "home-direct-reference");
        navigate(ORB_SHELL_FACE);
        drag(i ? 100 : 360, 233, i ? 260 : -260, 0);
        place(destinations[i]); sim_display_rgb(s_actual);
        save(i ? "agents-face-swipe" : "home-face-swipe");
        CHECK(!memcmp(s_expected, s_actual, sizeof(s_expected)));
        centre_stays(destinations[i]);
        boot(); place(ORB_SHELL_FACE);
        drag(i ? 100 : 360, 233, i ? 260 : -260, 0);
        drag(12, 233, 200, 0); place(ORB_SHELL_FACE);
    }
    s_case = "Face centre vertical exception still opens both layers";
    drag(233, 190, 0, 200); place(ORB_SHELL_NOTIFICATIONS);
    boot(); place(ORB_SHELL_FACE);
    drag(233, 390, 0, -200); place(ORB_SHELL_STACK);
    boot(); place(ORB_SHELL_FACE);
}
static void home_fresh_entry(void)
{
    s_case = "Home room and layer Back retain the landing scroll";
    navigate(ORB_SHELL_FACE); navigate(ORB_SHELL_HOME); home_root();
    lv_obj_t *list = semantic(orb_shell_root(s_shell), "rooms:list"); CHECK(list);
    const int positions[] = {0, s_home_rest, 180};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(*positions); ++i) {
        lv_obj_scroll_to_y(list, positions[i], LV_ANIM_OFF); advance(400);
        CHECK(lv_obj_get_scroll_y(list) == positions[i]);
        lv_obj_t *room = semantic(orb_shell_root(s_shell),
            i == 2 ? "room:Kitchen" : "room:Living Room"); CHECK(room);
        lv_area_t bounds; CHECK(orb_round_list_row_bounds(room, &bounds));
        tap((bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2);
        CHECK(!semantic(orb_shell_root(s_shell), "rooms:list"));
        no_actions(); boot(); place(ORB_SHELL_HOME);
        list = semantic(orb_shell_root(s_shell), "rooms:list"); CHECK(list);
        CHECK(lv_obj_get_scroll_y(list) == positions[i]);
        navigate(ORB_SHELL_CONTROL_CENTER); boot(); place(ORB_SHELL_HOME);
        list = semantic(orb_shell_root(s_shell), "rooms:list"); CHECK(list);
        CHECK(lv_obj_get_scroll_y(list) == positions[i]);
    }

    s_case = "fresh Face swipe entry starts Home at its resting position";
    boot(); place(ORB_SHELL_FACE);
    drag(360, 233, -260, 0); place(ORB_SHELL_HOME);
    list = semantic(orb_shell_root(s_shell), "rooms:list"); CHECK(list);
    CHECK(lv_obj_get_scroll_y(list) == s_home_rest);
    lv_obj_scroll_to_y(list, 180, LV_ANIM_OFF); advance(400);
    boot(); place(ORB_SHELL_FACE);

    s_case = "fresh Apps entry starts Home at its resting position";
    navigate(ORB_SHELL_APPS); tap(146, 183); place(ORB_SHELL_HOME);
    list = semantic(orb_shell_root(s_shell), "rooms:list"); CHECK(list);
    CHECK(lv_obj_get_scroll_y(list) == s_home_rest);
    no_actions();
}

static void early_home_taps(void)
{
    /* The original tests waited through settlement and twisted before a tap.
     * A tap as the destination appears must work without that preparation. */
    s_case = "Home room row tap follows its visible moving bounds";
    navigate(ORB_SHELL_HOME); home_root();
    s_case = "Home landing horizontal swipes keep the named room list";
    drag(330, 233, -190, 0); place(ORB_SHELL_HOME);
    CHECK(semantic(orb_shell_root(s_shell), "room:Living Room"));
    drag(140, 233, 190, 0); place(ORB_SHELL_HOME);
    CHECK(semantic(orb_shell_root(s_shell), "room:Living Room"));
    no_actions();
    s_case = "Home room row tap follows its visible moving bounds";
    navigate(ORB_SHELL_FACE);
    drag_trace(360, 233, -260, 0, 0);
    lv_obj_t *row = semantic(orb_shell_root(s_shell), "room:Living Room"); CHECK(row);
    lv_area_t bounds; lv_obj_get_coords(row, &bounds);
    tap((bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2);
    place(ORB_SHELL_HOME);
    CHECK(!semantic(orb_shell_root(s_shell), "room:Living Room"));
    const unsigned waits[] = {96, 150, 250, 700};
    for (unsigned i = 0; i < sizeof(waits) / sizeof(*waits); ++i) {
        s_case = "Home room tap shortly after the Face swipe";
        home_root();
        navigate(ORB_SHELL_FACE);
        drag_trace(360, 233, -260, 0, waits[i]);
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
        row = semantic(orb_shell_root(s_shell), "room:Living Room"); CHECK(row);
        lv_obj_get_coords(row, &bounds);
        tap((bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2);
        place(ORB_SHELL_HOME);
        CHECK(!semantic(orb_shell_root(s_shell), "room:Living Room"));
        no_actions();
    }
    home_root();
}
static void routes(void)
{
    const orb_shell_surface_t destinations[] = {ORB_SHELL_AGENTS, ORB_SHELL_HOME};
    for (unsigned i = 0; i < 2; ++i) {
        orb_shell_surface_t surface = destinations[i];
        int x = i ? 146 : 233, y = i ? 183 : 133;
        s_case = "honeycomb icon route owns content and returns to Face";
        navigate(ORB_SHELL_FACE); navigate(ORB_SHELL_APPS);
        tap(x, y); centre_stays(surface);
        boot(); place(ORB_SHELL_FACE);
        s_case = "Face Apps tap then honeycomb route owns content";
        tap(233, 390); place(ORB_SHELL_APPS);
        tap(x, y); centre_stays(surface);
        boot(); place(ORB_SHELL_FACE);
        s_case = "Face BOOT Apps then honeycomb route owns content";
        boot(); place(ORB_SHELL_APPS);
        tap(x, y); centre_stays(surface);
        boot(); place(ORB_SHELL_FACE);
        s_case = "developer direct navigation owns content";
        navigate(surface); centre_stays(surface);
        boot(); place(ORB_SHELL_FACE);
        s_case = "direct navigation dismisses an existing layer";
        navigate(ORB_SHELL_NOTIFICATIONS); navigate(surface); centre_stays(surface);
        CHECK(!text(orb_shell_root(s_shell), "Notifications"));
    }
    s_case = "Face Home complication tap owns content";
    /* Design 2.0 Home is the middle80px complication, not the old text slot. */
    navigate(ORB_SHELL_FACE); tap(233, 316); centre_stays(ORB_SHELL_HOME);
    boot(); place(ORB_SHELL_FACE);
    s_case = "Stack Scenes card enters Home with content ownership";
    navigate(ORB_SHELL_STACK);
    for (unsigned i = 0; i < ORB_MODEL_MAX_CARDS + 7 && !text(orb_shell_root(s_shell), "Scenes"); ++i) {
        CHECK(orb_shell_twist(s_shell, 1)); advance(600);
    }
    CHECK(text(orb_shell_root(s_shell), "Scenes"));
    tap(290, 302); centre_stays(ORB_SHELL_HOME);
    no_actions();
}
static void direct_layer_replacement(void)
{
    const orb_shell_surface_t bases[] = {ORB_SHELL_FACE, ORB_SHELL_APPS};
    const orb_shell_surface_t layers[] = {ORB_SHELL_NOTIFICATIONS, ORB_SHELL_STACK, ORB_SHELL_CONTROL_CENTER};
    for (unsigned base = 0; base < sizeof(bases) / sizeof(*bases); ++base) {
        for (unsigned layer = 0; layer < sizeof(layers) / sizeof(*layers); ++layer) {
            s_case = "direct plain-place route dismisses layer history before recording Back";
            navigate(ORB_SHELL_FACE); navigate(bases[base]); navigate(layers[layer]);
            navigate(ORB_SHELL_SETTINGS); centre_stays(ORB_SHELL_SETTINGS);
            boot(); place(bases[base]);
        }
        s_case = "direct plain-place route dismisses every nested layer";
        navigate(ORB_SHELL_FACE); navigate(bases[base]);
        for (unsigned layer = 0; layer < sizeof(layers) / sizeof(*layers); ++layer)
            navigate(layers[layer]);
        navigate(ORB_SHELL_SETTINGS);
        boot(); place(bases[base]);
    }
    no_actions();
}
static void hold_drag(int x, int y, int dx, int dy)
{
    pointer(x, y, true, 40);
    for (unsigned step = 1; step <= 20; ++step)
        pointer(x + dx * (int)step / 20, y + dy * (int)step / 20, true, 20);
    advance(80);
}
static void save(const char *name)
{
    const char *directory = getenv("ORB_QA_NAV_SHOTS");
    if (!directory) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    CHECK(sim_display_save_png(path));
}
static void layer_back_axes(void)
{
    const orb_shell_surface_t layers[] = {ORB_SHELL_NOTIFICATIONS, ORB_SHELL_STACK};
    for (unsigned i = 0; i < sizeof(layers) / sizeof(*layers); ++i) {
        s_case = "edge Back dismisses a layer along its arrival axis";
        navigate(ORB_SHELL_FACE); navigate(layers[i]);
        hold_drag(233, i ? 12 : 454, 0, i ? 160 : -160);
        sim_display_rgb(s_expected);
        release(); advance(1400); place(ORB_SHELL_FACE);
        navigate(layers[i]);
        hold_drag(12, 233, 160, 0);
        sim_display_rgb(s_actual);
        CHECK(!memcmp(s_expected, s_actual, sizeof(s_expected)));
        save(i ? "stack-back-drag" : "notifications-back-drag");
        release(); advance(1400); place(ORB_SHELL_FACE);
    }
}
static void nested_back(void)
{
    s_case = "Time edge Back closes quick timers before leaving the app";
    navigate(ORB_SHELL_FACE); navigate(ORB_SHELL_TIME);
    /* Return to Timer even if earlier traces retained another local page. */
    for (unsigned i = 0; i < 4 && !text(orb_shell_root(s_shell), "Quick timers"); ++i)
        drag(110, 240, 200, 0);
    lv_obj_t *quick = text(orb_shell_root(s_shell), "Quick timers"); CHECK(quick);
    lv_area_t area; lv_obj_get_coords(quick, &area);
    tap((area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
    CHECK(text(orb_shell_root(s_shell), ORB_ICON_X));
    save("time-quick-timers");
    drag(12, 233, 200, 0); place(ORB_SHELL_TIME);
    CHECK(!text(orb_shell_root(s_shell), ORB_ICON_X));
    drag(12, 233, 200, 0); place(ORB_SHELL_FACE);

    s_case = "gallery edge Back unwinds local Face mode";
    navigate(ORB_SHELL_FACE);
    pointer(233, 220, true, 650);
    CHECK(!text(orb_shell_root(s_shell), "Play"));
    release(); advance(1200);
    CHECK(text(orb_shell_root(s_shell), "Edit"));
    save("face-gallery");
    drag(12, 233, 200, 0); place(ORB_SHELL_FACE);
    CHECK(!text(orb_shell_root(s_shell), "Edit"));
    no_actions();
}
static void face_capture_freshness(void)
{
    s_case = "Back previews the updated Face clock before settlement";
    for (unsigned fresh = 0; fresh < 2; ++fresh) {
        strcpy(s_status.clock_time, "10:09"); orb_shell_update(s_shell, &s_status);
        navigate(ORB_SHELL_FACE);
        strcpy(s_status.clock_time, "11:38"); orb_shell_update(s_shell, &s_status);
        if (fresh) {
            /* Force a complete source rebuild to produce the reference frame,
             * rather than relying on the ordinary in-place clock update. */
            s_status.net = ORB_SHELL_NET_FAILED; orb_shell_update(s_shell, &s_status);
            s_status.net = ORB_SHELL_NET_CONNECTED; orb_shell_update(s_shell, &s_status);
        }
        advance(1200); boot(); place(ORB_SHELL_APPS);
        hold_drag(12, 233, 160, 0);
        sim_display_rgb(fresh ? s_expected : s_actual);
        release(); advance(1400); place(ORB_SHELL_FACE);
    }
    CHECK(!memcmp(s_expected, s_actual, sizeof(s_expected)));
    strcpy(s_status.clock_time, "10:09"); orb_shell_update(s_shell, &s_status);
}
static void remaining_surfaces(void)
{
    /* Complement gestures3's populated app/list coverage with empty detail,
     * voice and utility routes. Ownership must not depend on having content. */
    const orb_shell_surface_t surfaces[] = {
        ORB_SHELL_SESSION, ORB_SHELL_CARD, ORB_SHELL_DISPLAY, ORB_SHELL_ACCESSIBILITY,
        ORB_SHELL_ABOUT, ORB_SHELL_VOICE, ORB_SHELL_IDLE_SESSIONS, ORB_SHELL_UNITS,
        ORB_SHELL_TEMPERATURE, ORB_SHELL_OFFLINE, ORB_SHELL_CHARGING,
        ORB_SHELL_NOTIFICATION_DETAIL, ORB_SHELL_USAGE, ORB_SHELL_ONBOARDING,
    };
    const int deltas[][2] = {{0, 100}, {0, -100}, {100, 0}, {-100, 0}};
    for (unsigned i = 0; i < sizeof(surfaces) / sizeof(*surfaces); ++i) {
        s_case = "empty detail and utility routes retain content ownership";
        navigate(ORB_SHELL_FACE); navigate(surfaces[i]);
        for (unsigned d = 0; d < sizeof(deltas) / sizeof(*deltas); ++d) {
            drag(233, 233, deltas[d][0], deltas[d][1]); place(surfaces[i]);
            no_actions();
        }
        drag(233, 12, 0, 180); place(ORB_SHELL_NOTIFICATIONS);
        boot(); place(surfaces[i]);
        orb_voice_request_t voice; CHECK(!orb_model_take_voice_request(&voice));
        no_actions();
    }
}
static void repeated_layers(void)
{
    s_case = "reopening a layer cannot overflow or desynchronise navigation";
    navigate(ORB_SHELL_FACE);
    for (unsigned i = 0; i < 9; ++i) {
        if (i % 2) { drag(454, 233, -180, 0); place(ORB_SHELL_CONTROL_CENTER); }
        else { drag(233, 12, 0, 180); place(ORB_SHELL_NOTIFICATIONS); }
    }
    boot(); place(ORB_SHELL_FACE);
    /* Developer/direct layer routes have the identical history policy. */
    for (unsigned i = 0; i < 9; ++i)
        navigate(i % 2 ? ORB_SHELL_CONTROL_CENTER : ORB_SHELL_NOTIFICATIONS);
    boot(); place(ORB_SHELL_FACE); no_actions();
}
static void retired_compass_hold(void)
{
    s_case = "blank content hold cannot resurrect the retired Compass";
    navigate(ORB_SHELL_FACE); navigate(ORB_SHELL_CARD);
    CHECK(!text(orb_shell_root(s_shell), "Play"));
    pointer(233, 220, true, 650);
    CHECK(!text(orb_shell_root(s_shell), "Play"));
    release(); advance(1200);
    place(ORB_SHELL_CARD);
    CHECK(!text(orb_shell_root(s_shell), "Play"));
    no_actions();
    s_case = "explicit compatibility Compass uses standard kit discs";
    navigate(ORB_SHELL_COMPASS);
    lv_obj_t *glyph = text(orb_shell_root(s_shell), ORB_ICON_SUN); CHECK(glyph);
    lv_obj_t *disc = lv_obj_get_parent(glyph);
    CHECK(lv_obj_get_width(disc) == ORB_KIT_DISC && lv_obj_get_height(disc) == ORB_KIT_DISC);
    lv_obj_t *clock = text(orb_shell_root(s_shell), s_status.clock_time);
    if (clock) {
        lv_obj_t *home = text(orb_shell_root(s_shell), ORB_ICON_HOUSE); CHECK(home);
        lv_area_t a, b; lv_obj_get_coords(clock, &a); lv_obj_get_coords(lv_obj_get_parent(home), &b);
        CHECK(a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1);
    }
    save("compatibility-compass");
}
int main(int argc, char **argv)
{
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); orb_time_init(); orb_apps_init(ORB_APP_NEEDS_IMU);
    if (argc > 1 && !strcmp(argv[1], "ax3")) {
        orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3);
        orb_fonts_set_bold_text(true); orb_ui_set_reduce_transparency(true);
    }
    if (argc > 3 && !strcmp(argv[3], "rm")) orb_ui_set_reduce_motion(true);
    CHECK(orb_model_init() && orb_home_init());
    orb_model_load_demo(); orb_home_load_demo(); orb_model_set_connection(true, true);
    for (unsigned i = 0; i < 2; ++i) {
        s_pointers[i] = lv_indev_create(); CHECK(s_pointers[i]);
        lv_indev_set_type(s_pointers[i], LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(s_pointers[i], s_display);
        lv_indev_set_read_cb(s_pointers[i], read_pointer);
    }
    orb_input_mark_synthetic(s_pointers[1]);
    s_shell = orb_shell_create(lv_screen_active(), action, NULL); CHECK(s_shell);
    s_status = (orb_shell_snapshot_t){.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .brightness = 70, .clock_time = "10:09", .clock_date = "Tuesday, October 27",
        .hub_name = "Synthetic Mac", .firmware_version = "0.5.2"};
    orb_shell_update(s_shell, &s_status); advance(1400);
    for (s_source = 0; s_source < 2; ++s_source) {
        if (argc > 2 && !strcmp(argv[2], "stale")) { face_capture_freshness(); continue; }
        if (argc > 2 && !strcmp(argv[2], "qa")) {
            layer_back_axes(); nested_back(); face_capture_freshness(); repeated_layers();
            retired_compass_hold(); continue;
        }
        face_swipes(); home_fresh_entry(); early_home_taps(); routes(); direct_layer_replacement(); no_actions();
        layer_back_axes(); nested_back(); face_capture_freshness();
        remaining_surfaces();
        repeated_layers();
        retired_compass_hold();
    }
    orb_shell_destroy(s_shell);
    for (unsigned i = 0; i < 2; ++i) lv_indev_delete(s_pointers[i]);
    lv_deinit();
    printf("gestures4 nav: %u checks passed through both real shell pointer inputs\n", s_checks);
    return 0;
}
