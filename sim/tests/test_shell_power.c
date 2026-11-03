/* Integrated power privacy/input regression: real shell and retained RGB565
 * surface, deterministic time, no PMU/network/audio or owner state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_sound.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "sim_display.h"
#include "../../firmware/components/orb_shell/orb_shell_power.h"

static unsigned s_checks;
static uint32_t s_now;
static orb_shell_t *s_shell;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_pointer_data;
static const char *s_out;
#define CHECK(value) do { s_checks++; if (!(value)) { \
    fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #value); exit(1); } } while (0)
static uint32_t tick(void) { return s_now; }
static void pointer_read(lv_indev_t *input, lv_indev_data_t *out)
{
    (void)input;
    *out = s_pointer_data;
}
static void step(uint32_t ms)
{
    s_now += ms;
    orb_shell_poll(s_shell);
    lv_timer_handler();
    lv_refr_now(s_display);
}
static void button(orb_button_gesture_t gesture)
{
    orb_button_event_t event = {.button = ORB_BUTTON_PWR, .gesture = gesture, .at_ms = s_now};
    orb_shell_button(s_shell, &event);
}
static void raw_button(orb_buttons_t *recognizer, unsigned mask, uint32_t ms, bool frame_ready)
{
    step(ms);
    if (frame_ready) {
        /* step() completed the real host framebuffer. The hardware UI owner
         * also renews the receipt when a short RM wake reaches its endpoint. */
        orb_power_snapshot_t presented = orb_shell_power_snapshot(s_shell);
        orb_shell_power_display_ready(s_shell, presented.revision, true);
    }
    orb_buttons_poll(recognizer, s_now, mask);
    orb_button_event_t event;
    while (orb_buttons_next(recognizer, &event)) {
        orb_power_snapshot_t policy = orb_shell_power_snapshot(s_shell);
        bool consume = policy.state != ORB_POWER_ACTIVE || policy.redact ||
                       policy.brightness_pct <= 10 || !frame_ready;
        if (event.button == ORB_BUTTON_PWR && event.gesture == ORB_BUTTON_DOWN && consume)
            orb_buttons_consume_press(recognizer, ORB_BUTTON_PWR);
        orb_shell_button(s_shell, &event);
    }
}

static void notice_swipe(lv_point_t start, lv_point_t end)
{
    /* The completed host framebuffer is the board's ordinary wake receipt. */
    orb_power_snapshot_t presented = orb_shell_power_snapshot(s_shell);
    orb_shell_power_display_ready(s_shell, presented.revision, true);
    s_pointer_data = (lv_indev_data_t){.point=start, .state=LV_INDEV_STATE_PRESSED};
    lv_indev_read(s_pointer); step(40);
    for (int i = 1; i <= 8; i++) {
        s_pointer_data.point = (lv_point_t){start.x + (end.x-start.x)*i/8, start.y + (end.y-start.y)*i/8};
        lv_indev_read(s_pointer); step(30);
    }
    s_pointer_data.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); step(400);
}

static lv_obj_t *label(lv_obj_t *object, const char *text)
{
    if (!lv_obj_is_visible(object)) return NULL;
    if (orb_kit_rim_text_get(object) &&
        !strcasecmp(orb_kit_rim_text_get(object), text)) return object;
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = label(lv_obj_get_child(object, i), text);
        if (found) return found;
    }
    return NULL;
}
static bool stored_notice(lv_obj_t *object)
{
    const char *rim = orb_kit_rim_text_get(object);
    if (rim && !strcasecmp(rim, "Battery")) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++)
        if (stored_notice(lv_obj_get_child(object, i))) return true;
    return false;
}

static bool has_text(lv_obj_t *object, const char *text)
{
    if (!lv_obj_is_visible(object)) return false;
    if (lv_obj_check_type(object, &lv_label_class) && strstr(lv_label_get_text(object), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++)
        if (has_text(lv_obj_get_child(object, i), text)) return true;
    return false;
}
static void shot(const char *name)
{
    if (!s_out) return;
    char path[512]; snprintf(path, sizeof(path), "%s/%s.png", s_out, name);
    CHECK(sim_display_save_png(path));
}
int main(int argc, char **argv)
{
    s_out = argc > 2 ? argv[2] : NULL;
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL);
    CHECK(orb_model_init()); CHECK(orb_home_init());
    orb_model_reset(); orb_model_set_connection(true, true);
    orb_model_weather_capability(true);
    orb_weather_snapshot_t weather = {.configured = true, .available = true,
        .temperature = 72, .high = 75, .low = 62, .unit = ORB_TEMPERATURE_F,
        .condition = ORB_WEATHER_CLEAR, .is_day = true, .updated_at = 1};
    snprintf(weather.place, sizeof(weather.place), "Test weather");
    snprintf(weather.label, sizeof(weather.label), "Sunny");
    CHECK(orb_model_set_weather(&weather));
    if (argc > 1 && strcmp(argv[1], "default")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(true);
        orb_ui_set_reduce_motion(true);
    }
    s_pointer = lv_indev_create(); CHECK(s_pointer);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, pointer_read);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    CHECK(!stored_notice(lv_display_get_layer_top(s_display)));
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .usb = true, .brightness = 70};
    snprintf(status.clock_time, sizeof(status.clock_time), "10:09");
    snprintf(status.clock_date, sizeof(status.clock_date), "Tuesday, October 27");
    orb_shell_update(s_shell, &status); step(800);
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); step(800);
    orb_shell_power_enable(s_shell, true); step(20);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    /* Boot's automatic Active surface stays safe until deliberate input. */
    button(ORB_BUTTON_DOWN); button(ORB_BUTTON_UP); button(ORB_BUTTON_SHORT); step(500);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    CHECK(!orb_shell_power_snapshot(s_shell).redact);
    /* A sustained drag is deliberate activity for its whole contact, not
     * only its initial DOWN. Reading/adjusting past the dim deadline must not
     * lose the finger to a redacted overlay midway through that gesture. */
    s_pointer_data.point = (lv_point_t){233, 240};
    s_pointer_data.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer);
    for (unsigned i = 0; i < 65; i++) {
        s_pointer_data.point.y = (int32_t)(240 - i % 40);
        lv_indev_read(s_pointer);
        step(500);
        CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
        CHECK(!orb_shell_power_snapshot(s_shell).redact);
    }
    /* A PWR/idle lock imposed during that contact wins. Retained raw state
     * is activity only while already revealed; it never becomes a new wake. */
    orb_shell_calm_lock(orb_shell_calm_context(s_shell));
    for (unsigned i = 0; i < 4; i++) {
        lv_indev_read(s_pointer); step(500);
        CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
        CHECK(orb_shell_power_snapshot(s_shell).redact);
    }
    s_pointer_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    button(ORB_BUTTON_DOWN); button(ORB_BUTTON_UP); button(ORB_BUTTON_SHORT); step(500);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    CHECK(!orb_shell_power_snapshot(s_shell).redact);
    /* Exact deadlines run while preserving the current place and pending work. */
    step(20000);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_DIMMED);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    step(10000); step(500);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_shell_power_snapshot(s_shell).brightness_pct == 9);
    lv_obj_t *top = lv_display_get_layer_top(s_display);
    orb_faces_snapshot_t public_face;
    CHECK(orb_shell_face_snapshot(s_shell, &public_face));
    CHECK(!strcmp(public_face.weekday, "Tuesday") && !strcmp(public_face.day, "27"));
    CHECK(label(top, "10:09"));
    CHECK(!label(top, "0 waiting"));
    CHECK(label(top, ORB_ICON_LOCK));
    CHECK(label(top, "Tuesday"));
    CHECK(has_text(top, "72°") && has_text(top, "Sunny"));
    shot("always-on-empty");
    /* A new pending decision wakes only a redacted glance; it cannot expose
     * the private title or change the underlying Settings navigation. */
    orb_session_t session = {.agent = ORB_AGENT_CLAUDE, .state = ORB_SESSION_WAITING, .can_message = true};
    snprintf(session.id, sizeof(session.id), "private-session");
    snprintf(session.title, sizeof(session.title), "PRIVATE SESSION");
    CHECK(orb_model_upsert_session(&session));
    orb_card_t card = {.kind = ORB_CARD_APPROVAL, .options_count = 2};
    snprintf(card.session, sizeof(card.session), "%s", session.id);
    snprintf(card.id, sizeof(card.id), "private-approval");
    snprintf(card.title, sizeof(card.title), "PRIVATE SHELL COMMAND");
    snprintf(card.options[0].id, sizeof(card.options[0].id), "allow");
    snprintf(card.options[0].label, sizeof(card.options[0].label), "Allow");
    snprintf(card.options[1].id, sizeof(card.options[1].id), "deny");
    snprintf(card.options[1].label, sizeof(card.options[1].label), "Deny");
    card.options[1].role = ORB_OPTION_CANCEL;
    CHECK(orb_model_upsert_card(&card)); step(20); step(500);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(!orb_sound_play(ORB_SOUND_NOTIFICATION));
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    CHECK(!label(top, "1 waiting")); CHECK(!label(top, "PRIVATE SHELL COMMAND"));
    CHECK(label(top, "Tuesday"));
    CHECK(label(top, ORB_ICON_LOCK));
    shot("attention-glance");
    /* The entire wake touch is consumed; its release cannot activate Settings
     * or the task under the overlay. A later deliberate press is ordinary. */
    s_pointer_data.point = (lv_point_t){233, 233};
    s_pointer_data.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer); step(30);
    s_pointer_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer); step(500);
    CHECK(!orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    button(ORB_BUTTON_DOWN); button(ORB_BUTTON_UP); button(ORB_BUTTON_SHORT); step(500);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_shell_power_snapshot(s_shell).redact && !orb_shell_quiet(s_shell));
    /* Developer pointer input can inspect but never reveal a locked surface. */
    orb_input_mark_synthetic(s_pointer);
    s_pointer_data.point = (lv_point_t){233, 233};
    s_pointer_data.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); step(30);
    s_pointer_data.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); step(30);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    lv_indev_set_user_data(s_pointer, NULL);
    /* An early lock remains AOD across the normal twenty-second Dimmed
     * deadline. The whole waking contact leaves the prior place intact. */
    step(21000);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    orb_shell_power_imu(s_shell, 0, 0, 1, 0);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_shell_power_snapshot(s_shell).moment == ORB_POWER_MOMENT_NONE);
    button(ORB_BUTTON_DOWN); button(ORB_BUTTON_UP); button(ORB_BUTTON_SHORT); step(500);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    orb_power_preferences_t preferences = orb_shell_power_preferences(s_shell);
    preferences.brightness_pct = 64;
    orb_shell_set_power_preferences(s_shell, &preferences);
    preferences.brightness_pct = 10;
    orb_shell_set_power_preferences(s_shell, &preferences);
    button(ORB_BUTTON_DOWN); button(ORB_BUTTON_UP); button(ORB_BUTTON_SHORT); step(500);
    CHECK(orb_shell_power_snapshot(s_shell).brightness_pct == 64);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); step(800);
    /* Quiet suppresses attention wakes; stable AOD incurs no redraws. */
    orb_shell_set_quiet(s_shell, true);
    CHECK(!orb_sound_play(ORB_SOUND_NOTIFICATION)); /* No new IMU sample needed. */
    step(500);
    CHECK(orb_shell_quiet(s_shell));
    CHECK(orb_shell_power_snapshot(s_shell).silent);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    sim_display_take_changed();
    for (unsigned i = 0; i < 20; i++) step(20);
    CHECK(!sim_display_take_changed());
    lv_obj_t *desk_clock = label(top, "10:09"); CHECK(desk_clock);
    lv_area_t before; lv_obj_get_coords(desk_clock, &before);
    CHECK(orb_model_cancel_card(card.id)); step(20);
    step(300);
    CHECK(!label(top, "1 waiting")); CHECK(!label(top, "0 waiting"));
    lv_area_t after; lv_obj_get_coords(desk_clock, &after);
    /* Lock keeps the chosen face stable. Private attention never reflows its
     * clock or substitutes an attention dashboard for the face. */
    CHECK(after.y1 == before.y1);
    CHECK(orb_model_upsert_card(&card)); step(20); step(300);
    CHECK(!label(top, "1 waiting"));
    lv_obj_get_coords(desk_clock, &after); CHECK(after.y1 == before.y1);
    /* The entire chosen face follows the one discrete minute corner. */
    orb_power_snapshot_t corner = orb_shell_power_snapshot(s_shell);
    int clock_x = after.x1;
    lv_obj_t *lock = label(top, ORB_ICON_LOCK); CHECK(lock);
    lv_area_t lock_before; lv_obj_get_coords(lock, &lock_before);
    uint64_t boundary = (corner.now_ms / 60000 + 1) * 60000;
    uint64_t delta = boundary - corner.now_ms + 1000;
    step((uint32_t)delta);
    orb_power_snapshot_t shifted = orb_shell_power_snapshot(s_shell);
    lv_obj_get_coords(desk_clock, &after);
    lv_area_t lock_after; lv_obj_get_coords(lock, &lock_after);
    CHECK(after.x1 - clock_x == shifted.shift_x - corner.shift_x);
    CHECK(lock_after.x1 - lock_before.x1 == shifted.shift_x - corner.shift_x);
    orb_shell_set_quiet(s_shell, false);
    unsigned midway = orb_ui_reduce_motion() ? 75 : 200;
    step(midway);
    if (orb_ui_reduce_motion()) {
        /* Reduce Motion retains the face while its safe wake fades out. */
        lv_obj_t *source_clock = label(top, "10:09"); CHECK(source_clock);
        CHECK(lv_obj_get_y(source_clock) > 100);
        CHECK(!label(top, "PRIVATE SESSION") && !label(top, "PRIVATE SHELL COMMAND"));
    }
    step(500 - midway);
    CHECK(!orb_shell_quiet(s_shell)); CHECK(!orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_sound_play(ORB_SOUND_NOTIFICATION));
    /* Confirmed face-down overrides manual wakes; exit itself does not wake. */
    for (unsigned i = 0; i < 30; i++) { step(50); orb_shell_power_imu(s_shell, 0, 0, -1, 0); }
    CHECK(orb_shell_power_snapshot(s_shell).face_down);
    CHECK(!orb_shell_power_snapshot(s_shell).panel_on);
    CHECK(!orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted);
    for (unsigned i = 0; i < 40; i++) { step(50); orb_shell_power_imu(s_shell, 0, 0, 1, 0); }
    CHECK(!orb_shell_power_snapshot(s_shell).face_down);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_SLEEP);
    /* Pick-up restores the Face glance over the retained place; it never
     * clears private redaction, answers a card or plays an attention cue. */
    orb_home_load_demo();
    size_t home_on = 0;
    CHECK(orb_home_glance_count(&home_on));
    CHECK(home_on > 0);
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PICKUP).accepted); step(500);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_shell_face_snapshot(s_shell, &public_face));
    CHECK(public_face.home_on == home_on && public_face.home_total >= home_on);
    CHECK(public_face.home_total > 0 && !public_face.home_stale);
    unsigned known_home_total = public_face.home_total, known_home_on = public_face.home_on;
    /* The shared snapshot retains household state; the locked presenter
     * withholds its presence-revealing count while keeping its provider. */
    CHECK(label(top, ORB_ICON_HOUSE) && !has_text(top, " on"));
    CHECK(label(top, "Tuesday")); CHECK(label(top, ORB_ICON_LOCK));
    CHECK(has_text(top, "72°") && has_text(top, "Sunny"));
    CHECK(!label(top, "PRIVATE SESSION") && !label(top, "PRIVATE SHELL COMMAND"));
    CHECK(!orb_sound_play(ORB_SOUND_NOTIFICATION));
    shot("pickup-glance");
    orb_home_set_connection(false, false); step(20);
    CHECK(!orb_home_glance_count(&home_on));
    CHECK(orb_shell_face_snapshot(s_shell, &public_face));
    CHECK(public_face.home_total == known_home_total && public_face.home_on == known_home_on && public_face.home_stale);
    CHECK(label(top, ORB_ICON_HOUSE) && !has_text(top, " on"));
    orb_shell_power_sleep(s_shell); step(500);
    /* Unknown battery never invents critical charge. */
    status.battery_pct = -1; status.usb = false;
    orb_shell_update(s_shell, &status);
    CHECK(orb_shell_power_snapshot(s_shell).battery_mode == ORB_POWER_BATTERY_NORMAL);
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted);
    orb_power_snapshot_t dark_wake = orb_shell_power_snapshot(s_shell);
    CHECK(dark_wake.wake_from_sleep && dark_wake.moment_elapsed_ms == 0);
    lv_refr_now(s_display);
    static uint8_t fade_rgb[SIM_D * SIM_D * 3];
    sim_display_rgb(fade_rgb);
    unsigned maximum = 0;
    for (unsigned i = 0; i < sizeof(fade_rgb); i++) maximum = LV_MAX(maximum, fade_rgb[i]);
    CHECK(maximum == 0); /* The first safe dark-wake frame has no emitted ink. */
    /* Check emitted pixels, independent of the face's retained kit internals. */
    step(dark_wake.moment_duration_ms / 2);
    sim_display_rgb(fade_rgb);
    maximum = 0;
    for (unsigned i = 0; i < sizeof(fade_rgb); i++) maximum = LV_MAX(maximum, fade_rgb[i]);
    CHECK(maximum > 0); /* Normal safe ink / RM fixed destination fades in. */
    step(500 - dark_wake.moment_duration_ms / 2);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    /* A second physical gesture during an unacknowledged wake remains
     * consumed even after the controller itself has become Active. */
    orb_shell_power_display_ready(s_shell, 0, false);
    button(ORB_BUTTON_DOWN); button(ORB_BUTTON_UP); button(ORB_BUTTON_SHORT); step(500);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    orb_power_snapshot_t ready = orb_shell_power_snapshot(s_shell);
    orb_shell_power_display_ready(s_shell, ready.revision, true);
    button(ORB_BUTTON_DOWN); button(ORB_BUTTON_UP); button(ORB_BUTTON_SHORT); step(500);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    /* Exercise the main-owned raw recognizer too. Its wake click is removed
     * before a second press inside the 300 ms double window, so that second
     * press locks again rather than opening Control Center. */
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); step(800);
    orb_shell_set_quiet(s_shell, true); step(500);
    orb_buttons_t recognizer; orb_buttons_init(&recognizer);
    raw_button(&recognizer, 1u << ORB_BUTTON_PWR, 1, false);
    raw_button(&recognizer, 1u << ORB_BUTTON_PWR, 40, false);
    raw_button(&recognizer, 0, 40, false);
    raw_button(&recognizer, 0, 40, false);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    ready = orb_shell_power_snapshot(s_shell);
    orb_shell_power_display_ready(s_shell, ready.revision, true);
    raw_button(&recognizer, 1u << ORB_BUTTON_PWR, 40, true);
    raw_button(&recognizer, 1u << ORB_BUTTON_PWR, 40, true);
    raw_button(&recognizer, 0, 40, true);
    raw_button(&recognizer, 0, 40, true);
    raw_button(&recognizer, 0, 320, true);
    step(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    orb_shell_power_sleep(s_shell); step(500);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_SLEEP);
    CHECK(!orb_shell_power_snapshot(s_shell).panel_on);
    /* BOOT keeps its established Back / hold-to-talk mapping from dark;
     * wake filtering must never turn its UP into an extra Back action. */
    orb_button_event_t boot = {.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_DOWN, .at_ms = s_now};
    orb_shell_button(s_shell, &boot);
    boot.gesture = ORB_BUTTON_HOLD; orb_shell_button(s_shell, &boot);
    orb_voice_request_t voice;
    CHECK(orb_model_take_voice_request(&voice) && voice.action == ORB_VOICE_START);
    orb_shell_surface_t talk_surface = orb_shell_surface(s_shell);
    boot.gesture = ORB_BUTTON_UP; orb_shell_button(s_shell, &boot);
    CHECK(orb_model_take_voice_request(&voice) && voice.action == ORB_VOICE_STOP);
    CHECK(!orb_model_take_voice_request(&voice));
    CHECK(orb_shell_surface(s_shell) == talk_surface);
    /* Face already has a Battery complication. A floating battery banner
     * would cross its Calendar/weather, so the public face stays unobscured
     * at both warning thresholds. Critical sleep still wins. */
    status.battery_pct = 50; status.usb = status.charging = false;
    orb_shell_update(s_shell, &status);
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); step(500);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); step(800);
    status.battery_pct = 20; orb_shell_update(s_shell, &status); step(20);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(!label(top, "20% · Battery"));
    CHECK(label(orb_shell_root(s_shell), "Tuesday") && label(orb_shell_root(s_shell), "10:09"));
    CHECK(has_text(orb_shell_root(s_shell), "72°") && has_text(orb_shell_root(s_shell), "Sunny"));
    shot("face-battery-low");
    status.battery_pct = 5; orb_shell_update(s_shell, &status); step(20);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_SLEEP);
    CHECK(!label(top, "5% · Battery low"));
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); step(500);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(!label(top, "5% · Battery low"));
    CHECK(label(orb_shell_root(s_shell), "Tuesday") && label(orb_shell_root(s_shell), "10:09"));
    CHECK(has_text(orb_shell_root(s_shell), "72°") && has_text(orb_shell_root(s_shell), "Sunny"));
    shot("face-battery-critical");
    /* Other apps retain public, quiet battery banners and their hysteresis.
     * Recover charge first so this is a fresh threshold episode. */
    status.battery_pct = 50; status.usb = false; status.charging = false;
    orb_shell_update(s_shell, &status);
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); step(500);
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); step(800);
    status.battery_pct = 20; orb_shell_update(s_shell, &status); step(20);
    CHECK(label(top, "20% · Battery")); shot("battery-low");
    /* A shared, exclusive notice lane must own an opaque backing. No task
     * row or rim title may be painted underneath the actionable notice. */
    lv_obj_t *notice_chip = lv_obj_get_parent(label(top, "20% · Battery"));
    lv_obj_t *notice_lane = lv_obj_get_parent(notice_chip);
    CHECK(lv_obj_get_style_bg_opa(notice_lane, 0) == LV_OPA_COVER);
    CHECK(lv_obj_get_width(notice_lane) == 466 && lv_obj_get_height(notice_lane) == 466);
    CHECK(label(notice_lane, "Battery"));
    notice_swipe((lv_point_t){233,300}, (lv_point_t){360,300});
    CHECK(label(top, "20% · Battery")); /* Centre never promotes to edge. */
    notice_swipe((lv_point_t){55,100}, (lv_point_t){190,100});
    CHECK(!label(top, "20% · Battery")); /* Radial left edge, outside x<40. */
    CHECK(!stored_notice(top)); /* Dismissal releases the hidden kit/material tree. */
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    status.battery_pct = 50; orb_shell_update(s_shell, &status); step(20);
    orb_shell_navigate(s_shell, ORB_SHELL_ONBOARDING); step(800);
    for (unsigned i = 0; i < 60 && !label(orb_shell_root(s_shell), "Welcome to Orb"); i++) step(20);
    CHECK(label(orb_shell_root(s_shell), "Welcome to Orb")); step(100);
    uint8_t *before_notice = malloc(SIM_D * SIM_D * 3), *after_notice = malloc(SIM_D * SIM_D * 3);
    CHECK(before_notice && after_notice); sim_display_rgb(before_notice); shot("onboarding-before-notice");
    status.battery_pct = 20; orb_shell_update(s_shell, &status); step(20);
    CHECK(label(top, "20% · Battery")); shot("onboarding-low-notice");
    orb_shell_button(s_shell, &(orb_button_event_t){.button=ORB_BUTTON_BOOT, .gesture=ORB_BUTTON_SHORT});
    step(800); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_ONBOARDING);
    CHECK(!label(top, "20% · Battery")); sim_display_rgb(after_notice);
    shot("onboarding-after-notice-back");
    CHECK(!memcmp(before_notice, after_notice, SIM_D * SIM_D * 3));
    free(before_notice); free(after_notice); shot("onboarding-after-notice-back");
    status.battery_pct = 50; orb_shell_update(s_shell, &status); step(20);
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); step(800);
    status.battery_pct = 20; orb_shell_update(s_shell, &status); step(20);
    CHECK(label(top, "20% · Battery"));
    CHECK(orb_shell_double_tap(s_shell));
    CHECK(!label(top, "20% · Battery"));
    CHECK(!stored_notice(top));
    for (int i = 0; i < 3; i++) { orb_shell_update(s_shell, &status); step(20); }
    CHECK(!label(top, "20% · Battery"));
    status.battery_pct = 21; orb_shell_update(s_shell, &status);
    status.battery_pct = 20; orb_shell_update(s_shell, &status); step(20);
    CHECK(!label(top, "20% · Battery"));
    status.battery_pct = 5; orb_shell_update(s_shell, &status); step(20);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_SLEEP);
    CHECK(!label(top, "5% · Battery low"));
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); step(500);
    /* The common secondary capsule keeps critical battery copy on one line. */
    lv_obj_t *warning = label(top, "5% · Battery low"); CHECK(warning);
    shot("battery-critical");
    lv_obj_send_event(lv_obj_get_parent(warning), LV_EVENT_CLICKED, NULL); step(500);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_BATTERY);
    CHECK(!label(top, "5% · Battery low"));
    status.battery_pct = 4; orb_shell_update(s_shell, &status); step(20);
    CHECK(!label(top, "4% · Battery low"));
    status.charging = true; status.usb = true; orb_shell_update(s_shell, &status); step(20);
    status.battery_pct = 20; status.charging = status.usb = false;
    orb_shell_update(s_shell, &status); step(20);
    /* The open Battery screen already explains this update. Suppress its
     * echo notice by the same policy as a visible level control's HUD. */
    CHECK(!label(top, "20% · Battery"));
    status.charging = true; orb_shell_update(s_shell, &status); step(20);
    CHECK(!label(top, "20% · Battery"));
    status.battery_pct = -1; status.charging = false; orb_shell_update(s_shell, &status); step(20);
    CHECK(!label(top, "20% · Battery"));
    /* Every public route shares the same PWR path. Keep the resolved surface
     * intact through lock/wake, including system screens and
     * empty detail routes without a selected item. */
    status.battery_pct = 80; status.usb = true;
    orb_shell_update(s_shell, &status);
    static const orb_shell_surface_t routes[] = {
        ORB_SHELL_FACE, ORB_SHELL_STACK, ORB_SHELL_APPS, ORB_SHELL_AGENTS,
        ORB_SHELL_SESSION, ORB_SHELL_CARD, ORB_SHELL_NOTIFICATIONS,
        ORB_SHELL_CONTROL_CENTER, ORB_SHELL_SETTINGS, ORB_SHELL_DISPLAY,
        ORB_SHELL_ACCESSIBILITY, ORB_SHELL_ABOUT, ORB_SHELL_DOT,
        ORB_SHELL_VOICE, ORB_SHELL_PLAY, ORB_SHELL_UPDATES,
        ORB_SHELL_IDLE_SESSIONS, ORB_SHELL_SESSION_SETTINGS, ORB_SHELL_HOME,
        ORB_SHELL_UNITS, ORB_SHELL_TEMPERATURE, ORB_SHELL_SOUND, ORB_SHELL_TIME,
        ORB_SHELL_OFFLINE, ORB_SHELL_CHARGING, ORB_SHELL_NOTIFICATION_DETAIL,
        ORB_SHELL_COMPASS, ORB_SHELL_USAGE, ORB_SHELL_SESSIONS,
        ORB_SHELL_BATTERY, ORB_SHELL_ONBOARDING,
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        orb_shell_navigate(s_shell, ORB_SHELL_FACE); step(800);
        orb_shell_navigate(s_shell, routes[i]); step(800);
        orb_shell_surface_t prior = orb_shell_surface(s_shell);
        ready = orb_shell_power_snapshot(s_shell);
        orb_shell_power_display_ready(s_shell, ready.revision, true);
        button(ORB_BUTTON_DOWN); button(ORB_BUTTON_UP); button(ORB_BUTTON_SHORT); step(500);
        CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
        CHECK(orb_shell_power_snapshot(s_shell).redact);
        CHECK(orb_shell_surface(s_shell) == prior);
        button(ORB_BUTTON_DOWN); button(ORB_BUTTON_UP); button(ORB_BUTTON_SHORT); step(500);
        CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
        CHECK(!orb_shell_power_snapshot(s_shell).redact);
        CHECK(orb_shell_surface(s_shell) == prior);
    }
    /* Time is admitted through the actual app registry, but its deadline
     * service runs independently of display/app ticks. Merely viewing a
     * running timer must not acquire the game's indefinite keep-awake lease. */
    orb_time_init(); orb_apps_init(0);
    CHECK(orb_apps_find("time"));
    CHECK(orb_apps_enter("time") == ESP_OK);
    orb_shell_navigate(s_shell, ORB_SHELL_TIME); step(800);
    CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "time"));
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted);
    CHECK(orb_time_timer_set(120));
    CHECK(orb_time_timer_start((int64_t)s_now * 1000));
    orb_apps_poll((int64_t)s_now * 1000, false, true, 0, 0, 1, false);
    step(20);
    orb_apps_poll((int64_t)(s_now + 30000) * 1000, false, true, 0, 0, 1, false);
    step(30000); step(500);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    orb_apps_poll((int64_t)(s_now + 30000) * 1000, false, false, 0, 0, 1, false);
    step(30000);
    orb_time_snapshot_t utility;
    orb_time_snapshot((int64_t)s_now * 1000, &utility);
    CHECK(utility.timer_state == ORB_TIME_RUNNING);
    CHECK(utility.timer_remaining_us > INT64_C(58000000) &&
          utility.timer_remaining_us < INT64_C(61000000));
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    orb_apps_exit(); orb_time_init();
    orb_shell_destroy(s_shell);
    /* Deleting shell removes its indev callback as well as the top overlay. */
    s_pointer_data.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer);
    s_pointer_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    /* SDL's actual shutdown order deletes the pointer before the shell. */
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    CHECK(!stored_notice(lv_display_get_layer_top(s_display)));
    orb_shell_update(s_shell, &status);
    orb_shell_power_enable(s_shell, true);
    lv_indev_delete(s_pointer);
    orb_shell_poll(s_shell);
    orb_shell_destroy(s_shell);
    printf("shell power: %u integrated checks passed (%s)\n", s_checks, argc > 1 ? argv[1] : "Default");
    lv_deinit();
    return 0;
}
