/* Behavior regressions use the production shell, Time service and kit tree.
 * Private fixtures never mount storage or contact a board or live service. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_time.h"
#include "sim_display.h"
#include "../../firmware/components/orb_shell/orb_shell_power.h"

static unsigned s_checks;
static uint32_t s_now;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static const char *s_out;
#define CHECK(v) do { ++s_checks; if (!(v)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); exit(1); } } while (0)
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *out) { (void)input; *out = s_input; }
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) {
        s_now += 20; orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}
static lv_obj_t *text(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    const char *rim = orb_kit_rim_text_get(root);
    if ((rim && !strcasecmp(rim, caption)) ||
        (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), caption))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = text(lv_obj_get_child(root, i), caption); if (found) return found;
    }
    return NULL;
}
static void event(orb_button_t button, orb_button_gesture_t gesture)
{
    orb_button_event_t value = {.button = button, .gesture = gesture, .at_ms = s_now};
    orb_shell_button(s_shell, &value); advance(20);
}
static void boot(orb_button_gesture_t gesture)
{
    event(ORB_BUTTON_BOOT, ORB_BUTTON_DOWN);
    if (gesture != ORB_BUTTON_HOLD) event(ORB_BUTTON_BOOT, ORB_BUTTON_UP);
    event(ORB_BUTTON_BOOT, gesture); advance(800);
}
static void shot(const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", s_out, name);
    CHECK(sim_display_save_png(path));
}
static void click_text(const char *caption)
{
    lv_obj_t *label = text(orb_shell_root(s_shell), caption); CHECK(label);
    lv_obj_t *button = lv_obj_get_parent(label);
    lv_obj_send_event(button, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(button, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL); advance(800);
}
static void begin_alert(unsigned kind)
{
    orb_time_init(); advance(20);
    const time_t wall = 1700000000;
    if (kind == 0) {
        struct tm local; CHECK(localtime_r(&wall, &local));
        CHECK(orb_time_alarm_set((unsigned)local.tm_hour, (unsigned)local.tm_min));
        CHECK(orb_time_alarm_enable(true, wall - 60));
        orb_time_poll(1000, wall);
    } else if (kind == 1) {
        CHECK(orb_time_timer_set(1)); CHECK(orb_time_timer_start(1000));
        orb_time_poll(1001000, wall);
    } else {
        uint32_t reminder;
        orb_time_poll(1000, wall);
        CHECK(orb_time_reminder_set("PRIVATE REMINDER", wall, &reminder));
        orb_time_poll(1000, wall);
    }
    advance(40); CHECK(orb_shell_time_alert_active(s_shell));
}
static void privacy(void)
{
    orb_session_t session = {.agent = ORB_AGENT_CLAUDE, .state = ORB_SESSION_BUSY};
    snprintf(session.id, sizeof(session.id), "private-task");
    snprintf(session.title, sizeof(session.title), "PRIVATE TASK");
    CHECK(orb_model_upsert_session(&session));
    orb_shell_select_session(s_shell, session.id); advance(800);
    orb_shell_power_enable(s_shell, true);
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); advance(500);
    orb_shell_power(s_shell); advance(800);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    begin_alert(0); advance(800);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(text(lv_display_get_layer_top(s_display), "Stop"));
    /* Real status, preferences and IMU delivery keep the alert lease and the
     * retained lock independent; none is authorization to expose the task. */
    orb_shell_power_imu(s_shell, 0, 0, 1, 0);
    orb_power_preferences_t preferences = orb_shell_power_preferences(s_shell);
    orb_shell_set_power_preferences(s_shell, &preferences); advance(20);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    shot("locked-alarm");
    /* Back dismisses the presentation, retaining both service latch and lock. */
    boot(ORB_BUTTON_SHORT);
    CHECK(!orb_shell_time_alert_active(s_shell));
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(text(lv_display_get_layer_top(s_display), ORB_ICON_LOCK));
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSION);
    shot("locked-alarm-dismissed");
    orb_time_init(); advance(20); begin_alert(0);
    boot(ORB_BUTTON_HOLD);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(!orb_shell_time_alert_active(s_shell));
    CHECK(text(lv_display_get_layer_top(s_display), ORB_ICON_MIC " Listening"));
    orb_voice_request_t request;
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_START);
    event(ORB_BUTTON_BOOT, ORB_BUTTON_UP);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_STOP);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    /* An explicit Stop touch acknowledges the alert, never the task lock. */
    orb_time_init(); advance(20); begin_alert(0); advance(500);
    lv_obj_t *stop = text(lv_display_get_layer_top(s_display), "Stop"); CHECK(stop);
    lv_area_t bounds; lv_obj_get_coords(lv_obj_get_parent(stop), &bounds);
    s_input.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); advance(80);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(800);
    CHECK(!orb_shell_time_alert_active(s_shell));
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
}
static void alerts(void)
{
    for (unsigned kind = 0; kind < 3; ++kind) {
        const orb_button_gesture_t gestures[] = {ORB_BUTTON_SHORT, ORB_BUTTON_DOUBLE, ORB_BUTTON_HOLD};
        for (unsigned g = 0; g < 3; ++g) {
            orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); advance(800);
            begin_alert(kind);
            event(ORB_BUTTON_BOOT, ORB_BUTTON_DOWN);
            CHECK(orb_shell_time_alert_active(s_shell));
            event(ORB_BUTTON_BOOT, ORB_BUTTON_UP);
            CHECK(orb_shell_time_alert_active(s_shell));
            boot(gestures[g]);
            if (gestures[g] == ORB_BUTTON_HOLD) {
                lv_obj_t *mic = text(lv_display_get_layer_top(s_display), ORB_ICON_MIC " Listening");
                CHECK(mic);
                CHECK(lv_obj_get_width(mic) == SIM_D && lv_obj_get_height(mic) == SIM_D);
                CHECK(lv_obj_is_clickable(mic));
                CHECK(!orb_shell_time_alert_active(s_shell));
                static const char *names[] = {"alarm-listening", "timer-listening", "reminder-listening"};
                shot(names[kind]);
                orb_voice_request_t request;
                CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_START);
                event(ORB_BUTTON_BOOT, ORB_BUTTON_UP);
                CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_STOP);
                CHECK(!orb_model_take_voice_request(&request));
            } else {
                CHECK(!orb_shell_time_alert_active(s_shell));
                CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
            }
            orb_time_snapshot_t time; orb_time_snapshot(orb_time_now_us(), &time);
            CHECK(kind == 0 ? time.alarm_ringing : kind == 1 ? time.timer_state == ORB_TIME_FINISHED : time.reminder_due);
            orb_time_init(); advance(40);
            orb_answer_t answer; CHECK(!orb_model_peek_answer(&answer));
        }
    }
}
static void gallery(void)
{
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(800);
    s_input.point = (lv_point_t){233, 233}; s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer); advance(800);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(800);
    CHECK(text(orb_shell_root(s_shell), "Edit"));
    click_text("Edit"); CHECK(text(orb_shell_root(s_shell), "Slots"));
    click_text("Slots");
    boot(ORB_BUTTON_SHORT);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(text(orb_shell_root(s_shell), "Slots"));
    boot(ORB_BUTTON_DOUBLE); CHECK(text(orb_shell_root(s_shell), "Edit"));
    boot(ORB_BUTTON_SHORT);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(!text(orb_shell_root(s_shell), "Edit"));
    boot(ORB_BUTTON_SHORT); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
}
static void close_sheet(void)
{
    event(ORB_BUTTON_PWR, ORB_BUTTON_HOLD); advance(800);
    lv_obj_t *x = text(lv_display_get_layer_top(s_display), ORB_ICON_X); CHECK(x);
    lv_obj_t *close = lv_obj_get_parent(x);
    CHECK(lv_obj_get_width(close) == ORB_KIT_DISC_MINI);
    lv_area_t bounds; lv_obj_get_coords(close, &bounds);
    CHECK((bounds.x1 + bounds.x2 + 1) / 2 == 101 && (bounds.y1 + bounds.y2 + 1) / 2 == 101);
    CHECK(text(lv_display_get_layer_top(s_display), "Restart"));
    shot("power-sheet-close");
    lv_obj_send_event(close, LV_EVENT_CLICKED, NULL); advance(800);
    CHECK(!text(lv_display_get_layer_top(s_display), "Restart"));
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
}
int main(int argc, char **argv)
{
    CHECK(argc >= 3); s_out = argv[2];
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init()); orb_time_init();
    orb_model_reset(); orb_model_set_connection(true, true);
    if (argc > 3) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    s_pointer = lv_indev_create(); lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display); lv_indev_set_read_cb(s_pointer, read_pointer);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .brightness = 70};
    snprintf(status.clock_time, sizeof(status.clock_time), "10:09");
    snprintf(status.clock_date, sizeof(status.clock_date), "Tuesday 27");
    orb_shell_update(s_shell, &status); advance(800);
    if (!strcmp(argv[1], "privacy")) privacy();
    else if (!strcmp(argv[1], "alerts")) alerts();
    else if (!strcmp(argv[1], "gallery")) gallery();
    else if (!strcmp(argv[1], "close")) close_sheet();
    else CHECK(false);
    orb_shell_destroy(s_shell); lv_indev_delete(s_pointer); lv_deinit();
    printf("fixes1 shell %s: %u checks passed\n", argv[1], s_checks); return 0;
}
