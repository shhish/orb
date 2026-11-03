/* System layers must suspend the exact editor, including uncommitted gallery
 * browsing and the slot the crown will edit on return. Inputs use real LVGL
 * pointer samples and the production button/twist entry points. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "orb_faces.h"
#include "orb_home.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_memory.h"
#include "orb_shell.h"
#include "orb_shell_power.h"
#include "orb_time.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static orb_shell_snapshot_t s_status;
static const char *s_stage = "setup", *s_output;
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "system Face context %s:%d at %u: %s\n", s_stage, __LINE__, s_now, #value); \
    exit(EXIT_FAILURE); } } while (0)

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *pointer, lv_indev_data_t *data) { (void)pointer; *data = s_input; }
static void advance(unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 16) {
        s_now += 16;
        lv_indev_read(s_pointer);
        orb_shell_poll(s_shell);
        lv_timer_handler();
    }
    lv_refr_now(s_display);
}
static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (!lv_obj_is_visible(root)) return NULL;
    const char *rim = orb_kit_rim_text_get(root), *button = orb_kit_button_get_text(root);
    if ((rim && !strcasecmp(rim, text)) || (button && !strcmp(button, text)) ||
        (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
static void point(int x, int y, bool down)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer); advance(16);
}
static void tap(int x, int y)
{
    point(x, y, true); advance(64); point(x, y, false); advance(500);
}
static void tap_text(const char *text)
{
    lv_obj_t *object = find(orb_shell_root(s_shell), text); CHECK(object);
    while (object && !lv_obj_is_clickable(object)) object = lv_obj_get_parent(object);
    CHECK(object);
    lv_area_t a; lv_obj_get_coords(object, &a);
    tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
}
static void drag(int x, int y, int dx, int dy)
{
    point(x, y, true);
    for (int i = 1; i <= 20; i++) point(x + dx * i / 20, y + dy * i / 20, true);
    point(x + dx, y + dy, false); advance(1200);
}
static void rim(void)
{
    point(449, 233, true);
    for (unsigned i = 1; i <= 18; i++) {
        double angle = i * M_PI / 36.;
        point(233 + (int)lround(216 * cos(angle)), 233 + (int)lround(216 * sin(angle)), true);
    }
    point(s_input.point.x, s_input.point.y, false); advance(700);
}

static void button(orb_button_t button, orb_button_gesture_t gesture)
{
    orb_shell_button(s_shell, &(orb_button_event_t){.button=button, .gesture=gesture, .at_ms=s_now});
    advance(1200);
}
static void boot(void) { button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT); }
static void place(orb_shell_surface_t surface)
{
    CHECK(orb_shell_surface(s_shell) == surface);
    CHECK(orb_shell_nav_current(s_shell) == surface);
}
static void shot(const char *name)
{
    if (!s_output) return;
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/%s.png", s_output, name) < sizeof(path));
    CHECK(sim_display_save_png(path));
}
static void open_gallery(void)
{
    point(233, 218, true); advance(480); point(233, 218, false); advance(700);
    CHECK(find(orb_shell_root(s_shell), "Edit"));
}
static void round_trip(unsigned layer, const char *caption)
{
    if (layer == 0) button(ORB_BUTTON_PWR, ORB_BUTTON_DOUBLE);
    else if (layer == 1) drag(233, 12, 0, 220);
    else drag(233, 454, 0, -220);
    place(layer == 0 ? ORB_SHELL_CONTROL_CENTER : layer == 1 ? ORB_SHELL_NOTIFICATIONS : ORB_SHELL_STACK);
    CHECK(!find(orb_shell_root(s_shell), caption));
    boot(); place(ORB_SHELL_FACE);
    CHECK(find(orb_shell_root(s_shell), caption));
}
static void clocks(void)
{
    s_stage = "retained Home summary during an outage";
    orb_faces_snapshot_t known, stale; CHECK(orb_shell_face_snapshot(s_shell, &known));
    CHECK(known.home_total && !known.home_stale);
    orb_home_set_connection(false, false); advance(200);
    CHECK(orb_shell_face_snapshot(s_shell, &stale));
    CHECK(stale.home_stale && stale.home_total == known.home_total && stale.home_on == known.home_on);
    orb_home_set_connection(true, true); advance(200);
    s_stage = "owner clock hour cycle and civil-hour scene selection";
    static const struct { const char *source, *twelve, *scene; } cases[] = {
        {"00:04", "12:04 AM", "Good morning"}, {"12:04", "12:04 PM", "I'm home"},
        {"17:28", "5:28 PM", "Evening"}, {"22:09", "10:09 PM", "Good Night"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(s_status.clock_time, sizeof(s_status.clock_time), "%s", cases[i].source);
        orb_shell_update(s_shell, &s_status); advance(200);
        orb_time_set_clock_format(false);
        orb_faces_snapshot_t snapshot; CHECK(orb_shell_face_snapshot(s_shell, &snapshot));
        /* The Face shows the 12-hour digits without AM/PM, like a watch face. */
        size_t digits = strcspn(cases[i].twelve, " ");
        CHECK(strlen(snapshot.clock) == digits && !strncmp(snapshot.clock, cases[i].twelve, digits));
        snprintf(s_status.clock_time, sizeof(s_status.clock_time), "%s", cases[i].twelve);
        orb_shell_update(s_shell, &s_status); advance(200);
        CHECK(orb_shell_face_snapshot(s_shell, &snapshot));
        CHECK(strlen(snapshot.clock) == digits && !strncmp(snapshot.clock, cases[i].twelve, digits));
        orb_time_set_clock_format(true);
        CHECK(orb_shell_face_snapshot(s_shell, &snapshot));
        CHECK(!strcmp(snapshot.clock, cases[i].source));
        orb_shell_navigate(s_shell, ORB_SHELL_CONTROL_CENTER); advance(1200);
        if (!find(orb_shell_root(s_shell), "Home")) drag(233, 370, 0, -180);
        place(ORB_SHELL_CONTROL_CENTER);
        CHECK(find(orb_shell_root(s_shell), cases[i].scene));
        boot(); place(ORB_SHELL_FACE);
    }
    orb_time_set_clock_format(false); advance(1200); shot("face-owner-12hour");
}
static void contexts(void)
{
    s_stage = "gallery browsing rollback across all system layers";
    open_gallery();
    CHECK(orb_shell_twist(s_shell, 1)); advance(500);
    CHECK(find(orb_shell_root(s_shell), "Meridian"));
    for (unsigned layer = 0; layer < 3; layer++) round_trip(layer, "Edit");
    CHECK(find(orb_shell_root(s_shell), "Meridian"));
    shot("gallery-after-layers");
    boot(); place(ORB_SHELL_FACE);
    /* Browsing was never a committed face selection. */
    CHECK(!orb_shell_twist(s_shell, 1));
    open_gallery(); CHECK(find(orb_shell_root(s_shell), "Orb"));
    s_stage = "colour and selected slot survive layers without reset";
    tap_text("Edit"); CHECK(find(orb_shell_root(s_shell), "Color"));
    CHECK(orb_shell_twist(s_shell, 1)); advance(500);
    CHECK(find(orb_shell_root(s_shell), "Orange"));
    for (unsigned layer = 0; layer < 3; layer++) round_trip(layer, "Color");
    CHECK(find(orb_shell_root(s_shell), "Orange"));
    rim(); place(ORB_SHELL_FACE);
    CHECK(find(orb_shell_root(s_shell), "Color"));
    CHECK(!find(orb_shell_root(s_shell), "Orange"));
    tap_text("Slots"); CHECK(find(orb_shell_root(s_shell), "Complications"));
    /* Orb's third edit handle is Battery. It must stay the selected slot so
     * the next crown step changes Battery to Weather, rather than slot zero. */
    tap(338, 316); CHECK(find(orb_shell_root(s_shell), "Battery"));
    for (unsigned layer = 0; layer < 3; layer++) round_trip(layer, "Complications");
    CHECK(find(orb_shell_root(s_shell), "Battery"));
    CHECK(orb_shell_twist(s_shell, 1)); advance(500);
    CHECK(find(orb_shell_root(s_shell), "Weather"));
    shot("slots-after-layers");
    /* A connection change and notification arrival update data in place;
     * neither changes the selected editor or submits any action. */
    orb_model_set_connection(false, false); advance(700);
    CHECK(find(orb_shell_root(s_shell), "Complications"));
    orb_card_t notice = {.id="system-editor-interruption", .kind=ORB_CARD_ATTENTION, .title="Build complete"};
    CHECK(orb_model_upsert_card(&notice)); advance(700);
    CHECK(find(orb_shell_root(s_shell), "Complications"));
    orb_model_set_connection(true, true); advance(700);
    CHECK(find(orb_shell_root(s_shell), "Weather"));
    orb_answer_t answer; CHECK(!orb_model_peek_answer(&answer));
    boot(); CHECK(find(orb_shell_root(s_shell), "Color"));
    boot(); CHECK(find(orb_shell_root(s_shell), "Edit"));
    boot(); CHECK(!orb_shell_twist(s_shell, 1));
}
int main(int argc, char **argv)
{
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init()); orb_time_init();
    if (argc > 1 && !strcmp(argv[1], "ax3-bold-rt")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    s_output = argc > 2 ? argv[2] : NULL;
    orb_model_load_demo(); orb_home_load_demo(); orb_model_set_connection(true, true);
    s_pointer = lv_indev_create(); CHECK(s_pointer); lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_pointer, read_pointer); lv_indev_set_mode(s_pointer, LV_INDEV_MODE_EVENT);
    lv_indev_set_long_press_time(s_pointer, 400);
    orb_memory_stats_t before, after; orb_memory_get(ORB_MEMORY_WORKSPACE, &before);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    s_status = (orb_shell_snapshot_t){.net=ORB_SHELL_NET_CONNECTED, .link=ORB_SHELL_LINK_ONLINE,
        .battery_pct=80, .brightness=70, .clock_time="17:28", .clock_date="Wednesday, October 28"};
    orb_shell_update(s_shell, &s_status); advance(1200);
    if (argc < 4 || strcmp(argv[3], "contexts")) clocks();
    if (argc < 4 || strcmp(argv[3], "clocks")) contexts();
    s_stage = "destroy while a layer retains the editor";
    open_gallery(); button(ORB_BUTTON_PWR, ORB_BUTTON_DOUBLE); place(ORB_SHELL_CONTROL_CENTER);
    orb_shell_destroy(s_shell); s_shell = NULL;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &after); CHECK(after.used == before.used);
    lv_indev_delete(s_pointer);
    printf("system Face context: %u checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
