/* System QA: real retained shell, private services, deterministic physical and
 * synthetic pointer/button input. No storage, network, or board access. */
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
#include "orb_time.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks, s_actions[8];
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static const char *s_out;
#define CHECK(v) do { ++s_checks; if (!(v)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); exit(1); } } while (0)
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *out) { (void)input; *out = s_input; }
static void action(orb_shell_action_t value, void *ctx) { (void)ctx; ++s_actions[value]; }
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) {
        s_now += 20; orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}
static void pointer(int x, int y, bool down)
{
    s_input.point = (lv_point_t){x, y}; s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer); advance(40);
}
static lv_obj_t *text(lv_obj_t *root, const char *value)
{
    if (!lv_obj_is_visible(root)) return NULL;
    const char *rim = orb_kit_rim_text_get(root);
    if ((rim && !strcasecmp(rim, value)) || (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), value))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = text(lv_obj_get_child(root, i), value); if (found) return found;
    }
    return NULL;
}
static void shot(const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", s_out, name); CHECK(sim_display_save_png(path));
}
static void button(orb_button_t b, orb_button_gesture_t g, bool synthetic)
{
    orb_shell_button(s_shell, &(orb_button_event_t){.button=b, .gesture=g, .synthetic=synthetic, .at_ms=s_now}); advance(40);
}
static void click(lv_obj_t *obj)
{
    lv_area_t area; lv_obj_get_coords(obj, &area);
    int x = (area.x1 + area.x2) / 2, y = (area.y1 + area.y2) / 2;
    pointer(x, y, true); pointer(x, y, false); advance(800);
}
static lv_obj_t *reader(void)
{
    lv_obj_t *copy = text(orb_shell_root(s_shell), "Current software is safe. Reconnect your Mac and try again.");
    CHECK(copy); return lv_obj_get_parent(copy);
}
static void update_reader(orb_shell_snapshot_t *state)
{
    state->ota = ORB_SHELL_OTA_FAILED;
    snprintf(state->error, sizeof(state->error), "Current software is safe. Reconnect your Mac and try again.");
    orb_shell_update(s_shell, state); advance(800);
    lv_obj_t *scroll = reader(); lv_obj_update_layout(scroll);
    CHECK(lv_obj_get_scroll_bottom(scroll) > 0);
    int before = lv_obj_get_scroll_y(scroll);
    /* Clockwise arc along the real outer band, lifted after multiple moves. */
    for (unsigned i = 0; i <= 8; ++i) {
        float angle = (-25.f + 8.f * i) * 3.14159265f / 180.f;
        pointer((int)lroundf(233 + 216 * cosf(angle)), (int)lroundf(233 + 216 * sinf(angle)), true);
    }
    pointer(396, 375, false); advance(800);
    CHECK(lv_obj_get_scroll_y(scroll) > before);
    shot("updates-rim-reader");
    orb_shell_power_enable(s_shell, true);
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); advance(800);
    orb_shell_power(s_shell); advance(800);
    before = lv_obj_get_scroll_y(scroll);
    CHECK(!orb_shell_twist(s_shell, -2));
    CHECK(lv_obj_get_scroll_y(scroll) == before);
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); advance(800);
    /* A centre swipe remains content; the same inward left edge is Back. */
    pointer(160, 233, true); pointer(235, 233, true); pointer(320, 233, true); pointer(320, 233, false); advance(800);
    CHECK(text(orb_shell_root(s_shell), "Try again"));
    pointer(20, 233, true); pointer(100, 233, true); pointer(200, 233, true); pointer(300, 233, false); advance(800);
    CHECK(!text(orb_shell_root(s_shell), "Try again"));
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    state->ota = ORB_SHELL_OTA_IDLE; orb_shell_update(s_shell, state); advance(800);
}
static void power_sheet(void)
{
    button(ORB_BUTTON_PWR, ORB_BUTTON_HOLD, true); advance(800);
    lv_obj_t *top = lv_display_get_layer_top(s_display);
    CHECK(text(top, "Power")); CHECK(orb_kit_rim_text_get(text(top, "Power")));
    lv_obj_t *sleep = text(top, "Sleep display"); CHECK(sleep);
    orb_input_mark_synthetic(s_pointer); click(lv_obj_get_parent(sleep));
    CHECK(s_actions[ORB_SHELL_ACTION_SLEEP] == 0); CHECK(text(top, "Restart"));
    lv_indev_set_user_data(s_pointer, NULL);
    /* Scrolling on a power decision cannot turn into a click on return. */
    lv_area_t area; lv_obj_get_coords(lv_obj_get_parent(sleep), &area);
    int x=(area.x1+area.x2)/2, y=(area.y1+area.y2)/2;
    pointer(x,y,true); pointer(x+30,y,true); pointer(x,y,true); pointer(x,y,false); advance(800);
    CHECK(s_actions[ORB_SHELL_ACTION_SLEEP] == 0); CHECK(text(top, "Restart"));
    shot("power-sheet");
    click(lv_obj_get_parent(text(top, ORB_ICON_X)));
    CHECK(!text(top, "Restart"));
    button(ORB_BUTTON_PWR, ORB_BUTTON_HOLD, true); advance(800);
    pointer(20,233,true); pointer(100,233,true); pointer(200,233,true); pointer(300,233,false); advance(800);
    CHECK(!text(top, "Restart"));
}
static esp_err_t enter(void) { return ESP_OK; }
static void exit_app(void) {}
static void pause_app(bool paused) { (void)paused; }
static void tick_app(const orb_app_input_t *input) { (void)input; }
static void app_lock(void)
{
    static const orb_app_entry_t entry = {.enter=enter, .exit=exit_app, .pause=pause_app, .tick=tick_app};
    static const orb_app_manifest_t app = {.id="qa-private", .name="Private test", .icon=ORB_ICON_PLAY, .entry=&entry};
    orb_apps_init(0); CHECK(orb_apps_register(&app) == ESP_OK); CHECK(orb_apps_enter(app.id) == ESP_OK);
    orb_shell_power_enable(s_shell, true); CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); advance(800);
    button(ORB_BUTTON_PWR, ORB_BUTTON_DOWN, false); button(ORB_BUTTON_PWR, ORB_BUTTON_UP, false); button(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false);
    advance(800); orb_apps_poll((int64_t)s_now * 1000, false, false, 0, 0, 1, false);
    CHECK(orb_apps_paused()); CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(!text(lv_display_get_layer_top(s_display), "Resume"));
    CHECK(text(lv_display_get_layer_top(s_display), ORB_ICON_LOCK)); shot("app-locked");
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); advance(800);
    lv_obj_t *resume = text(lv_display_get_layer_top(s_display), "Resume"); CHECK(resume);
    click(lv_obj_get_parent(resume));
    orb_apps_poll((int64_t)s_now * 1000, false, true, 0, 0, 1, false);
    CHECK(!orb_apps_paused()); orb_apps_exit();
}
static void system_frame(orb_shell_snapshot_t *state, const char *title, const char *name)
{
    orb_shell_update(s_shell, state); advance(800);
    lv_obj_t *heading = text(orb_shell_root(s_shell), title); CHECK(heading);
    CHECK(orb_kit_rim_text_get(heading)); shot(name);
}
static unsigned visible_arcs(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return 0;
    unsigned count = lv_obj_check_type(root, &lv_arc_class) ? 1 : 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) count += visible_arcs(lv_obj_get_child(root, i));
    return count;
}
static void setup(orb_shell_snapshot_t *state)
{
    state->net = ORB_SHELL_NET_STARTING; state->link = ORB_SHELL_LINK_OFFLINE;
    system_frame(state, "Starting Orb", "starting");
    state->net = ORB_SHELL_NET_PROVISIONING;
    snprintf(state->service_name, sizeof(state->service_name), "Orb-QA");
    snprintf(state->pop, sizeof(state->pop), "12345678");
    system_frame(state, "Set up Wi-Fi", "setup");
    CHECK(text(orb_shell_root(s_shell), "1234 5678"));
    snprintf(state->pop, sizeof(state->pop), "invalid");
    system_frame(state, "Set up Wi-Fi", "setup-preparing");
    CHECK(text(orb_shell_root(s_shell), "Preparing setup code"));
    CHECK(!text(orb_shell_root(s_shell), "1234 5678"));
    snprintf(state->pop, sizeof(state->pop), "12345678");
    state->net = ORB_SHELL_NET_CONNECTING;
    system_frame(state, "Connecting to Wi-Fi", "setup-connecting");
    state->net = ORB_SHELL_NET_FAILED;
    system_frame(state, "Try Wi-Fi again", "setup-failed");
    state->net = ORB_SHELL_NET_CONNECTED; state->link = ORB_SHELL_LINK_PAIRING;
    snprintf(state->pairing_code, sizeof(state->pairing_code), "123456");
    snprintf(state->hub_name, sizeof(state->hub_name), "A very long Mac name that remains fully readable after scrolling");
    system_frame(state, "Pair with your Mac", "pairing-long");
    CHECK(text(orb_shell_root(s_shell), "123 456"));
    char identity[96]; snprintf(identity, sizeof(identity), "Found %s", state->hub_name);
    /* The reader rests on whole lines (polish review #1), so at AX3 the name
     * may start just below its fade; the rim scrolls it fully into view. */
    orb_shell_twist(s_shell, 20); advance(800); shot("pairing-long-end");
    lv_obj_t *found = text(orb_shell_root(s_shell), identity); CHECK(found);
    CHECK(lv_obj_is_scrollable(lv_obj_get_parent(found)));
    state->link = ORB_SHELL_LINK_ONLINE;
    snprintf(state->service_error, sizeof(state->service_error), "A service needs attention. Restart Orb and check your Mac.");
    system_frame(state, "Needs attention", "service-error");
    CHECK(visible_arcs(orb_shell_root(s_shell)) == 0);
    CHECK(!text(orb_shell_root(s_shell), ORB_ICON_GEAR));
    orb_shell_back(s_shell); advance(800); CHECK(text(orb_shell_root(s_shell), "Needs attention"));
    state->service_error[0] = 0; state->identify = true;
    snprintf(state->device_name, sizeof(state->device_name), "A very long device display name");
    snprintf(state->device_id, sizeof(state->device_id), "orb-0123456789abcdef");
    system_frame(state, "This is your Orb", "identify");
    state->identify = false; orb_shell_update(s_shell, state); advance(800);
}
int main(int argc, char **argv)
{
    CHECK(argc == 3); s_out=argv[1];
    lv_init(); lv_tick_set_cb(tick); s_display=sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init()); orb_time_init();
    orb_model_set_connection(true,true);
    if (strcmp(argv[2], "default")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    s_pointer=lv_indev_create(); lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer,s_display); lv_indev_set_read_cb(s_pointer,read_pointer);
    s_shell=orb_shell_create(lv_screen_active(),action,NULL); CHECK(s_shell);
    orb_shell_snapshot_t state={.net=ORB_SHELL_NET_CONNECTED,.link=ORB_SHELL_LINK_ONLINE,.battery_pct=80,.brightness=70};
    snprintf(state.clock_time,sizeof(state.clock_time),"10:09"); snprintf(state.clock_date,sizeof(state.clock_date),"Wednesday 28");
    snprintf(state.firmware_version,sizeof(state.firmware_version),"0.5.2");
    orb_shell_update(s_shell,&state); advance(800);
    const char *only = getenv("ORB_QA_CASE");
    if (!only || !strcmp(only, "updates")) update_reader(&state);
    if (!only || !strcmp(only, "power")) power_sheet();
    if (!only || !strcmp(only, "lock")) app_lock();
    if (!only || !strcmp(only, "setup")) setup(&state);
    orb_shell_destroy(s_shell); lv_indev_delete(s_pointer); lv_deinit();
    printf("QA shell %s: %u checks passed\n", argv[2],s_checks); return 0;
}
