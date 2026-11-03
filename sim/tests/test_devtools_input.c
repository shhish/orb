/* Production developer command decoding, recognizer, shell dispatch and the
 * board's two-pointer topology. No hub, PMU, audio or persisted owner state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_button_command.h"
#include "orb_devtools_input.h"
#include "orb_devtools_scroll_fixture.h"
#include "orb_devtools_wire.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_model.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static orb_shell_t *s_shell;
static lv_indev_t *s_developer;
static lv_indev_data_t s_touch;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "%s:%d at %u ms: %s\n", __FILE__, __LINE__, s_now, #value); exit(1); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *out)
{
    *out = input == s_developer ? s_touch : (lv_indev_data_t){.state = LV_INDEV_STATE_RELEASED};
}
static void advance(unsigned ms)
{
    uint32_t end = s_now + ms;
    while (s_now < end) {
        s_now += LV_MIN(10, end - s_now);
        orb_shell_poll(s_shell);
        lv_timer_handler();
    }
}
static lv_indev_t *pointer(void)
{
    lv_indev_t *input = lv_indev_create(); CHECK(input);
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_pointer);
    lv_indev_set_display(input, lv_display_get_default());
    return input;
}
static lv_obj_t *semantic(lv_obj_t *root, const char *id)
{
    if (!lv_obj_is_visible(root)) return NULL;
    const char *data = lv_obj_get_user_data(root);
    if ((intptr_t)data > 4096 && !strcmp(data, id)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = semantic(lv_obj_get_child(root, i), id);
        if (found) return found;
    }
    return NULL;
}
static void press(const char *button_name, const char *gesture_name)
{
    char json[128];
    snprintf(json, sizeof(json), "{\"stream\":1,\"args\":[\"%s\",\"%s\"]}", button_name, gesture_name);
    cJSON *params = cJSON_Parse(json); CHECK(params);
    uint32_t stream = 0;
    char command[ORB_DEVTOOLS_COMMAND_BYTES];
    CHECK(orb_devtools_wire_command("devtools.press", params, &stream, command));
    CHECK(stream == 1);
    cJSON_Delete(params);
    orb_button_t button;
    orb_button_gesture_t gesture;
    CHECK(orb_devtools_button_command(command, &button, &gesture));
    CHECK(button == (!strcmp(button_name, "pwr") ? ORB_BUTTON_PWR : ORB_BUTTON_BOOT));
    orb_buttons_t recognizer;
    orb_buttons_init(&recognizer);
    CHECK(orb_buttons_inject(&recognizer, button, gesture, s_now));
    orb_button_event_t event;
    unsigned events = 0;
    while (orb_buttons_next(&recognizer, &event)) {
        CHECK(event.synthetic);
        CHECK(event.button == button);
        orb_shell_button(s_shell, &event);
        ++events;
    }
    CHECK(events == 3);
    advance(800);
}
static void awake_face(void)
{
    CHECK(orb_devtools_input_goto(s_shell, ORB_SHELL_FACE));
    advance(1600);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(!orb_shell_power_snapshot(s_shell).redact);
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    CHECK(sim_display_create());
    orb_ui_init(NULL);
    CHECK(orb_model_init()); CHECK(orb_home_init());
    orb_model_reset(); orb_model_set_connection(true, true);
    lv_indev_t *physical = pointer();
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .usb = true, .brightness = 70};
    strcpy(status.clock_time, "10:09"); strcpy(status.clock_date, "Tue 27");
    orb_shell_update(s_shell, &status);
    orb_shell_power_enable(s_shell, true);
    advance(800);
    /* Devtools adds its synthetic indev after the physical display pointer. */
    s_developer = pointer(); orb_input_mark_synthetic(s_developer);
    awake_face();

    static const orb_shell_surface_t places[] = {ORB_SHELL_FACE, ORB_SHELL_AGENTS,
        ORB_SHELL_HOME, ORB_SHELL_SETTINGS, ORB_SHELL_NOTIFICATIONS, ORB_SHELL_CONTROL_CENTER};
    for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        CHECK(orb_devtools_input_goto(s_shell, places[i])); advance(1600);
        CHECK(orb_shell_surface(s_shell) == places[i]);
        press("pwr", "short");
        CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
        CHECK(orb_shell_power_snapshot(s_shell).redact);
        CHECK(orb_shell_surface(s_shell) == places[i]);
        CHECK(orb_devtools_input_wake(s_shell)); advance(800);
        CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    }
    awake_face(); press("boot", "short");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    press("boot", "short");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);

    /* Wake alone renews both clocks for repeated 2 s developer commands. */
    for (unsigned i = 0; i < 20; i++) {
        advance(2000);
        CHECK(orb_devtools_input_wake(s_shell));
        CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
        CHECK(lv_display_get_inactive_time(NULL) == 0);
    }
    /* Pointer contacts must independently renew the power controller, even
     * when no preceding wake helper runs and this is the second indev. */
    for (unsigned i = 0; i < 20; i++) {
        advance(2000);
        s_touch.point = (lv_point_t){233, 145};
        s_touch.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_developer); advance(40);
        s_touch.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_developer); advance(40);
        CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
        CHECK(!orb_shell_power_snapshot(s_shell).redact);
        CHECK(lv_display_get_inactive_time(NULL) < 100);
    }
    advance(31000);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_devtools_input_goto(s_shell, ORB_SHELL_NOTIFICATIONS)); advance(1600);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATIONS);
    orb_home_load_demo();
    CHECK(orb_devtools_input_goto(s_shell, ORB_SHELL_HOME)); advance(1600);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_HOME);
    CHECK(orb_shell_nav_current(s_shell) == ORB_SHELL_HOME);
    CHECK(semantic(orb_shell_root(s_shell), "room:Living Room"));
    /* Match the lead's pause between commands with power policy enabled;
     * the second synthetic pointer must still deliver a stationary room tap. */
    advance(2000);
    lv_obj_t *room = semantic(orb_shell_root(s_shell), "room:Living Room"); CHECK(room);
    lv_area_t room_bounds; lv_obj_get_coords(room, &room_bounds);
    s_touch.point = (lv_point_t){(room_bounds.x1 + room_bounds.x2) / 2,
        (room_bounds.y1 + room_bounds.y2) / 2};
    s_touch.state = LV_INDEV_STATE_PRESSED; advance(80);
    s_touch.state = LV_INDEV_STATE_RELEASED; advance(1200);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(orb_shell_nav_current(s_shell) == ORB_SHELL_HOME);
    CHECK(!semantic(orb_shell_root(s_shell), "room:Living Room"));
    CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
    /* The real board keeps its live power controller while devtools replaces
     * the active screen with a private scene. Explicit wake must precede the
     * private fixture, and every current surface must actually move. */
    awake_face();
    lv_obj_t *saved = lv_screen_active();
    for (unsigned surface = 0; surface < ORB_SCROLL_SURFACE_COUNT; surface++) {
        CHECK(orb_devtools_input_wake(s_shell));
        lv_obj_t *screen = lv_obj_create(NULL); CHECK(screen);
        lv_obj_remove_style_all(screen);
        lv_obj_set_size(screen, 466, 466);
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_indev_t *head = lv_indev_get_next(NULL);
        orb_scroll_fixture_t *fixture = orb_devtools_scroll_fixture_create(screen, surface);
        CHECK(fixture);
        CHECK(lv_indev_get_next(NULL) != head);
        lv_screen_load(screen); advance(1000);
        CHECK(orb_scroll_fixture_prepare(fixture));
        bool moved = false;
        int32_t initial = orb_scroll_fixture_position(fixture);
        for (unsigned ms = 0; ms <= 650; ms += 4) {
            orb_scroll_fixture_tick(fixture, ms); advance(4);
            moved |= orb_scroll_fixture_position(fixture) != initial;
        }
        CHECK(moved);
        CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
        CHECK(!orb_shell_power_snapshot(s_shell).redact);
        orb_scroll_fixture_destroy(fixture);
        lv_screen_load(saved); lv_obj_delete(screen); advance(100);
    }
    /* Deleting either pointer first cannot strand a callback descriptor. */
    lv_indev_delete(s_developer); s_developer = NULL; advance(20);
    orb_shell_destroy(s_shell);
    lv_indev_delete(physical);
    lv_deinit();
    printf("devtools input: %u checks passed\n", s_checks);
    return 0;
}
