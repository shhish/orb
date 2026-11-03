#include "orb_app_play.h"
/* Real shell routing and visible feedback driven by portable button events.
 * Synthetic holds must never start voice, answer a card or restart the device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_fs.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks, s_actions;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
static lv_indev_data_t s_input;
#define CHECK(value) do { s_checks++; if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); } } while (0)

/* Games use the production controller/view, with no mounted or persisted state. */
bool orb_fs_valid_app_id(const char *id)
{
    if (!id || !*id || strlen(id) > ORB_FS_APP_ID_MAX) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return false;
    return true;
}
bool orb_fs_ready(void) { return false; }
esp_err_t orb_fs_app_prepare(const char *id)
{
    return orb_fs_valid_app_id(id) ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t orb_fs_read(const char *id, const char *key, void *data, size_t capacity, size_t *size)
{
    (void)id; (void)key; (void)data; (void)capacity;
    *size = 0;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size)
{
    (void)id; (void)key; (void)data; (void)size;
    return ESP_OK;
}

/* A non-game app keeps the generic shell-owned pause contract covered too. */
static esp_err_t app_enter(void) { return ESP_OK; }
static void app_exit(void) {}
static void app_pause(bool paused) { (void)paused; }
static void app_tick(const orb_app_input_t *input) { (void)input; }
static const orb_app_entry_t s_app_entry = {app_enter, app_exit, app_pause, app_tick};
static const orb_app_manifest_t s_app = {
    "button-test", "Button Test", ORB_ICON_CIRCLE_FILL, 0x0091ff, 0, &s_app_entry};

static uint32_t tick(void) { return s_now; }
static void action(orb_shell_action_t value, void *ctx) { (void)value; (void)ctx; s_actions++; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *out) { (void)input; *out = s_input; }
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) {
        s_now += 20;
        orb_apps_poll((int64_t)s_now * 1000, false, true, 0, 0, 1, false);
        orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}
static lv_obj_t *label(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), caption)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = label(lv_obj_get_child(root, (int32_t)i), caption);
        if (found) return found;
    }
    if (s_shell && root == orb_shell_root(s_shell))
        return label(lv_display_get_layer_top(s_display), caption);
    return NULL;
}
/* Labels in the tree are insufficient: cold/memory-denied glass previously
 * suppressed its opaque fallback while a zero-radius blur drew no material. */
static void glass_control_painted(const char *caption)
{
    lv_obj_t *text = label(orb_shell_root(s_shell), caption);
    CHECK(text);
    lv_obj_t *control = lv_obj_get_parent(text);
    CHECK(orb_glass_get_amount(control) > 0);
    lv_area_t bounds; lv_obj_get_coords(control, &bounds);
    static uint8_t rgb[SIM_D * SIM_D * 3];
    sim_display_rgb(rgb);
    int x = (bounds.x1 + bounds.x2) / 2, y = bounds.y1 + 10;
    /* A small patch above the label lies inside either circle or capsule,
     * so only material ink can make it differ from the black sheet canvas. */
    for (int dy = 0; dy < 3; dy++) for (int dx = 0; dx < 3; dx++) {
        const uint8_t *pixel = &rgb[((y + dy) * SIM_D + x + dx) * 3];
        CHECK(LV_MAX(pixel[0], LV_MAX(pixel[1], pixel[2])) > 8);
    }
}

static void gesture(orb_button_t button, orb_button_gesture_t kind, bool synthetic)
{
    orb_button_event_t event = {.button = button, .gesture = kind, .synthetic = synthetic, .at_ms = s_now};
    if (!synthetic && kind != ORB_BUTTON_DOWN && kind != ORB_BUTTON_UP) {
        orb_button_event_t down = event;
        down.gesture = ORB_BUTTON_DOWN;
        orb_shell_button(s_shell, &down);
        if (kind == ORB_BUTTON_SHORT || kind == ORB_BUTTON_DOUBLE) {
            down.gesture = ORB_BUTTON_UP;
            orb_shell_button(s_shell, &down);
        }
    }
    orb_shell_button(s_shell, &event);
    advance(20);
}
static void shot(const char *directory, const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    CHECK(sim_display_save_png(path));
}
int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init());
    orb_model_load_demo(); orb_home_load_demo();
    (void)orb_app_play_init();
    orb_apps_init(ORB_APP_NEEDS_IMU | ORB_APP_NEEDS_STORAGE);
    CHECK(orb_apps_register(&s_app) == ESP_OK);
    if (argc > 2) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(true);
    }
    s_shell = orb_shell_create(lv_screen_active(), action, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .brightness = 70};
    strcpy(status.clock_time, "10:09"); strcpy(status.clock_date, "Tue 27");
    orb_shell_update(s_shell, &status); advance(800);
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, true); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, true); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_shell_power_snapshot(s_shell).redact && !orb_shell_quiet(s_shell));
    shot(argv[1], "button-lock");
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); advance(800);
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_DOUBLE, true); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, true); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    /* Pending snapshots can settle only behind the retained lock surface. */
    CHECK(orb_shell_nav_bench(s_shell, "slide-left", 0, false));
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    orb_shell_nav_bench(s_shell, NULL, 0, true); advance(800);
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); advance(800);
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_HOLD, true);
    CHECK(label(orb_shell_root(s_shell), ORB_ICON_MIC " Listening"));
    shot(argv[1], "button-feedback");
    orb_voice_request_t voice; CHECK(!orb_model_take_voice_request(&voice));
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_UP, true);
    CHECK(!label(orb_shell_root(s_shell), ORB_ICON_MIC " Listening"));
    /* Actual holds queue exactly one Start and Stop, from a non-Home page. */
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_HOLD, false);
    CHECK(orb_model_take_voice_request(&voice) && voice.action == ORB_VOICE_START);
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_UP, false);
    CHECK(orb_model_take_voice_request(&voice) && voice.action == ORB_VOICE_STOP);
    CHECK(!orb_model_take_voice_request(&voice));
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_HOLD, true);
    CHECK(label(orb_shell_root(s_shell), "Sleep display")); CHECK(label(orb_shell_root(s_shell), "Restart"));
    advance(800); /* HIG materialisation settles before the final-frame gate. */
    CHECK(label(lv_display_get_layer_top(s_display), ORB_ICON_X));
    glass_control_painted("Sleep display");
    glass_control_painted("Restart");
    advance(100);
    glass_control_painted("Restart");
    shot(argv[1], "power-sheet");
    /* Real synthetic pointer dispatch cannot confirm the power-sheet action. */
    lv_indev_t *pointer = lv_indev_create(); lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, s_display); lv_indev_set_read_cb(pointer, read_pointer);
    orb_input_mark_synthetic(pointer);
    s_input.point = (lv_point_t){233,335}; s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer); advance(40); s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer); advance(40); CHECK(s_actions == 0);
    CHECK(label(orb_shell_root(s_shell), "Restart"));
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(!label(orb_shell_root(s_shell), "Restart"));
    CHECK(orb_apps_enter("button-test") == ESP_OK);
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false);
    CHECK(orb_apps_paused());
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(label(orb_shell_root(s_shell), "Resume"));
    advance(800);
    CHECK(label(lv_display_get_layer_top(s_display), ORB_ICON_X));
    glass_control_painted("Resume");
    glass_control_painted("Exit app");
    shot(argv[1], "app-pause-sheet");
    /* The shared visible close is reciprocal with BOOT: restore the same
     * app's previous pause ownership without executing a power action. */
    lv_obj_t *close = label(lv_display_get_layer_top(s_display), ORB_ICON_X); CHECK(close);
    lv_obj_send_event(lv_obj_get_parent(close), LV_EVENT_CLICKED, NULL); advance(40);
    CHECK(!orb_apps_paused());
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(!orb_apps_active());
    /* Lock pauses Marble through its own controller; waking leaves its local
     * pause sheet ready for an explicit Resume. */
    lv_obj_t *shell_screen = lv_screen_active();
    CHECK(orb_apps_enter("marble") == ESP_OK); advance(800);
    CHECK(lv_screen_active() != shell_screen);
    CHECK(!label(lv_screen_active(), "Play"));
    CHECK(!label(lv_screen_active(), "Tap to Play"));
    CHECK(!label(lv_screen_active(), "Garden"));
    shot(argv[1], "marble-immediate-playing");
    CHECK(!label(lv_screen_active(), "Resume"));
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(orb_apps_active() && !orb_apps_paused());
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(label(lv_screen_active(), "Resume"));
    CHECK(!label(lv_screen_active(), "Garden"));
    CHECK(label(lv_screen_active(), "Exit"));
    CHECK(label(lv_screen_active(), "End round"));
    CHECK(!label(lv_screen_active(), ORB_ICON_X));
    CHECK(label(lv_screen_active(), "Touch controls"));
    CHECK(!label(lv_display_get_layer_top(s_display), "Exit app"));
    shot(argv[1], "marble-pause-sheet");
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(label(lv_screen_active(), "Resume"));
    CHECK(!label(lv_display_get_layer_top(s_display), "Exit app"));
    lv_obj_t *resume = label(lv_screen_active(), "Resume");
    lv_obj_send_event(lv_obj_get_parent(resume), LV_EVENT_CLICKED, NULL); advance(800);
    CHECK(orb_apps_active() && !orb_apps_paused());
    CHECK(!label(lv_screen_active(), "Resume"));
    /* Locking from the Power sheet retires its temporary pause ownership.
     * The game's own Pause remains after a wake. */
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_HOLD, false); advance(800);
    CHECK(orb_apps_paused());
    CHECK(label(lv_display_get_layer_top(s_display), "Restart"));
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    gesture(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false); advance(800);
    CHECK(!orb_apps_paused());
    CHECK(label(lv_screen_active(), "Resume"));
    CHECK(!label(lv_screen_active(), "Garden"));
    CHECK(label(lv_screen_active(), "Exit"));
    CHECK(label(lv_screen_active(), "End round"));
    CHECK(!label(lv_screen_active(), ORB_ICON_X));
    CHECK(label(lv_screen_active(), "Touch controls"));
    CHECK(!label(lv_display_get_layer_top(s_display), "Restart"));
    CHECK(!label(lv_display_get_layer_top(s_display), "Sleep display"));
    CHECK(!label(lv_display_get_layer_top(s_display), "Exit app"));
    /* BOOT short is Back: the game's paused round handles it immediately,
     * without dismissing a hidden generic sheet or queueing voice capture. */
    gesture(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(!orb_apps_active() && lv_screen_active() == shell_screen);
    CHECK(!orb_model_take_voice_request(&voice));
    CHECK(s_actions == 0);
    orb_answer_t answer; CHECK(!orb_model_peek_answer(&answer));
    orb_shell_destroy(s_shell);
    printf("button shell: %u checks passed\n", s_checks);
    return 0;
}
