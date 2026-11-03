/* Production navigation contracts. All state is synthetic; host rendering
 * establishes pixel/cache lifetime, never panel FPS or transfer cadence. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "orb_home.h"
#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_faces.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "orb_nav_admission.h"
#include "orb_nav_compositor.h"
#include "sim_display.h"
#include "../../firmware/components/orb_devtools/orb_open_source.h"

static uint32_t s_now;
static unsigned s_checks;
static const char *s_case;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static uint8_t s_before[SIM_D * SIM_D * 3], s_after[sizeof(s_before)];
static bool s_fail_settle_capture;
static bool s_slow_bands;
static unsigned s_read_period = LV_DEF_REFR_PERIOD;
static void no_actions(void);

#define CHECK(value) do { \
    s_checks++; \
    if (!(value)) { \
        fprintf(stderr,"nav shell %s: %s:%d: %s\n",s_case,__FILE__,__LINE__,#value); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static uint32_t tick(void) { return s_now; }

static void fail_settle_capture(lv_event_t *event)
{
    if (!s_fail_settle_capture || !s_shell) return;
    orb_shell_open_metrics_t metrics;
    orb_shell_open_metrics(s_shell, &metrics);
    if (!metrics.retaining_snapshot) return;
    s_fail_settle_capture = false;
    /* The normal first band begins at row zero. A synthetic out-of-order
     * observer event rejects only retention; it does not change driver pixels. */
    lv_area_t malformed = {0, 2, SIM_D - 1, 3};
    lv_display_send_event(lv_event_get_target(event), LV_EVENT_FLUSH_START, &malformed);
}
static void delay_band(lv_event_t *event)
{
    (void)event;
    /* Real board rendering advances time inside refresh callbacks. Keep the
     * direct settlement cases sensitive to that timer ordering too. */
    if (s_slow_bands) s_now += 3;
}
static void advance(unsigned ms)
{
    uint32_t end = s_now + ms;
    do {
        s_now += LV_MIN(8, end - s_now);
        if (s_shell) orb_shell_poll(s_shell);
        lv_timer_handler();
        lv_refr_now(s_display);
    } while (s_now < end);
}

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    *data = s_input;
}

static void pointer(int x, int y, bool down)
{
    s_input.point = (lv_point_t){x,y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    advance(16);
}

static void scheduled_pointer(int x, int y, bool down, unsigned ms)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    /* Let LVGL's production16ms read timer observe the trace. Calling
     * lv_indev_read directly here hides the release/poll ordering regression. */
    advance(ms);
}

static void natural_trace(int x, int y, int dx, int dy, unsigned duration, unsigned pause)
{
    const unsigned period = LV_DEF_REFR_PERIOD;
    scheduled_pointer(x, y, true, s_read_period);
    float length = hypotf(dx, dy);
    for (unsigned ms = period; ms < duration; ms += period) {
        float t = (float)ms / duration;
        float progress = 1 - (1 - t) * (1 - t);
        float curve = 8 * sinf(3.14159265f * t);
        scheduled_pointer((int)roundf(x + dx * progress - dy * curve / length),
            (int)roundf(y + dy * progress + dx * curve / length), true, period);
    }
    scheduled_pointer(x + dx, y + dy, true, period + pause);
    scheduled_pointer(x + dx, y + dy, false, s_read_period * 2);
    advance(1200);
}

static void natural_gestures(void)
{
    const struct {
        orb_shell_surface_t source, destination;
        int x, y, dx, dy;
    } routes[] = {
        {ORB_SHELL_FACE, ORB_SHELL_HOME, 300, 240, -130, 10},
        {ORB_SHELL_AGENTS, ORB_SHELL_FACE, 12, 240, 130, 10},
        {ORB_SHELL_FACE, ORB_SHELL_AGENTS, 160, 240, 130, 10},
        {ORB_SHELL_HOME, ORB_SHELL_FACE, 12, 240, 130, 10},
        {ORB_SHELL_FACE, ORB_SHELL_NOTIFICATIONS, 233, 180, 6, 130},
        {ORB_SHELL_NOTIFICATIONS, ORB_SHELL_FACE, 20, 233, 130, 6},
        {ORB_SHELL_FACE, ORB_SHELL_STACK, 233, 300, 6, -130},
        {ORB_SHELL_STACK, ORB_SHELL_FACE, 233, 12, 6, 130},
        {ORB_SHELL_FACE, ORB_SHELL_CONTROL_CENTER, 454, 233, -130, 6},
        {ORB_SHELL_CONTROL_CENTER, ORB_SHELL_FACE, 12, 240, 130, 10},
    };
    const unsigned durations[] = {200, 350, 600};
    const unsigned pauses[] = {0, 48, 128};
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        for (unsigned d = 0; d < sizeof(durations) / sizeof(durations[0]); d++) {
            s_case = "curved, decelerating natural swipe at LVGL input cadence";
            /* Back follows actual history rather than a Home/Agents peer
             * shortcut, so establish the same Face source for each route. */
            orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
            orb_shell_navigate(s_shell, routes[i].source); advance(1200);
            natural_trace(routes[i].x, routes[i].y, routes[i].dx, routes[i].dy,
                durations[d], pauses[d]);
            if (orb_shell_surface(s_shell) != routes[i].destination)
                fprintf(stderr, "route %u duration=%u source=%u expected=%u actual=%u\n", i,
                    durations[d], routes[i].source, routes[i].destination, orb_shell_surface(s_shell));
            CHECK(orb_shell_surface(s_shell) == routes[i].destination);
            no_actions();
        }
    }
    s_case = "short slow accidental drags remain on Face after a paused lift";
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    for (unsigned d = 0; d < sizeof(durations) / sizeof(durations[0]); d++) {
        natural_trace(300, 240, -52, 4, durations[d], 96);
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    }
    s_case = "natural swipe survives stale reads when input runs slower than navigation";
    s_read_period = 32;
    lv_timer_set_period(lv_indev_get_read_timer(s_pointer), s_read_period);
    natural_trace(300, 240, -130, 10, 350, 128);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    natural_trace(12, 240, 130, 10, 600, 128);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    s_read_period = LV_DEF_REFR_PERIOD;
    lv_timer_set_period(lv_indev_get_read_timer(s_pointer), s_read_period);
    s_case = "reverse flick cancels a natural swipe before lift";
    scheduled_pointer(300, 240, true, 16);
    for (unsigned ms = 16; ms <= 320; ms += 16)
        scheduled_pointer((int)roundf(300 - 180.f * ms / 320), 240, true, 16);
    scheduled_pointer(130, 240, true, 16);
    scheduled_pointer(144, 240, true, 16);
    scheduled_pointer(158, 240, true, 16);
    scheduled_pointer(158, 240, false, 32);
    advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    no_actions();
}

static lv_obj_t *control(lv_obj_t *root, const char *caption)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), caption)) {
        for (lv_obj_t *parent = lv_obj_get_parent(root); parent; parent = lv_obj_get_parent(parent))
            if (lv_obj_is_clickable(parent)) return parent;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = control(lv_obj_get_child(root, (int32_t)i), caption);
        if (found) return found;
    }
    return NULL;
}

static unsigned objects(lv_obj_t *root)
{
    unsigned count = 1;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        count += objects(lv_obj_get_child(root, (int32_t)i));
    return count;
}

static bool has_text(lv_obj_t *root, const char *text)
{
    const char *semantic = orb_kit_button_get_text(root);
    if (semantic && !strcmp(semantic, text)) return true;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        if (has_text(lv_obj_get_child(root, (int32_t)i), text)) return true;
    return false;
}

static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static lv_obj_t *scroll_list(lv_obj_t *root)
{
    if (lv_obj_is_visible(root) && lv_obj_is_scrollable(root) &&
        lv_obj_get_height(root) < SIM_D && lv_obj_get_scroll_bottom(root) + lv_obj_get_scroll_y(root) > 0)
        return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *list = scroll_list(lv_obj_get_child(root, (int32_t)i));
        if (list) return list;
    }
    return NULL;
}

static void no_actions(void)
{
    orb_answer_t answer;
    orb_home_request_t home;
    CHECK(!orb_model_peek_answer(&answer));
    CHECK(!orb_home_take_request(&home));
}

static orb_shell_t *create_shell(void)
{
    orb_shell_t *shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    CHECK(shell != NULL);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .usb = true};
    strcpy(status.clock_time, "10:09");
    strcpy(status.clock_date, "Tuesday, October 27");
    strcpy(status.hub_name, "Synthetic Mac");
    orb_shell_update(shell, &status);
    return shell;
}

static void unrevealed_sheet_input(void)
{
    s_case = "unrevealed sheet input";
    orb_shell_navigate(s_shell, ORB_SHELL_STACK); advance(1200);
    lv_obj_t *allow = control(orb_shell_root(s_shell), "Review");
    CHECK(allow != NULL);
    lv_area_t settled; lv_obj_get_coords(allow, &settled);
    int x = (settled.x1 + settled.x2)/2, y = (settled.y1 + settled.y2)/2;
    CHECK(x > 40 && x < SIM_D - 40 && y > 300 && y < SIM_D - 20);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    sim_display_rgb(s_before);
    pointer(233,240,true);
    pointer(233,210,true);
    pointer(233,210,false);
    allow = control(orb_shell_root(s_shell), "Review");
    CHECK(allow != NULL);
    lv_area_t moving; lv_obj_get_coords(allow, &moving);
    /* The actual destination hit target must move with its cached sheet. The
     * settled button's old position still belongs to the unrevealed region. */
    CHECK(moving.y1 > settled.y2);
    sim_display_rgb(s_after);
    CHECK(memcmp(s_before, s_after, sizeof(s_before)) != 0);
    no_actions();
    pointer(x,y,true);
    pointer(x,y,false);
    advance(1000);
    no_actions();
    /* Positive control: after deliberate navigation and settle, the same
     * readable decision button is still actionable through real pointer input. */
    orb_shell_navigate(s_shell, ORB_SHELL_STACK); advance(1200);
    allow = control(orb_shell_root(s_shell), "Review"); CHECK(allow != NULL);
    lv_obj_get_coords(allow, &settled);
    x = (settled.x1 + settled.x2)/2; y = (settled.y1 + settled.y2)/2;
    pointer(x,y,true); pointer(x,y,false); advance(1200);
    /* Stack's safe action opens the full decision; it cannot approve from a
     * glance. The explicit Allow tap remains independently actionable. */
    no_actions();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CARD);
    allow = control(orb_shell_root(s_shell), "Allow"); CHECK(allow != NULL);
    lv_obj_get_coords(allow, &settled);
    x = (settled.x1 + settled.x2) / 2; y = (settled.y1 + settled.y2) / 2;
    pointer(x,y,true); pointer(x,y,false); advance(32);
    orb_answer_t answer;
    CHECK(orb_model_take_answer(&answer));
    CHECK(!strcmp(answer.option, "allow"));
    CHECK(orb_model_cancel_card(answer.card));
    no_actions();
    orb_model_load_demo(); orb_model_set_connection(true,true);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
}

static void home_place_gesture(void)
{
    s_case = "Home horizontal ownership";
    orb_shell_navigate(s_shell, ORB_SHELL_HOME); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    lv_obj_t *list = scroll_list(orb_shell_root(s_shell)); CHECK(list);
    int before = lv_obj_get_scroll_y(list);
    /* Horizontal centre contacts remain content and never page to Climate. */
    for (unsigned direction = 0; direction < 2; ++direction) {
        int start = direction ? 105 : 325, end = direction ? 325 : 105;
        pointer(start, 200, true);
        pointer((start + end) / 2, 200, true);
        pointer(end, 200, true); pointer(end, 200, false); advance(1200);
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
        CHECK(lv_obj_is_valid(list) && scroll_list(orb_shell_root(s_shell)) == list);
        CHECK(lv_obj_get_scroll_y(list) == before);
        no_actions();
    }
}

static void home_current_snapshot(void)
{
    s_case = "current Home snapshot follows native Rooms scroll and rotary";
    orb_shell_navigate(s_shell, ORB_SHELL_HOME); advance(1200);
    lv_obj_t *list = scroll_list(orb_shell_root(s_shell)); CHECK(list);
    lv_obj_stop_scroll_anim(list); lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); advance(1200);
    CHECK(orb_shell_twist(s_shell, 3)); advance(800);
    CHECK(lv_obj_is_valid(list) && lv_obj_get_scroll_y(list) > 0);
    CHECK(orb_shell_twist(s_shell, -4096)); advance(1200);
    CHECK(lv_obj_get_scroll_y(list) == 0);
    sim_display_rgb(s_before);
    /* Return a right-edge sheet preview to zero progress. Its source must
     * reproduce today's Rooms pixels, never an old captured page. */
    pointer(454, 233, true); pointer(434, 233, true);
    pointer(354, 233, true); pointer(454, 233, true);
    sim_display_rgb(s_after);
    for (int row = 150; row < 350; row++) {
        unsigned start = (unsigned)(row * SIM_D + 70) * 3;
        CHECK(!memcmp(s_before + start, s_after + start, (396 - 70) * 3));
    }
    pointer(454, 233, false); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    no_actions();
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
}

static void benchmark_contract(const char *gesture)
{
    s_case = gesture;
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS);
    advance(1200);
    lv_obj_t *list = scroll_list(orb_shell_root(s_shell));
    CHECK(list != NULL);
    lv_obj_scroll_to_y(list, 96, LV_ANIM_OFF);
    advance(100);
    int saved_scroll = lv_obj_get_scroll_y(list);
    CHECK(saved_scroll > 0);
    CHECK(orb_shell_nav_bench(s_shell, gesture, 0, false));
    /* Cold capture is deliberately excluded. Warm frames must only read the
     * admitted pixels. Rotary exercises the existing retained list instead. */
    advance(33);
    CHECK(orb_shell_nav_bench(s_shell, gesture, 33, false));
    lv_refr_now(s_display);
    unsigned count = objects(orb_shell_root(s_shell));
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    sim_display_rgb(s_before);
    bool changed = false;
    for (unsigned sample = 2; sample <= 7; sample++) {
        advance(33);
        CHECK(orb_shell_nav_bench(s_shell, gesture, sample * 33, false));
        lv_refr_now(s_display);
        CHECK(objects(orb_shell_root(s_shell)) == count);
        sim_display_rgb(s_after);
        changed |= memcmp(s_before, s_after, sizeof(s_before)) != 0;
    }
    lv_obj_raster_get_stats(&after);
    if (strcmp(gesture, "rotary")) CHECK(after.builds == before.builds);
    CHECK(changed);
    no_actions();
    CHECK(orb_shell_nav_bench(s_shell, gesture, 0, true));
    advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    list = scroll_list(orb_shell_root(s_shell));
    CHECK(list != NULL && lv_obj_get_scroll_y(list) == saved_scroll);
    no_actions();
}

static void regrab_release(void)
{
    /* Stay above Home's actionable rows: a fresh centre-row tap during
     * arrival legitimately opens that room before the shell has settled. */
    s_case = "stationary regrab preserves reverse-flick cancellation";
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    pointer(400,233,true); pointer(300,233,true); pointer(35,233,true);
    pointer(95,233,true); pointer(135,233,true); pointer(135,233,false);
    CHECK(sim_display_snapshot_transition_active());
    pointer(233,80,true); pointer(233,80,false); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    s_case = "stationary regrab preserves commit";
    pointer(400,233,true); pointer(300,233,true); pointer(150,233,true);
    pointer(150,233,false);
    CHECK(sim_display_snapshot_transition_active());
    pointer(233,80,true); pointer(233,80,false); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    s_case = "shell uses core projected release threshold";
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    pointer(360,233,true); pointer(206,233,true); advance(96);
    pointer(206,233,false); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    no_actions();
}

static void display_off_navigation(void)
{
    for (unsigned distance = 200; distance <= 300; distance += 100) {
        s_case = "display off settles held drag at nearest endpoint";
        orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
        pointer(400,233,true); pointer(400 - (int)distance,233,true);
        CHECK(sim_display_snapshot_transition_active());
        orb_shell_display_active(s_shell, false);
        CHECK(!sim_display_snapshot_transition_active());
        orb_shell_surface_t expected = distance < 233 ? ORB_SHELL_FACE : ORB_SHELL_HOME;
        CHECK(orb_shell_surface(s_shell) == expected);
        advance(500);
        orb_shell_display_active(s_shell, true);
        pointer(60,233,true); pointer(60,233,false); advance(1200);
        CHECK(orb_shell_surface(s_shell) == expected);
        CHECK(!sim_display_snapshot_transition_active());
    }
    s_case = "display off settles released spring immediately";
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    pointer(400,233,true); pointer(100,233,true); pointer(100,233,false);
    CHECK(sim_display_snapshot_transition_active());
    orb_shell_display_active(s_shell, false);
    CHECK(!sim_display_snapshot_transition_active());
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    orb_shell_display_active(s_shell, true); advance(1200);
    s_case = "fresh gesture after wake";
    pointer(12,233,true); pointer(112,233,true); pointer(282,233,true);
    pointer(282,233,false); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);

    s_case = "display off cancels queued capture admission";
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); advance(1200);
    lv_obj_t *display = control(orb_shell_root(s_shell), "Display");
    CHECK(display != NULL);
    pointer(233,30,true); pointer(233,30,false);
    lv_obj_send_event(display, LV_EVENT_CLICKED, NULL);
    orb_shell_open_metrics_t opening;
    orb_shell_open_metrics(s_shell, &opening); CHECK(opening.active);
    CHECK(!sim_display_snapshot_transition_active());
    orb_shell_display_active(s_shell, false); advance(500);
    CHECK(!sim_display_snapshot_transition_active());
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    orb_shell_display_active(s_shell, true); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);

    for (unsigned released = 0; released < 2; released++) {
        s_case = "display off clears held crown and fling ownership";
        lv_obj_t *list = scroll_list(orb_shell_root(s_shell)); CHECK(list != NULL);
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); advance(100);
        pointer(449,233,true); pointer(445,274,true); pointer(429,324,true);
        if (released) pointer(429,324,false);
        orb_shell_display_active(s_shell, false);
        int scroll = lv_obj_get_scroll_y(list); CHECK(scroll > 0);
        orb_shell_display_active(s_shell, true);
        if (!released) { pointer(233,449,true); pointer(233,449,false); }
        advance(1200);
        CHECK(lv_obj_get_scroll_y(list) == scroll);
        CHECK(!sim_display_snapshot_transition_active());
    }
    s_case = "display off preserves a content-owned centre drag";
    orb_shell_navigate(s_shell, ORB_SHELL_HOME); advance(1200);
    pointer(350,233,true); pointer(50,233,true);
    CHECK(!sim_display_snapshot_transition_active());
    orb_shell_display_active(s_shell, false);
    CHECK(!sim_display_snapshot_transition_active());
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    pointer(50,233,false);
    orb_shell_display_active(s_shell, true); advance(1200);
    no_actions();
}

static void centre_during_sheet_settle(void)
{
    static const struct {
        int x, y, dx, dy;
        orb_shell_surface_t destination;
    } routes[] = {
        {233, 12, 0, 150, ORB_SHELL_NOTIFICATIONS},
        {233, 454, 0, -150, ORB_SHELL_STACK},
        {454, 233, -150, 0, ORB_SHELL_CONTROL_CENTER},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(*routes); ++i) {
        s_case = "centre content contact cannot regrab or freeze a settling system sheet";
        orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
        pointer(routes[i].x, routes[i].y, true);
        pointer(routes[i].x + routes[i].dx, routes[i].y + routes[i].dy, true);
        pointer(routes[i].x + routes[i].dx, routes[i].y + routes[i].dy, false);
        CHECK(sim_display_snapshot_transition_active());
        pointer(233, 233, true);
        pointer(233, 180, true);
        pointer(233, 233, true);
        pointer(233, 233, false); advance(1200);
        CHECK(orb_shell_surface(s_shell) == routes[i].destination);
        CHECK(!sim_display_snapshot_transition_active());
        no_actions();
    }
}

static void rotary_pages(void)
{
    s_case = "one crown callback applies every page detent";
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    orb_shell_navigate(s_shell, ORB_SHELL_CONTROL_CENTER); advance(1200);
    CHECK(orb_shell_twist(s_shell, 3));
    CHECK(has_text(orb_shell_root(s_shell), ORB_ICON_SUN " 73%"));
    /* Brightness owns rim/twist. Vertical content swipes still page. */
    pointer(233,320,true); pointer(233,120,true); pointer(233,120,false); advance(1200);
    /* Page 2's rim names Home; its content leads with the scene or Open Home. */
    CHECK(has_text(orb_shell_root(s_shell), "Open Home") || has_text(orb_shell_root(s_shell), "Good morning") ||
          has_text(orb_shell_root(s_shell), "Evening") || has_text(orb_shell_root(s_shell), "Good Night") ||
          has_text(orb_shell_root(s_shell), "I'm home"));
    CHECK(orb_shell_twist(s_shell, -1)); advance(1200);
    CHECK(has_text(orb_shell_root(s_shell), ORB_ICON_SUN " 73%"));
    CHECK(control(orb_shell_root(s_shell), ORB_ICON_FLASHLIGHT));
    CHECK(control(orb_shell_root(s_shell), ORB_ICON_GEAR));
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    no_actions();
}

static void workspace_shot(const char *name)
{
    const char *directory = getenv("ORB_WORKSPACE_SHOTS");
    if (!directory) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    CHECK(sim_display_save_png(path));
}

static void compass_and_glass(void)
{
    /* Design 2.0 retires the Face long-press Compass. The held contact
     * opens the face gallery and cannot activate a face on release. */
    s_case = "Face hold opens gallery and owns release";
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    pointer(233, 220, true); advance(650);
    pointer(233, 220, false); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(has_text(orb_shell_root(s_shell), "Edit"));
    CHECK(!has_text(orb_shell_root(s_shell), "Settings"));
    orb_shell_back(s_shell); advance(1200);
    CHECK(!has_text(orb_shell_root(s_shell), "Edit"));
    no_actions();
    s_case = "explicit Compass selects from the rim without a prior hold";
    orb_shell_navigate(s_shell, ORB_SHELL_COMPASS); advance(1200);
    CHECK(!has_text(orb_shell_root(s_shell), "Compass"));
    pointer(430, 233, true); pointer(233, 36, true);
    pointer(233, 36, false); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    s_case = "Glass setting updates all controls and Back returns to Settings";
    lv_obj_t *row = control(orb_shell_root(s_shell), "Glass"); CHECK(row);
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); advance(100);
    workspace_shot("Glass-default");
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(true); advance(500);
    workspace_shot("Glass-ax3-bold-rt");
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(false); advance(500);
    row = control(orb_shell_root(s_shell), "Clear"); CHECK(row);
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); advance(100);
    CHECK(orb_glass_preference() == ORB_GLASS_PREFERENCE_CLEAR);
    row = control(orb_shell_root(s_shell), "Tinted"); CHECK(row);
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); advance(100);
    CHECK(orb_glass_preference() == ORB_GLASS_PREFERENCE_TINTED);
    orb_shell_back(s_shell); advance(500);
    CHECK(control(orb_shell_root(s_shell), "Sound"));
    CHECK(orb_glass_set_preference(ORB_GLASS_PREFERENCE_DEFAULT));
    no_actions();
}

static void seed_face_preferences(const orb_faces_prefs_t *prefs)
{
    orb_faces_t *view = orb_faces_create(lv_screen_active(), &(orb_faces_config_t){.preview = true}); CHECK(view);
    /* Mark a local editing transaction dirty, then install the requested
     * versioned value. Preview persistence is process-local, never a user file. */
    orb_faces_select(view, (orb_face_kind_t)((prefs->face + 1) % ORB_FACE_COUNT));
    orb_faces_set_preferences(view, prefs);
    CHECK(orb_faces_save(view)); orb_faces_destroy(view);
}

static void codex_face_route(void)
{
    s_case = "Codex complication opens its own Usage page and Back restores Face";
    orb_faces_prefs_t saved; orb_faces_current_preferences(&saved);
    orb_faces_prefs_t selected = saved;
    selected.face = ORB_FACE_ORB; selected.slot[0] = ORB_FACE_CODEX;
    seed_face_preferences(&selected);
    orb_usage_snapshot_t usage = {.count = 2, .windows = {
        {.provider = ORB_AGENT_CLAUDE, .label = "5 hours", .used_percent = 62, .source = ORB_USAGE_USAGE},
        {.provider = ORB_AGENT_CODEX, .label = "5 hours", .used_percent = 38, .source = ORB_USAGE_USAGE}}};
    CHECK(orb_model_set_usage(&usage));
    s_shell = create_shell(); advance(1200);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    pointer(128, 316, true); advance(64); pointer(128, 316, false); advance(1400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_USAGE);
    CHECK(has_text(orb_shell_root(s_shell), "Codex"));
    CHECK(has_text(orb_shell_root(s_shell), "38%"));
    orb_shell_back(s_shell); advance(1400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(has_text(orb_shell_root(s_shell), "38"));
    orb_shell_destroy(s_shell); s_shell = NULL;
    seed_face_preferences(&saved);
}

static void benchmark_source_contract(void)
{
    static const orb_shell_surface_t sources[] = {
        ORB_SHELL_FACE, ORB_SHELL_STACK, ORB_SHELL_APPS, ORB_SHELL_AGENTS,
        ORB_SHELL_SESSION, ORB_SHELL_CARD, ORB_SHELL_NOTIFICATIONS, ORB_SHELL_CONTROL_CENTER,
        ORB_SHELL_SETTINGS, ORB_SHELL_DISPLAY, ORB_SHELL_ACCESSIBILITY, ORB_SHELL_ABOUT,
        ORB_SHELL_DOT, ORB_SHELL_VOICE, ORB_SHELL_PLAY, ORB_SHELL_IDLE_SESSIONS,
        ORB_SHELL_SESSION_SETTINGS, ORB_SHELL_HOME, ORB_SHELL_UNITS, ORB_SHELL_TEMPERATURE,
        ORB_SHELL_SOUND,
    };
    static const char *gestures[] = {"slide-left", "slide-right", "stack-up", "shade-down", "back", "iris", "rotary"};
    for (unsigned i = 0; i < sizeof(sources) / sizeof(*sources); i++) {
        s_case = "benchmark chooses source and restores every workspace surface";
        orb_shell_navigate(s_shell, sources[i]); advance(800);
        CHECK(orb_shell_surface(s_shell) == sources[i]);
        const char *gesture = gestures[i % 7];
        CHECK(orb_shell_nav_bench(s_shell, gesture, 0, false));
        CHECK(orb_shell_nav_bench(s_shell, gesture, 165, false));
        CHECK(orb_shell_nav_bench(s_shell, gesture, 0, true));
        advance(800);
        CHECK(orb_shell_surface(s_shell) == sources[i]);
        no_actions();
    }
    s_case = "benchmark restores accessibility detail";
    orb_shell_navigate(s_shell, ORB_SHELL_ACCESSIBILITY); advance(800);
    lv_obj_t *row = control(orb_shell_root(s_shell), "Text size");
    CHECK(row != NULL);
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); advance(100);
    CHECK(control(orb_shell_root(s_shell), "Default") != NULL);
    CHECK(orb_shell_nav_bench(s_shell, "iris", 0, false));
    CHECK(orb_shell_nav_bench(s_shell, "iris", 165, false));
    CHECK(orb_shell_nav_bench(s_shell, NULL, 0, true)); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_ACCESSIBILITY);
    CHECK(control(orb_shell_root(s_shell), "Default") != NULL);
    no_actions();

    s_case = "optional Updates menu is restored";
    orb_shell_navigate(s_shell, ORB_SHELL_UPDATES); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_UPDATES);
    CHECK(orb_shell_nav_bench(s_shell, "slide-left", 0, false));
    CHECK(orb_shell_nav_bench(s_shell, "slide-left", 165, false));
    CHECK(orb_shell_nav_bench(s_shell, NULL, 0, true)); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_UPDATES);
    no_actions();
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); advance(800);

    s_case = "mandatory installation refuses admission";
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .ota = ORB_SHELL_OTA_DOWNLOADING, .battery_pct = 80, .progress = 25};
    orb_shell_update(s_shell, &status); advance(100);
    unsigned count = objects(orb_shell_root(s_shell));
    CHECK(!orb_shell_nav_bench(s_shell, "slide-left", 0, false));
    CHECK(orb_nav_admission_last().reason == ORB_NAV_ADMISSION_WORKSPACE_HOLD);
    CHECK(orb_shell_nav_bench(s_shell, NULL, 0, true));
    CHECK(objects(orb_shell_root(s_shell)) == count);
    status.ota = ORB_SHELL_OTA_IDLE;
    orb_shell_update(s_shell, &status); advance(800);
    no_actions();
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); advance(800);
}

static void back_update_contract(void)
{
    s_case = "Back retains return pixels and commits through a status update";
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(1200);
    orb_open_source_t setup;
    orb_open_source_begin(&setup, s_shell, ORB_SHELL_PLAY);
    unsigned steps = 0;
    while (!orb_open_source_ready(&setup, s_shell) && ++steps < 300) advance(8);
    CHECK(steps < 300);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    sim_display_rgb(s_before);
    orb_shell_back(s_shell);
    orb_shell_open_metrics_t first, updated, before_open;
    orb_shell_open_metrics(s_shell, &before_open);
    orb_shell_open_metrics(s_shell, &first);
    advance(24);
    sim_display_rgb(s_after);
    CHECK(memcmp(s_before, s_after, sizeof(s_before)) != 0);
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds);
    orb_shell_open_metrics(s_shell, &updated);
    CHECK(updated.snapshot_captures == before_open.snapshot_captures);
    CHECK(updated.background_prepares == before_open.background_prepares);
    CHECK(updated.background_hits > before_open.background_hits);
    CHECK(updated.build_slices == 0);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 79, .rssi = -54, .brightness = 70, .usb = true};
    strcpy(status.clock_time, "10:10");
    orb_shell_update(s_shell, &status);
    advance(32);
    orb_shell_open_metrics(s_shell, &updated);
    CHECK(updated.sequence == first.sequence);
    advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    no_actions();
}

static void await_navigation(orb_shell_surface_t surface)
{
    orb_shell_open_metrics_t metrics = {0};
    for (unsigned elapsed = 0; elapsed < 2400; elapsed += 8) {
        advance(8);
        orb_shell_open_metrics(s_shell, &metrics);
        if (orb_shell_surface(s_shell) == surface && metrics.surface == surface &&
            !metrics.active && metrics.content_ready && !metrics.retaining_snapshot &&
            !sim_display_snapshot_transition_active()) {
            CHECK(true);
            return;
        }
    }
    fprintf(stderr, "navigation timeout: target=%u live=%u receipt=%u active=%d ready=%d retaining=%d\n",
        surface, orb_shell_surface(s_shell), metrics.surface, metrics.active,
        metrics.content_ready, metrics.retaining_snapshot);
    CHECK(false);
}

static void repeated_back_settlement(void)
{
    static uint8_t retained[2][sizeof(s_before)];
    s_slow_bands = true;
    s_case = "three consecutive Back commands settle without benchmark assistance";
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); await_navigation(ORB_SHELL_FACE);
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); await_navigation(ORB_SHELL_APPS);
    sim_display_rgb(retained[1]);
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); await_navigation(ORB_SHELL_SETTINGS);
    sim_display_rgb(retained[0]);
    orb_shell_navigate(s_shell, ORB_SHELL_DISPLAY); await_navigation(ORB_SHELL_DISPLAY);
    const orb_shell_surface_t returns[] = {ORB_SHELL_SETTINGS, ORB_SHELL_APPS, ORB_SHELL_FACE};
    for (unsigned i = 0; i < sizeof(returns) / sizeof(*returns); i++) {
        orb_shell_back(s_shell);
        await_navigation(returns[i]);
        sim_display_rgb(s_after);
        /* Face has a time-driven ambient indicator. The two static return
         * planes must remain exact; Face must restore its real clock. */
        if (i < 2) CHECK(!memcmp(retained[i], s_after, sizeof(s_after)));
        else CHECK(has_text(orb_shell_root(s_shell), "10:10"));
    }
    s_case = "Back forward Back settles without benchmark assistance";
    for (unsigned run = 0; run < 12; run++) {
        orb_shell_navigate(s_shell, ORB_SHELL_APPS); await_navigation(ORB_SHELL_APPS);
        sim_display_rgb(s_before);
        orb_shell_navigate(s_shell, ORB_SHELL_PLAY); await_navigation(ORB_SHELL_PLAY);
        orb_shell_back(s_shell); await_navigation(ORB_SHELL_APPS);
        sim_display_rgb(s_after);
        CHECK(!memcmp(s_before, s_after, sizeof(s_after)));
        orb_shell_navigate(s_shell, ORB_SHELL_PLAY); await_navigation(ORB_SHELL_PLAY);
        orb_shell_back(s_shell); await_navigation(ORB_SHELL_APPS);
        sim_display_rgb(s_after);
        CHECK(!memcmp(s_before, s_after, sizeof(s_after)));
        /* Board receipts restore Controls between runs; layer history must
         * remain valid when the next ordinary navigation leaves that layer. */
        orb_shell_navigate(s_shell, ORB_SHELL_CONTROL_CENTER); await_navigation(ORB_SHELL_CONTROL_CENTER);
        orb_shell_navigate(s_shell, ORB_SHELL_FACE); await_navigation(ORB_SHELL_FACE);
    }
    for (unsigned reduced = 0; reduced < 2; reduced++) {
        orb_ui_set_reduce_motion(reduced);
        for (unsigned boundary = 0; boundary < 3; boundary++) {
            s_case = "reverse Back settles through admission and presentation model updates";
            orb_shell_navigate(s_shell, ORB_SHELL_APPS); await_navigation(ORB_SHELL_APPS);
            sim_display_rgb(s_before);
            orb_shell_navigate(s_shell, ORB_SHELL_PLAY); await_navigation(ORB_SHELL_PLAY);
            orb_shell_back(s_shell);
            /* Zero targets the queued admission gap; the others target a
             * presented frame and the late spring/fade respectively. */
            if (boundary) advance(boundary == 1 ? 24 : reduced ? 120 : 480);
            orb_model_load_demo(); orb_model_set_connection(true, true);
            orb_shell_poll(s_shell);
            await_navigation(ORB_SHELL_APPS);
            CHECK(control(orb_shell_root(s_shell), ORB_ICON_GAME_CONTROLLER) != NULL);
            CHECK(!has_text(orb_shell_root(s_shell), "No games installed"));
            sim_display_rgb(s_after);
            CHECK(!memcmp(s_before, s_after, sizeof(s_after)));
        }
    }
    orb_ui_set_reduce_motion(false);
    s_slow_bands = false;
    no_actions();
}

static void warm_apps_first_presentation(void)
{
    s_case = "warm Apps presents its retained plane before live construction";
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(1200);
    sim_display_rgb(s_before);
    orb_open_source_t setup;
    orb_open_source_begin(&setup, s_shell, ORB_SHELL_FACE);
    unsigned steps = 0;
    while (!orb_open_source_ready(&setup, s_shell) && ++steps < 200) advance(8);
    CHECK(steps < 200);
    orb_shell_open_metrics_t before, first;
    orb_shell_open_metrics(s_shell, &before);
    orb_shell_navigate(s_shell, ORB_SHELL_APPS);
    advance(8);
    orb_shell_open_metrics(s_shell, &first);
    CHECK(sim_display_snapshot_transition_active());
    CHECK(first.snapshot_captures == before.snapshot_captures);
    CHECK(first.build_slices == 1); /* Only the unchanged cheap scaffold exists. */
    CHECK(!first.content_ready && first.active);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    advance(1200);
    sim_display_rgb(s_after);
    /* Neither the delayed live tree nor the cached entry may change the
     * settled Apps pixels, including chrome and background. */
    CHECK(!memcmp(s_before, s_after, sizeof(s_before)));
    no_actions();

    s_case = "warm Apps Reduce Motion retains the ordinary content fade";
    orb_ui_set_reduce_motion(true);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(8);
    CHECK(!sim_display_snapshot_transition_active());
    orb_shell_open_metrics(s_shell, &first);
    CHECK(first.active && !first.content_ready);
    advance(1200);
    sim_display_rgb(s_after);
    CHECK(!memcmp(s_before, s_after, sizeof(s_before)));
    orb_ui_set_reduce_motion(false);
    no_actions();

    s_case = "direct return to Apps retains its reverse-launch presentation";
    orb_shell_navigate(s_shell, ORB_SHELL_PLAY); advance(1200);
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(8);
    CHECK(!sim_display_snapshot_transition_active());
    advance(1200);
    sim_display_rgb(s_after);
    CHECK(!memcmp(s_before, s_after, sizeof(s_before)));
    no_actions();

    s_case = "warm Apps rejects a retained caption from another focus context";
    lv_obj_t *agents = control(orb_shell_root(s_shell), ORB_ICON_PROMPT);
    lv_obj_t *play = control(orb_shell_root(s_shell), ORB_ICON_GAME_CONTROLLER);
    CHECK(agents != NULL && play != NULL);
    lv_obj_send_event(agents, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(agents, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(play, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(play, LV_EVENT_RELEASED, NULL);
    advance(1200);
    sim_display_rgb(s_before);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(8);
    CHECK(!sim_display_snapshot_transition_active());
    advance(1200);
    sim_display_rgb(s_after);
    CHECK(!memcmp(s_before, s_after, sizeof(s_before)));
    no_actions();
}

static void failed_settle_retention(void)
{
    s_case = "failed final-band retention cannot strand an ordinary open";
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(1200);
    sim_display_rgb(s_before);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    s_fail_settle_capture = true;
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(1200);
    CHECK(!s_fail_settle_capture);
    orb_shell_open_metrics_t first, later;
    orb_shell_open_metrics(s_shell, &first);
    CHECK(!first.active && first.content_ready && !first.retaining_snapshot);
    sim_display_rgb(s_after);
    CHECK(!memcmp(s_before, s_after, sizeof(s_before)));
    advance(1200);
    orb_shell_open_metrics(s_shell, &later);
    CHECK(!later.active && !later.retaining_snapshot);
    CHECK(first.snapshot_captures == later.snapshot_captures);
    no_actions();
}

static void reverse_back_interaction(void)
{
    s_case = "reverse Back cancels on the visible round interval";
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(1200);
    orb_shell_navigate(s_shell, ORB_SHELL_PLAY); advance(1200);
    sim_display_rgb(s_before);
    pointer(12, 233, true); pointer(40, 233, true);
    sim_display_rgb(s_after);
    CHECK(memcmp(s_before, s_after, sizeof(s_before)) != 0);
    pointer(12, 233, true); pointer(12, 233, false); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_PLAY);
    sim_display_rgb(s_after);
    CHECK(!memcmp(s_before, s_after, sizeof(s_before)));

    s_case = "reverse Back suspend uses logical visible distance";
    pointer(12, 233, true); pointer(198, 233, true);
    orb_shell_display_active(s_shell, false);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_PLAY);
    orb_shell_display_active(s_shell, true);
    pointer(198, 233, false); advance(1200);

    s_case = "stationary regrab preserves reverse Back pixels and commit";
    orb_shell_back(s_shell); advance(24);
    sim_display_rgb(s_before);
    pointer(12, 233, true);
    sim_display_rgb(s_after);
    CHECK(!memcmp(s_before, s_after, sizeof(s_before)));
    pointer(12, 233, false); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    no_actions();
}

static void system_back_restores_scroll(void)
{
    s_case = "Design 2.0 Back restores the Settings chord position";
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); advance(1200);
    lv_obj_t *list = scroll_list(orb_shell_root(s_shell)); CHECK(list);
    lv_obj_scroll_to_y(list, 168, LV_ANIM_OFF); orb_round_list_update(list); advance(400);
    int saved = lv_obj_get_scroll_y(list); CHECK(saved > 0);
    orb_shell_navigate(s_shell, ORB_SHELL_DISPLAY); advance(1200);
    orb_shell_back(s_shell); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    list = scroll_list(orb_shell_root(s_shell)); CHECK(list);
    CHECK(lv_obj_get_scroll_y(list) == saved);
    no_actions();
}

static void motion_table_timing(void)
{
    /* Actual transition lifetime, not merely the spring response parameter:
     * the old common .45s spring settled in 534 ms through this public API. */
    const struct {
        orb_nav_transition_kind_t kind;
        bool reverse;
        unsigned duration;
    } cases[] = {
        {ORB_NAV_TRANSITION_IRIS, false, 280},
        {ORB_NAV_TRANSITION_IRIS, true, 240},
        {ORB_NAV_TRANSITION_SHEET, false, 260},
        {ORB_NAV_TRANSITION_BACK, true, 260},
    };
    s_case = "Main motion table measures opening, closing and layer lifetime";
    orb_nav_compositor_t *compositor = orb_nav_compositor_create(16, 16); CHECK(compositor);
    for (unsigned slot = 0; slot < 2; ++slot) {
        CHECK(orb_nav_compositor_capture_begin(compositor, slot, 1));
        memset(orb_nav_compositor_pixels(compositor, slot), 0, 16 * 16 * sizeof(uint16_t));
        CHECK(orb_nav_compositor_capture_complete(compositor, slot));
    }
    const orb_nav_token_rect_t token = {4, 4, 8, 8};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        for (unsigned reduced = 0; reduced < 2; ++reduced) {
            unsigned duration = reduced ? 150 : cases[i].duration;
            CHECK(orb_nav_compositor_begin(compositor, cases[i].kind, 0, 1,
                ORB_NAV_DIRECTION_LEFT, &token, cases[i].reverse, reduced));
            orb_nav_compositor_release(compositor, true, 0);
            for (unsigned elapsed = 1; elapsed < duration; ++elapsed) {
                orb_nav_compositor_tick(compositor, 1);
                CHECK(orb_nav_compositor_active(compositor));
            }
            /* Deadline closure only retires an already near-settled spring tail;
             * motion cannot stay stationary then snap to its destination. */
            CHECK(orb_nav_compositor_progress(compositor) > .99f);
            CHECK(orb_nav_compositor_progress(compositor) < 1.f);
            orb_nav_compositor_tick(compositor, 1);
            CHECK(!orb_nav_compositor_active(compositor));
            CHECK(orb_nav_compositor_committed(compositor));
            CHECK(orb_nav_compositor_progress(compositor) == 1.f);
        }
    }
    orb_nav_compositor_destroy(compositor);
}

int main(void)
{
    static const char *gestures[] = {"slide-left", "slide-right", "stack-up", "shade-down", "back", "iris", "rotary"};
    s_case = "setup";
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    CHECK(s_display != NULL);
    orb_ui_init(NULL);
    orb_apps_init(ORB_APP_NEEDS_IMU);
    orb_glass_cache_prewarm(); /* Same cold-mask admission as firmware boot. */
    CHECK(orb_model_init() && orb_home_init());
    orb_model_load_demo();
    orb_model_set_connection(true, true);
    orb_home_load_demo();
    s_pointer = lv_indev_create(); CHECK(s_pointer != NULL);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    s_shell = create_shell();
    lv_display_add_event_cb(s_display, fail_settle_capture, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_display, delay_band, LV_EVENT_FLUSH_START, NULL);
    advance(1200);
    motion_table_timing();
    natural_gestures();
    regrab_release();
    centre_during_sheet_settle();
    display_off_navigation();
    rotary_pages();
    compass_and_glass();
    unrevealed_sheet_input();
    home_place_gesture();
    home_current_snapshot();
    for (unsigned i = 0; i < sizeof(gestures) / sizeof(gestures[0]); i++) benchmark_contract(gestures[i]);
    benchmark_source_contract();
    back_update_contract();
    repeated_back_settlement();
    warm_apps_first_presentation();
    failed_settle_retention();
    reverse_back_interaction();
    system_back_restores_scroll();
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); advance(800);

    s_case = "rejected admission restore";
    CHECK(!orb_shell_nav_bench(s_shell, "unsupported", 0, false));
    CHECK(orb_shell_nav_bench(s_shell, NULL, 0, true));
    advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    no_actions();
    orb_shell_destroy(s_shell); s_shell = NULL;
    advance(100);
    unsigned baseline_objects = objects(lv_screen_active()), baseline_timers = timers();
    for (unsigned cycle = 0; cycle < 20; cycle++) {
        s_case = "create/delete active transition";
        s_shell = create_shell(); advance(800);
        const char *gesture = gestures[cycle % 7];
        CHECK(orb_shell_nav_bench(s_shell, gesture, 0, false));
        advance(33);
        CHECK(orb_shell_nav_bench(s_shell, gesture, 33, false));
        CHECK(sim_display_snapshot_transition_active() == (strcmp(gesture, "rotary") != 0));
        if (cycle % 2) CHECK(orb_shell_nav_bench(s_shell, gesture, 0, true));
        no_actions();
        orb_shell_destroy(s_shell); s_shell = NULL;
        CHECK(!sim_display_snapshot_transition_active());
        advance(100);
        CHECK(objects(lv_screen_active()) == baseline_objects);
        CHECK(timers() == baseline_timers);
    }
    codex_face_route();
    printf("nav shell: %u checks passed, seven gestures and 20 create/delete cycles\n", s_checks);
    lv_indev_delete(s_pointer);
    lv_deinit();
    return 0;
}
