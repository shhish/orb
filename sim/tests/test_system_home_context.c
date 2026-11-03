/* System Home controls keep their labels, action identity and availability in
 * sync with authoritative snapshots. Pointer contacts never leave this process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "orb_home.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_time.h"
#include "../../firmware/components/orb_shell/orb_shell_power.h"
#include "sim_display.h"
#include "src/misc/lv_text_private.h"

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static orb_shell_snapshot_t s_status;
#define CHECK(v) do { ++s_checks; if (!(v)) { \
    fprintf(stderr, "system Home context:%d at %u: %s\n", __LINE__, s_now, #v); exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void pointer_read(lv_indev_t *pointer, lv_indev_data_t *out) { (void)pointer; *out = s_input; }
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) {
        s_now += 20; orb_shell_poll(s_shell); lv_timer_handler();
    }
    lv_refr_now(s_display);
}
static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (lv_obj_is_hidden(root)) return NULL;
    const char *rim = orb_kit_rim_text_get(root), *button = orb_kit_button_get_text(root);
    if ((rim && !strcasecmp(rim, text)) || (button && !strcmp(button, text)) ||
        (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text); if (found) return found;
    }
    return NULL;
}
static lv_obj_t *find_name(lv_obj_t *root, int center)
{
    if (lv_obj_is_hidden(root)) return NULL;
    lv_area_t area; lv_obj_get_coords(root, &area);
    if (lv_obj_check_type(root, &lv_label_class) && area.y1 == 270 &&
        abs((area.x1 + area.x2) / 2 - center) <= 1) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_name(lv_obj_get_child(root, i), center); if (found) return found;
    }
    return NULL;
}
static lv_obj_t *find_suffix(lv_obj_t *root, const char *suffix)
{
    if (lv_obj_is_hidden(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *text = lv_label_get_text(root);
        size_t length = strlen(text), tail = strlen(suffix);
        if (length >= tail && !strcmp(text + length - tail, suffix)) return root;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_suffix(lv_obj_get_child(root, i), suffix);
        if (found) return found;
    }
    return NULL;
}
static void suffix_visible(const char *suffix)
{
    lv_obj_t *name = find_suffix(orb_shell_root(s_shell), suffix); CHECK(name);
    lv_text_attributes_t attributes = {.letter_space=lv_obj_get_style_text_letter_space(name, 0),
        .max_width=LV_COORD_MAX};
    lv_point_t ink;
    lv_text_get_size_attributes(&ink, lv_label_get_text(name),
        lv_obj_get_style_text_font(name, 0), &attributes);
    CHECK(ink.x <= lv_obj_get_width(name));
}
static lv_obj_t *find_disc(lv_obj_t *root, int center)
{
    lv_area_t area; lv_obj_get_coords(root, &area);
    if (lv_obj_is_clickable(root) && lv_obj_get_width(root) == ORB_KIT_DISC &&
        lv_obj_get_height(root) == ORB_KIT_DISC && abs((area.x1 + area.x2) / 2 - center) <= 1) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_disc(lv_obj_get_child(root, i), center); if (found) return found;
    }
    return NULL;
}
/* Control Center's Home names sit under their discs on two lines, with at
 * least 20px between the two names (polish review #6); a name that still
 * overflows keeps its distinguishing suffix through the kit's middle elision. */
static void name_below_disc(int center)
{
    lv_obj_t *name = find_name(orb_shell_root(s_shell), center);
    lv_obj_t *disc = find_disc(orb_shell_root(s_shell), center); CHECK(name && disc);
    lv_area_t text, control; lv_obj_get_coords(name, &text); lv_obj_get_coords(disc, &control);
    CHECK(text.y1 >= control.y2 + 8);
    CHECK(lv_obj_get_width(name) <= 140);
}
static void route(orb_shell_surface_t surface)
{
    orb_shell_navigate(s_shell, surface); advance(1200);
    CHECK(orb_shell_surface(s_shell) == surface);
}
static void tap(const char *text)
{
    lv_obj_t *target = find(orb_shell_root(s_shell), text);
    if (!target) fprintf(stderr, "missing tap: %s\n", text);
    CHECK(target);
    while (target && !lv_obj_is_clickable(target)) target = lv_obj_get_parent(target);
    CHECK(target);
    lv_obj_scroll_to_view(target, LV_ANIM_OFF); advance(80);
    lv_area_t area; lv_obj_get_coords(target, &area);
    s_input.point = (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); advance(40);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(1200);
}
static void control_home(void)
{
    route(ORB_SHELL_CONTROL_CENTER);
    s_input = (lv_indev_data_t){.point={233,394}, .state=LV_INDEV_STATE_PRESSED};
    lv_indev_read(s_pointer); advance(40);
    for (unsigned i = 1; i <= 7; i++) {
        s_input.point.y = 394 - (int)i * 22; lv_indev_read(s_pointer); advance(40);
    }
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "Good morning"));
}
static void capture(const char *directory, const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    CHECK(sim_display_save_png(path));
}
static void contact(int x, int y, bool pressed)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer); advance(40);
}
static void interrupted_unlock(const char *directory)
{
    orb_home_room_t room = {.id="qa-entry", .name="Entry"};
    orb_home_accessory_t lock = {.id="qa-lock", .name="QA Lock", .room="qa-entry",
        .kind=ORB_HOME_LOCK, .reachable=true, .writable=ORB_HOME_LOCKED,
        .state={.present=ORB_HOME_LOCKED, .locked=true}};
    CHECK(orb_home_snapshot_chunk(&room, 1, &lock, 1, NULL, 0, true, false));
    orb_shell_power_enable(s_shell, true);
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); advance(500);
    route(ORB_SHELL_HOME);
    contact(233, 233, false);
    tap("Entry");
    CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
    capture(directory, "unlock-before-open");
    tap("QA Lock"); CHECK(find(orb_shell_root(s_shell), "Hold to unlock"));
    for (unsigned alarm = 0; alarm < 2; alarm++) {
        contact(233, 140, true); advance(440);
        CHECK(find(orb_shell_root(s_shell), "Keep holding"));
        if (alarm) {
            CHECK(orb_time_timer_set(1)); CHECK(orb_time_timer_start(orb_time_now_us()));
            orb_time_poll(orb_time_now_us() + 1000001, time(NULL));
            orb_shell_poll(s_shell);
        } else {
            orb_shell_button(s_shell, &(orb_button_event_t){.button=ORB_BUTTON_PWR,
                .gesture=ORB_BUTTON_SHORT, .at_ms=s_now});
        }
        advance(1200);
        CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
        if (alarm) CHECK(orb_shell_time_alert_active(s_shell));
        else CHECK(orb_shell_power_snapshot(s_shell).redact);
        capture(directory, alarm ? "unlock-alarm-cancelled" : "unlock-lock-cancelled");
        contact(233, 140, false);
        if (alarm) {
            orb_shell_button(s_shell, &(orb_button_event_t){.button=ORB_BUTTON_BOOT,
                .gesture=ORB_BUTTON_SHORT, .at_ms=s_now});
            orb_time_init();
        } else CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted);
        advance(1200);
        CHECK(find(orb_shell_root(s_shell), "Hold to unlock"));
        CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
    }
}
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init());
    orb_model_set_connection(true, true);
    if (!strcmp(argv[1], "ax3-bold-rt")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    s_pointer = lv_indev_create(); lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_pointer, pointer_read); lv_indev_set_mode(s_pointer, LV_INDEV_MODE_EVENT);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    s_status = (orb_shell_snapshot_t){.net=ORB_SHELL_NET_CONNECTED, .link=ORB_SHELL_LINK_ONLINE,
        .battery_pct=80, .brightness=70};
    strcpy(s_status.clock_time, "10:09"); orb_shell_update(s_shell, &s_status); advance(1200);
    orb_home_room_t rooms[] = {{.id="room-a", .name="Guest Suite Bath ceiling suite"},
        {.id="room-b", .name="Office"}};
    orb_home_accessory_t accessories[] = {
        {.id="home-context-a", .name="Guest Suite Bath ceiling lights", .room="room-a",
            .kind=ORB_HOME_LIGHT, .reachable=true, .favourite=true, .writable=ORB_HOME_ON,
            .state={.present=ORB_HOME_ON, .on=true}},
        {.id="home-context-b", .name="Kitchen very long named outlet", .room="room-b",
            .kind=ORB_HOME_OUTLET, .reachable=true, .favourite=true, .writable=ORB_HOME_ON,
            .state={.present=ORB_HOME_ON, .on=false}},
    };
    orb_home_scene_t scenes[] = {{.id="scene-am", .name="Good morning", .favourite=true},
        {.id="scene-pm", .name="Evening", .favourite=true}};
    orb_home_set_connection(true, true);
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 2, true, false)); advance(100);
    /* Face counts include known lighting/power state, never climate/security.
     * Link and provider freshness are independent from ordinary shell online. */
    orb_faces_snapshot_t face;
    CHECK(orb_shell_face_snapshot(s_shell, &face));
    CHECK(face.home_on == 1 && face.home_total == 2);
    accessories[1].kind = ORB_HOME_FAN;
    accessories[1].state.on = true;
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 2, true, false)); advance(100);
    CHECK(orb_shell_face_snapshot(s_shell, &face));
    CHECK(face.home_on == 1 && face.home_total == 1);
    accessories[1].kind = ORB_HOME_LOCK;
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 2, true, false)); advance(100);
    CHECK(orb_shell_face_snapshot(s_shell, &face));
    CHECK(face.home_on == 1 && face.home_total == 1);
    orb_home_set_connection(false, false); advance(100);
    CHECK(orb_shell_face_snapshot(s_shell, &face));
    CHECK(face.home_on == 1 && face.home_total == 1 && face.home_stale);
    orb_home_set_connection(true, true);
    accessories[0].reachable = accessories[1].reachable = false;
    CHECK(orb_home_snapshot_chunk_available(rooms, 2, accessories, 2, scenes, 2, false, true, false)); advance(100);
    CHECK(orb_shell_face_snapshot(s_shell, &face));
    CHECK(face.home_on == 1 && face.home_total == 1 && face.home_stale);
    /* An unreachable retained object still has its last reading. Missing ON
     * fields, rather than reachability, distinguish an unreceived summary. */
    accessories[0].state.present = accessories[1].state.present = 0;
    CHECK(orb_home_snapshot_chunk_available(rooms, 2, accessories, 2, scenes, 2, false, true, false)); advance(100);
    CHECK(orb_shell_face_snapshot(s_shell, &face));
    CHECK(face.home_on == 0 && face.home_total == 0 && face.home_stale);
    accessories[0].state.present = accessories[1].state.present = ORB_HOME_ON;
    accessories[1].kind = ORB_HOME_OUTLET;
    accessories[1].state.on = false;
    accessories[0].reachable = accessories[1].reachable = true;
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 2, true, false)); advance(100);
    control_home(); capture(argv[2], "control-home-long");
    name_below_disc(153); name_below_disc(313);

    /* A quick toggle must retain the differing suffix before activation. */
    strcpy(accessories[0].name, "Guest Suite Bath ceiling East");
    strcpy(accessories[1].name, "Guest Suite Bath ceiling West");
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 2, true, false)); advance(1200);
    suffix_visible("East"); suffix_visible("West");
    capture(argv[2], "control-home-shared-prefix");

    strcpy(accessories[0].name, "New lamp");
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 2, true, false)); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "New lamp"));
    accessories[1].state.present = 0;
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 2, true, false)); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "—"));
    CHECK(!find(orb_shell_root(s_shell), "Off"));

    /* Current-model reconciliation can precede the deferred Home rebuild.
     * A held old scene refuses the new suggestion; favourites deliberately
     * retain the original current-ordinal action policy in that same gap. */
    contact(233, 130, true);
    strcpy(s_status.clock_time, "6:00 PM"); orb_shell_update(s_shell, &s_status);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer);
    CHECK(!orb_home_take_request(&(orb_home_request_t){0}));
    advance(1200);
    CHECK(find(orb_shell_root(s_shell), "Evening"));
    contact(153, 220, true);
    orb_home_accessory_t first_favourite = accessories[0];
    accessories[0] = accessories[1]; accessories[1] = first_favourite;
    accessories[0].state.present = ORB_HOME_ON;
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 2, true, false));
    orb_shell_poll(s_shell);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer);
    orb_home_request_t ordinal_request; CHECK(orb_home_take_request(&ordinal_request));
    CHECK(ordinal_request.kind == ORB_HOME_REQUEST_TOGGLE && !strcmp(ordinal_request.id, accessories[0].id));
    orb_home_complete(&ordinal_request, NULL, NULL);
    first_favourite = accessories[0]; accessories[0] = accessories[1]; accessories[1] = first_favourite;
    accessories[1].state.present = 0;
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 2, true, false));
    advance(1200);

    /* The explicit scene tap must name exactly the scene being invoked, even
     * after the current local hour selects another suggested scene. */
    strcpy(s_status.clock_time, "6:00 PM"); orb_shell_update(s_shell, &s_status); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "Evening"));
    CHECK(!find(orb_shell_root(s_shell), "Good morning"));
    capture(argv[2], "control-home-updated"); tap("Evening");
    orb_home_request_t request; CHECK(orb_home_take_request(&request));
    CHECK(request.kind == ORB_HOME_REQUEST_SCENE && !strcmp(request.id, "scene-pm"));
    orb_home_complete(&request, NULL, NULL); advance(1200);
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 1, true, false)); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "Open Home"));
    CHECK(!find(orb_shell_root(s_shell), "Evening"));

    /* A room-scoped All off must become the whole-home Open Home when a
     * second lit room changes the card's meaning, and recover after failure. */
    route(ORB_SHELL_STACK); CHECK(find(orb_shell_root(s_shell), "All off"));
    capture(argv[2], "stack-home-long");
    lv_obj_t *room_name = find(orb_shell_root(s_shell), rooms[0].name); CHECK(room_name);
    lv_obj_t *reader = lv_obj_get_parent(room_name);
    CHECK(lv_obj_is_scrollable(reader));
    CHECK(lv_obj_get_scroll_bottom(reader) > 0);
    /* The reader rests on whole lines (polish-text #2): drag from its own
     * centre rather than from the old fixed 56 px lane's lower edge. */
    lv_area_t reader_area; lv_obj_get_coords(reader, &reader_area);
    int32_t start_y = (reader_area.y1 + reader_area.y2) / 2;
    s_input = (lv_indev_data_t){.point={233,start_y}, .state=LV_INDEV_STATE_PRESSED};
    lv_indev_read(s_pointer); advance(40);
    for (unsigned i = 1; i <= 6; i++) {
        s_input.point.y = start_y - (int)i * 5; lv_indev_read(s_pointer); advance(40);
    }
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(1200);
    CHECK(lv_obj_get_scroll_y(reader) > 0);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    accessories[1].kind = ORB_HOME_LIGHT;
    accessories[1].state = (orb_home_state_t){.present=ORB_HOME_ON, .on=true};
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 1, true, false)); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "2 lights on"));
    CHECK(find(orb_shell_root(s_shell), "Open Home"));
    CHECK(!find(orb_shell_root(s_shell), "All off"));
    capture(argv[2], "stack-home-multiple-rooms");
    accessories[1].state.on = false;
    CHECK(orb_home_snapshot_chunk(rooms, 2, accessories, 2, scenes, 1, true, false)); advance(1200);
    tap("All off"); CHECK(orb_home_take_request(&request));
    capture(argv[2], "stack-home-pending");
    lv_obj_t *off = find(orb_shell_root(s_shell), "All off"); CHECK(off);
    CHECK(lv_obj_has_state(off, LV_STATE_DISABLED));
    orb_home_complete(&request, NULL, "Change failed"); advance(1200);
    off = find(orb_shell_root(s_shell), "All off"); CHECK(off);
    CHECK(!lv_obj_has_state(off, LV_STATE_DISABLED));
    capture(argv[2], "stack-home-recovered");

    /* A layer has its own selected card. It must not replace the notification
     * being read when BOOT restores the interrupted detail. */
    orb_card_t first = {.id="home-context-notice-a", .kind=ORB_CARD_ATTENTION,
        .title="Notice A", .body="Body A"};
    orb_card_t second = {.id="home-context-notice-b", .kind=ORB_CARD_ATTENTION,
        .title="Notice B", .body="Body B"};
    CHECK(orb_model_upsert_card(&first)); CHECK(orb_model_upsert_card(&second)); advance(1200);
    route(ORB_SHELL_NOTIFICATIONS); tap("Notice B");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATION_DETAIL);
    CHECK(find(orb_shell_root(s_shell), "Notice B"));
    route(ORB_SHELL_STACK);
    orb_shell_button(s_shell, &(orb_button_event_t){.button=ORB_BUTTON_BOOT, .gesture=ORB_BUTTON_SHORT});
    advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATION_DETAIL);
    CHECK(find(orb_shell_root(s_shell), "Notice B"));
    CHECK(!find(orb_shell_root(s_shell), "Notice A"));
    capture(argv[2], "notification-context-restored");
    interrupted_unlock(argv[2]);
    orb_shell_destroy(s_shell);
    printf("system Home context %s: %u checks passed\n", argv[1], s_checks);
    return 0;
}
