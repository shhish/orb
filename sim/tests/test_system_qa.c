/* Pointer-driven system regressions. All services and data stay in process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "orb_home.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_sound.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static const char *s_case;
static orb_shell_snapshot_t s_status;
#define CHECK(v) do { ++s_checks; if (!(v)) { \
    fprintf(stderr, "system QA %s:%d at %u: %s\n", s_case, __LINE__, s_now, #v); exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *pointer, lv_indev_data_t *out) { (void)pointer; *out = s_input; }
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
    if ((rim && !strcasecmp(rim, text)) || (button && !strcmp(button, text))) return root;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *shown = lv_label_get_text(root);
        size_t length = strlen(shown), ellipsis = sizeof("…") - 1;
        /* Native row titles can end in the shared single U+2026 after using
         * both lines. The displayed unique prefix still identifies the row. */
        if (!strcmp(shown, text) || (length > ellipsis &&
            !strcmp(shown + length - ellipsis, "…") && !strncmp(text, shown, length - ellipsis))) return root;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text); if (found) return found;
    }
    return NULL;
}
static void boot(void)
{
    orb_shell_button(s_shell, &(orb_button_event_t){.button=ORB_BUTTON_BOOT, .gesture=ORB_BUTTON_SHORT});
    advance(1200);
}
static void route(orb_shell_surface_t surface)
{
    orb_shell_navigate(s_shell, surface); advance(1200);
    CHECK(orb_shell_surface(s_shell) == surface);
}
static void tap(const char *text)
{
    lv_obj_t *target = find(orb_shell_root(s_shell), text);
    if (!target) fprintf(stderr, "Missing tap target: %s\n", text);
    CHECK(target);
    while (target && !lv_obj_is_clickable(target)) target = lv_obj_get_parent(target);
    CHECK(target);
    lv_obj_scroll_to_view(target, LV_ANIM_OFF); advance(80);
    lv_area_t area; lv_obj_get_coords(target, &area);
    s_input.point = (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); advance(40);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(1200);
}
static void capture(const char *directory)
{
    if (!directory) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", directory, s_case);
    CHECK(sim_display_save_png(path));
}
static void context(void)
{
    route(ORB_SHELL_DISPLAY); tap("Brightness");
    /* Local fresh arrival counts for at most 420 ms after its fence. */
    advance(450); CHECK(find(orb_shell_root(s_shell), "70%"));
    orb_shell_button(s_shell, &(orb_button_event_t){.button=ORB_BUTTON_PWR, .gesture=ORB_BUTTON_DOUBLE});
    advance(1200); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    boot(); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_DISPLAY);
    CHECK(!find(orb_shell_root(s_shell), "Always On"));
    CHECK(find(orb_shell_root(s_shell), "Brightness"));
    boot(); CHECK(find(orb_shell_root(s_shell), "Always On"));
    orb_card_t a = {.id="context-a", .kind=ORB_CARD_ATTENTION, .title="Notice A", .body="Body A"};
    orb_card_t b = {.id="context-b", .kind=ORB_CARD_ATTENTION, .title="Notice B", .body="Body B"};
    CHECK(orb_model_upsert_card(&a)); CHECK(orb_model_upsert_card(&b)); advance(200);
    route(ORB_SHELL_NOTIFICATIONS); tap("Notice B");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATION_DETAIL);
    CHECK(find(orb_shell_root(s_shell), "Body B"));
    route(ORB_SHELL_STACK); boot();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATION_DETAIL);
    CHECK(find(orb_shell_root(s_shell), "Body B"));
    CHECK(!find(orb_shell_root(s_shell), "Body A"));
}
static void connectivity(void)
{
    route(ORB_SHELL_CONTROL_CENTER);
    CHECK(find(orb_shell_root(s_shell), "Control Center"));
    s_status.link = ORB_SHELL_LINK_OFFLINE;
    orb_shell_update(s_shell, &s_status); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "Control Center"));
    route(ORB_SHELL_SETTINGS); tap("Mac");
    CHECK(find(orb_shell_root(s_shell), "Mac offline"));
    s_status.link = ORB_SHELL_LINK_ONLINE;
    orb_shell_update(s_shell, &s_status); advance(1200);
    CHECK(!find(orb_shell_root(s_shell), "Mac offline"));
    route(ORB_SHELL_CONTROL_CENTER);
    CHECK(find(orb_shell_root(s_shell), "Control Center"));
}
static void title(void)
{
    orb_card_t card = {.id="qa-system-title", .kind=ORB_CARD_ATTENTION,
        .title="Guest Suite Bath ceiling lights need the connection restored now",
        .body="Every word of this notification remains readable."};
    CHECK(orb_model_upsert_card(&card)); advance(1200);
    route(ORB_SHELL_NOTIFICATIONS); tap("System");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATION_DETAIL);
    CHECK(find(orb_shell_root(s_shell), card.title));
    unsigned volume = orb_sound_volume();
    (void)orb_shell_twist(s_shell, 8); advance(400);
    CHECK(orb_sound_volume() == volume);
    CHECK(find(orb_shell_root(s_shell), card.body));
}
static void stack_clock(void)
{
    strcpy(s_status.clock_date, "Wednesday, October 28");
    orb_shell_update(s_shell, &s_status); route(ORB_SHELL_STACK);
    CHECK(find(orb_shell_root(s_shell), "Smart Stack"));
    strcpy(s_status.clock_time, "10:10"); orb_shell_update(s_shell, &s_status); advance(1200);
    CHECK(!find(orb_shell_root(s_shell), "10:10"));
    CHECK(find(orb_shell_root(s_shell), "Smart Stack"));
    CHECK(!find(orb_shell_root(s_shell), "10:09"));
}
static lv_obj_t *fully_visible(const char *text)
{
    lv_obj_t *label = find(orb_shell_root(s_shell), text); CHECK(label);
    lv_area_t ink; lv_obj_get_coords(label, &ink);
    for (lv_obj_t *parent = lv_obj_get_parent(label); parent; parent = lv_obj_get_parent(parent)) {
        if (!lv_obj_is_scrollable(parent)) continue;
        lv_area_t viewport; lv_obj_get_coords(parent, &viewport);
        CHECK(ink.x1 >= viewport.x1 && ink.x2 <= viewport.x2);
        CHECK(ink.y1 >= viewport.y1 && ink.y2 <= viewport.y2);
    }
    return label;
}
static void stack_readers(void)
{
    route(ORB_SHELL_FACE);
    orb_home_room_t rooms[3] = {0}; orb_home_accessory_t lights[3] = {0};
    for (unsigned i = 0; i < 3; i++) {
        snprintf(rooms[i].id, sizeof(rooms[i].id), "reader-room-%u", i);
        snprintf(rooms[i].name, sizeof(rooms[i].name), "Room %u", i);
        snprintf(lights[i].id, sizeof(lights[i].id), "reader-light-%u", i);
        strcpy(lights[i].name, "Light"); strcpy(lights[i].room, rooms[i].id);
        lights[i].kind = ORB_HOME_LIGHT; lights[i].reachable = true;
        lights[i].state = (orb_home_state_t){.present = ORB_HOME_ON, .on = true};
    }
    orb_home_set_connection(true, true);
    CHECK(orb_home_snapshot_chunk(rooms, 3, lights, 3, NULL, 0, true, false));
    advance(100); route(ORB_SHELL_STACK); (void)orb_shell_twist(s_shell, -8); advance(1200);
    fully_visible("In 3 rooms");
    route(ORB_SHELL_FACE);
    for (unsigned i = 0; i < 3; i++) lights[i].state.on = false;
    CHECK(orb_home_snapshot_chunk(rooms, 3, lights, 3, NULL, 0, true, false));
    orb_nowplaying_t media = {.title = "Midnight City", .artist = "M83", .app = "Music",
        .duration = 240, .state = ORB_MEDIA_PLAYING, .volume = 60, .playing = true};
    orb_model_nowplaying_capability(true); CHECK(orb_model_set_nowplaying(&media));
    CHECK(orb_model_nowplaying_request(ORB_MEDIA_PLAYPAUSE, 0)); orb_model_complete_nowplaying("failed");
    advance(100); route(ORB_SHELL_STACK); (void)orb_shell_twist(s_shell, -8); advance(1200);
    lv_obj_t *warning = fully_visible(ORB_ICON_WARNING " Couldn’t control playback");
    lv_obj_t *previous = find(orb_shell_root(s_shell), ORB_ICON_SKIP_BACK_FILL); CHECK(previous);
    while (!lv_obj_is_clickable(previous)) previous = lv_obj_get_parent(previous);
    lv_area_t copy, control; lv_obj_get_coords(warning, &copy); lv_obj_get_coords(previous, &control);
    CHECK(copy.y2 < control.y1);
}
static void home_batch(void)
{
    orb_home_room_t room = {.id="qa-room", .name="Living Room"};
    orb_home_accessory_t lights[6] = {0};
    for (unsigned i = 0; i < 6; i++) {
        snprintf(lights[i].id, sizeof(lights[i].id), "qa-light-%u", i);
        snprintf(lights[i].name, sizeof(lights[i].name), "Light %u", i);
        strcpy(lights[i].room, room.id);
        lights[i].kind = ORB_HOME_LIGHT; lights[i].reachable = true;
        lights[i].writable = ORB_HOME_ON;
        lights[i].state = (orb_home_state_t){.present=ORB_HOME_ON, .on=true};
    }
    orb_home_set_connection(true, true);
    CHECK(orb_home_snapshot_chunk(&room, 1, lights, 6, NULL, 0, true, false));
    advance(100); route(ORB_SHELL_STACK);
    lv_obj_t *off = find(orb_shell_root(s_shell), "All off");
    lv_obj_t *scenes = find(orb_shell_root(s_shell), "Scenes");
    CHECK(off && scenes);
    lv_area_t left, right; lv_obj_get_coords(off, &left); lv_obj_get_coords(scenes, &right);
    bool stacked = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    CHECK(stacked ? right.y1 - left.y2 - 1 >= 20 : right.x1 - left.x2 - 1 >= 20);
    /* AX3 keeps the labels in a vertical pair inside the taller card. */
    CHECK(left.x1 >= 63 && right.x1 >= 63 && left.x2 <= 402 && right.x2 <= 402);
    CHECK(left.y1 >= 96 && right.y1 >= 96);
    CHECK(left.y2 <= (stacked ? 399 : 331) && right.y2 <= (stacked ? 399 : 331));
    tap("All off");
    unsigned completed = 0; orb_home_request_t request;
    while (orb_home_take_request(&request)) {
        CHECK(request.kind == ORB_HOME_REQUEST_SET && request.values.present == ORB_HOME_ON && !request.values.on);
        orb_home_complete(&request, &request.values, NULL); completed++; advance(40);
    }
    CHECK(completed == 6);
}
static void preferences_complete(void)
{
    orb_prefs_patch_t patch; CHECK(orb_model_take_prefs_request(&patch));
    orb_prefs_t prefs; orb_model_prefs_defaults(&prefs);
    if (patch.has_temperature) {
        prefs.temperature = patch.temperature == ORB_TEMPERATURE_UNKNOWN ? ORB_TEMPERATURE_F : patch.temperature;
        prefs.temperature_owner = patch.temperature != ORB_TEMPERATURE_UNKNOWN;
    }
    orb_model_complete_prefs(&prefs, NULL); advance(400);
}
static void feature_walk(void)
{
    route(ORB_SHELL_SETTINGS); tap("Display");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_DISPLAY);
    tap("Always On"); tap("Wake on pickup"); tap("Quiet mode");
    CHECK(orb_shell_quiet(s_shell));
    (void)orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR); advance(800);
    orb_shell_set_quiet(s_shell, false); advance(400);
    tap("Brightness"); CHECK(orb_shell_twist(s_shell, 5)); advance(200); boot();
    tap("Always On brightness"); CHECK(orb_shell_twist(s_shell, -2)); advance(200); boot(); boot();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    route(ORB_SHELL_ACCESSIBILITY);
    const char *access[] = {"Text size", "Bold text", "Reduce motion", "Reduce transparency", "Increase contrast"};
    for (unsigned i = 0; i < 5; i++) {
        tap(access[i]);
        CHECK(find(orb_shell_root(s_shell), access[i]));
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_ACCESSIBILITY);
        if (i == 0) boot(); /* Only Text size is a child destination. */
    }
    route(ORB_SHELL_SETTINGS); tap("Glass"); tap("Tinted"); tap("Clear"); tap("Default"); boot();
    route(ORB_SHELL_UNITS); tap("Temperature");
    const char *units[] = {"°F", "°C", "Automatic"};
    for (unsigned i = 0; i < 3; i++) { tap(units[i]); preferences_complete(); }
    boot(); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_UNITS);
    route(ORB_SHELL_SESSION_SETTINGS);
    const char *sources[] = {"Claude", "Codex", "Dot", "Recent activity"};
    for (unsigned i = 0; i < 4; i++) { tap(sources[i]); preferences_complete(); }
    route(ORB_SHELL_ABOUT); tap("Licenses");
    CHECK(find(orb_shell_root(s_shell), "Inter"));
    CHECK(orb_shell_twist(s_shell, 3)); advance(400); boot();
    CHECK(find(orb_shell_root(s_shell), "About"));
    route(ORB_SHELL_SETTINGS);
    unsigned volume = orb_sound_volume();
    s_input = (lv_indev_data_t){.point={452,233}, .state=LV_INDEV_STATE_PRESSED};
    lv_indev_read(s_pointer); advance(40);
    for (unsigned i = 1; i <= 5; i++) {
        s_input.point = (lv_point_t){452 - (int)i * 7, 233 + (int)i * 20};
        lv_indev_read(s_pointer); advance(40);
    }
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SETTINGS);
    CHECK(orb_sound_volume() == volume);
}
int main(int argc, char **argv)
{
    CHECK(argc >= 3); s_case = argv[1];
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init());
    orb_model_set_connection(true, true);
    if (!strcmp(argv[2], "ax3-bold-rt")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    s_pointer = lv_indev_create(); lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    s_status = (orb_shell_snapshot_t){.net=ORB_SHELL_NET_CONNECTED, .link=ORB_SHELL_LINK_ONLINE,
        .battery_pct=80, .brightness=70};
    strcpy(s_status.clock_time, "10:09"); orb_shell_update(s_shell, &s_status); advance(1200);
    if (!strcmp(s_case, "context")) context();
    else if (!strcmp(s_case, "connectivity")) connectivity();
    else if (!strcmp(s_case, "title")) title();
    else if (!strcmp(s_case, "stack-clock")) { stack_clock(); stack_readers(); }
    else if (!strcmp(s_case, "home-batch")) home_batch();
    else if (!strcmp(s_case, "walk")) feature_walk();
    else CHECK(false);
    capture(argc > 3 ? argv[3] : NULL);
    orb_shell_destroy(s_shell);
    printf("system QA %s: %u assertions passed\n", s_case, s_checks);
    return 0;
}
