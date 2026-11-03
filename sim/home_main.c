/* Home previews and real-pointer animation receipts. Synthetic Demo Home data
 * stays inside orb_home; no network, credentials or physical board are used. */
#include "home_harness.h"
#include <time.h>

size_t home_memory_allocations(void);

static void verify_frame(void)
{
    static uint8_t retained[SIM_D * SIM_D * 3], full[sizeof(retained)];
    sim_display_rgb(retained);
    const char *diagnostic = getenv("ORB_HOME_DIAGNOSTIC");
    char diagnostic_path[1024];
    if (diagnostic) {
        snprintf(diagnostic_path, sizeof(diagnostic_path), "%s-retained.png", diagnostic);
        sim_display_save_png(diagnostic_path);
        snprintf(diagnostic_path, sizeof(diagnostic_path), "%s-retained.rgb", diagnostic);
        FILE *raw = fopen(diagnostic_path, "wb"); if (raw) { fwrite(retained, 1, sizeof(retained), raw); fclose(raw); }
    }
    /* Validate the retained framebuffer itself: previews can misrender PNG
     * regions, and a fresh redraw must never conceal missing navigation. */
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(s_home.display);
    sim_display_rgb(full);
    if (diagnostic) {
        snprintf(diagnostic_path, sizeof(diagnostic_path), "%s-full.png", diagnostic);
        sim_display_save_png(diagnostic_path);
        snprintf(diagnostic_path, sizeof(diagnostic_path), "%s-full.rgb", diagnostic);
        FILE *raw = fopen(diagnostic_path, "wb"); if (raw) { fwrite(full, 1, sizeof(full), raw); fclose(raw); }
    }
    if (memcmp(retained, full, sizeof(full))) {
        unsigned pixels = 0, min_x = SIM_D, min_y = SIM_D, max_x = 0, max_y = 0;
        for (unsigned y = 0; y < SIM_D; y++) for (unsigned x = 0; x < SIM_D; x++) {
            unsigned at = (y * SIM_D + x) * 3;
            if (!memcmp(retained + at, full + at, 3)) continue;
            pixels++; min_x = LV_MIN(min_x, x); min_y = LV_MIN(min_y, y);
            max_x = LV_MAX(max_x, x); max_y = LV_MAX(max_y, y);
        }
        fprintf(stderr, "Full redraw difference: pixels=%u bbox=%u,%u,%u,%u\n", pixels, min_x, min_y, max_x, max_y);
    }
    home_check(!memcmp(retained, full, sizeof(full)), "retained screenshot matches full frozen redraw");
}

static void verify_complete_name(const char *name)
{
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "read imported row identity");
    bool found = false, room_name = false;
    for (unsigned i = 0; i < snapshot->scene_count; ++i) found |= !strcmp(snapshot->scenes[i].name, name);
    for (unsigned i = 0; i < snapshot->room_count; ++i) room_name |= !strcmp(snapshot->rooms[i].name, name);
    found |= room_name;
    home_check(found, "maximum-byte row semantic name is preserved");
    free(snapshot);
    char prefix[5] = "";
    memcpy(prefix, name, LV_MIN(strlen(name), sizeof(prefix) - 1));
    lv_obj_t *label = home_find_text(orb_home_view_root(s_home.view), prefix);
    home_check(label != NULL, "maximum-byte imported row name remains reachable");
    int32_t lines = lv_label_get_max_lines(label), line_height = lv_obj_get_style_text_font(label, 0)->line_height;
    home_check(lines >= 1 && (room_name || lines <= (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? 2 : 1)) &&
        lv_obj_get_height(label) == lines * line_height + (lines - 1) * lv_obj_get_style_text_line_space(label, 0),
        "maximum-byte identity stays in the kit title lane");
    lv_obj_t *parent = lv_obj_get_parent(label);
    lv_area_t text_bounds, row_bounds;
    lv_obj_get_coords(label, &text_bounds); lv_obj_get_coords(parent, &row_bounds);
    home_check(text_bounds.y1 >= row_bounds.y1 && text_bounds.y2 <= row_bounds.y2 &&
        text_bounds.x1 >= row_bounds.x1 && text_bounds.x2 <= row_bounds.x2,
        "row name fits inside its action rather than an ancestor clip");
}

static void save(const char *directory, const char *sequence, unsigned frame)
{
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s-%02u.png", directory, sequence, frame);
    home_check(length > 0 && (size_t)length < sizeof(path), "screenshot path fits");
    home_check(sim_display_save_png(path), "save sequence PNG");
}

static void asynchronous_fixture(void)
{
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot != NULL && orb_home_snapshot(snapshot), "copy synthetic fixture");
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
        snapshot->scene_count, true, false), "install synthetic authoritative snapshot");
    free(snapshot);
}

static void acknowledge(void)
{
    orb_home_request_t request;
    orb_home_snapshot_t *snapshot = calloc(1,sizeof(*snapshot));
    home_check(snapshot != NULL, "allocate synthetic authority snapshot");
    while (orb_home_take_request(&request)) {
        home_check(orb_home_snapshot(snapshot), "read synthetic authoritative result");
        const orb_home_accessory_t *a = orb_home_find(snapshot,request.id);
        orb_home_complete(&request, a ? &a->state : NULL, NULL);
    }
    free(snapshot);
    home_advance(16);
}

static void scroll_list_step(lv_obj_t *list)
{
    home_check(list != NULL, "surface exposes its native scroll owner");
    lv_area_t bounds; lv_obj_get_coords(list, &bounds);
    int start = LV_MIN(360, bounds.y2 - 24);
    int distance = LV_MIN(160, start - bounds.y1 - 24);
    int before = lv_obj_get_scroll_y(list);
    home_press(233, start); home_advance(10);
    for (unsigned step = 1; step <= 40; step++) {
        home_press(233, start - (int)step * distance / 40);
        home_advance(10);
    }
    home_release(); home_advance(800);
    home_check(lv_obj_is_valid(list) && lv_obj_get_scroll_y(list) > before,
        "finger drag advances the native list without rebuilding its rows");
}

static void scroll_room_tail(void)
{
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    home_check(list != NULL, "room exposes its native scroll continuation");
    lv_obj_update_layout(list);
    for (unsigned pass = 0; pass < 64 && lv_obj_get_scroll_bottom(list) > 1; pass++)
        scroll_list_step(list);
    home_check(lv_obj_get_scroll_y(list) > 0 && lv_obj_get_scroll_bottom(list) <= 1,
        "finger drags reach the final room accessory");
}

static void scroll_to_room(unsigned index)
{
    home_advance(16);
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot) && index < snapshot->room_count,
        "requested room exists in synthetic list");
    char token[48];
    snprintf(token, sizeof(token), "room:%s", snapshot->rooms[index].id);
    free(snapshot);
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    lv_obj_t *row = home_find_id(orb_home_view_root(s_home.view), token);
    home_check(list && row, "every room has a reachable named row");
    lv_area_t bounds;
    lv_obj_get_coords(row, &bounds);
    int position = lv_obj_get_scroll_y(list) + (bounds.y1 + bounds.y2 + 1) / 2 - 233;
    lv_obj_scroll_to_y(list, position, LV_ANIM_OFF);
    home_advance(800);
}

static void rooms_list_fixture(void)
{
    static const char *names[] = {"Dining", "Living Room", "Kitchen", "Bedroom", "Office", "Bathroom", "Garage"};
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot != NULL, "allocate named room-list fixture");
    snapshot->room_count = sizeof(names) / sizeof(*names);
    snapshot->accessory_count = snapshot->room_count + 3;
    for (unsigned i = 0; i < snapshot->room_count; ++i) {
        snprintf(snapshot->rooms[i].id, sizeof(snapshot->rooms[i].id), "room-%u", i);
        snprintf(snapshot->rooms[i].name, sizeof(snapshot->rooms[i].name), "%s", names[i]);
        orb_home_accessory_t *a = &snapshot->accessories[i];
        snprintf(a->id, sizeof(a->id), "light-%u", i);
        snprintf(a->name, sizeof(a->name), "%s Light", names[i]);
        snprintf(a->room, sizeof(a->room), "%s", snapshot->rooms[i].id);
        a->kind = i == 2 ? ORB_HOME_SWITCH : ORB_HOME_LIGHT;
        a->reachable = true; a->writable = ORB_HOME_ON;
        a->state = (orb_home_state_t){.present = ORB_HOME_ON, .on = i < 3};
    }
    for (unsigned i = 7; i < snapshot->accessory_count; ++i) {
        orb_home_accessory_t *a = &snapshot->accessories[i];
        snprintf(a->id, sizeof(a->id), "temperature-%u", i);
        snprintf(a->name, sizeof(a->name), "Temperature");
        snprintf(a->room, sizeof(a->room), "room-%u", i - 7);
        a->kind = i == 7 ? ORB_HOME_THERMOSTAT : ORB_HOME_TEMPERATURE;
        a->reachable = true;
        a->state = (orb_home_state_t){.present = ORB_HOME_TEMP, .temp = 21 + (float)(i - 7)};
    }
    snapshot->scene_count = 1;
    snprintf(snapshot->scenes[0].id, sizeof(snapshot->scenes[0].id), "evening");
    snprintf(snapshot->scenes[0].name, sizeof(snapshot->scenes[0].name), "Evening");
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
        snapshot->scene_count, true, false), "publish seven-room synthetic list fixture");
    free(snapshot);
    orb_home_set_connection(true, true);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS);
    home_advance(800);
}

static void select_scene(const char *scene)
{
    if (!strncmp(scene, "detail-", 7)) {
        const char *status = strrchr(scene, '-');
        home_check(status != NULL && status != scene + 6, "detail matrix has a state suffix");
        char kind[24];
        size_t length = (size_t)(status - scene - 7);
        home_check(length < sizeof(kind), "detail matrix kind fits");
        memcpy(kind, scene + 7, length); kind[length] = 0; ++status;
        orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "copy detail state matrix fixture");
        if (!strcmp(status, "roomlong"))
            for (unsigned i = 0; i < snapshot->room_count; i++)
                snprintf(snapshot->rooms[i].name, sizeof(snapshot->rooms[i].name),
                    "Living Room and Downstairs Study");
        orb_home_kind_t selected = !strcmp(kind, "thermostat") || !strcmp(kind, "modes") ? ORB_HOME_THERMOSTAT :
            !strcmp(kind, "fan") ? ORB_HOME_FAN : !strcmp(kind, "lock") ? ORB_HOME_LOCK :
            !strcmp(kind, "switch") ? ORB_HOME_SWITCH : !strcmp(kind, "outlet") ? ORB_HOME_OUTLET :
            !strcmp(kind, "temperature") ? ORB_HOME_TEMPERATURE : !strcmp(kind, "humidity") ? ORB_HOME_HUMIDITY :
            !strcmp(kind, "motion") ? ORB_HOME_MOTION : !strcmp(kind, "contact") ? ORB_HOME_CONTACT : ORB_HOME_LIGHT;
        char id[33] = "";
        for (unsigned i = 0; i < snapshot->accessory_count; ++i) {
            orb_home_accessory_t *accessory = &snapshot->accessories[i];
            if (accessory->kind != selected) continue;
            snprintf(id, sizeof(id), "%s", accessory->id);
            if (!strcmp(status, "long"))
                snprintf(accessory->name, sizeof(accessory->name), "Evening Reading Lamp by Window");
            if (!strcmp(status, "empty")) accessory->state.present = 0;
            if (!strcmp(status, "error")) accessory->reachable = false;
            break;
        }
        home_check(id[0] != 0, "detail matrix kind exists");
        home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
            snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
            snapshot->scene_count, true, false), "publish detail state matrix fixture");
        free(snapshot);
        orb_home_view_open(s_home.view, id); home_advance(800);
        if (!strcmp(kind, "thermostat") && !strcmp(status, "roomlong")) {
            lv_obj_t *context = home_find_text(orb_home_view_root(s_home.view), "Heat · Now 22°C");
            home_check(context && !strcmp(lv_label_get_text(context), "Heat · Now 22°C") &&
                lv_label_get_long_mode(context) == LV_LABEL_LONG_MODE_CLIP,
                "long room context cannot corrupt thermostat live text through DOTS storage");
        }
        if (!strcmp(kind, "modes")) orb_home_view_detail_page(s_home.view, 1);
        else if (!strcmp(kind, "colour")) orb_home_view_detail_page(s_home.view, 1);
        if (!strcmp(status, "offline")) orb_home_set_connection(false, false);
        return;
    }
    if (!strncmp(scene, "design2-", 8)) {
        orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "copy Design 2.0 fixture");
        if (!strcmp(scene, "design2-rooms-long") || !strcmp(scene, "design2-rooms-wide")) {
            static const char *names[] = {"Bedroom", "Guest Suite Bath", "Kitchen", "Office", "Guest Bath", "Reading Room & Family Library"};
            snapshot->room_count = sizeof(names) / sizeof(*names);
            for (unsigned i = 0; i < snapshot->room_count; ++i) {
                if (i == 5) snprintf(snapshot->rooms[i].id, sizeof(snapshot->rooms[i].id), "reading");
                snprintf(snapshot->rooms[i].name, sizeof(snapshot->rooms[i].name), "%s", names[i]);
            }
            if (!strcmp(scene, "design2-rooms-wide"))
                snprintf(snapshot->rooms[5].name, sizeof(snapshot->rooms[5].name), "%s",
                    "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
        }
        if (!strcmp(scene, "design2-scenes-empty")) snapshot->scene_count = 0;
        if (!strcmp(scene, "design2-climate-empty") || !strcmp(scene, "design2-sensors-empty"))
            snapshot->accessory_count = 0;
        if (!strcmp(scene, "design2-scenes-long"))
            snprintf(snapshot->scenes[0].name, sizeof(snapshot->scenes[0].name), "Evening Reading with Warm Lights");
        home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
            snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
            snapshot->scene_count, true, false), "publish Design 2.0 fixture");
        free(snapshot);
        home_advance(800);
        if (!strcmp(scene, "design2-rooms-long") || !strcmp(scene, "design2-rooms-wide")) scroll_to_room(5);
        else if (!strcmp(scene, "design2-climate-empty") || !strcmp(scene, "design2-sensors-empty")) {
            orb_home_view_select(s_home.view, ORB_HOME_CLIMATE); home_advance(800);
            if (!strcmp(scene, "design2-sensors-empty")) home_tap_text("Sensors");
        }
        else if (!strcmp(scene, "design2-room-offline")) {
            orb_home_view_room(s_home.view, "Living Room"); home_advance(800);
            orb_home_set_connection(false, false);
        } else if (!strcmp(scene, "design2-scenes-empty")) {
            /* An already-open page can become empty even though the landing
             * list correctly omits its entry when no safe scenes remain. */
            orb_home_view_state_t state;
            orb_home_view_save(s_home.view, &state);
            state.scene_page = true;
            orb_home_view_restore(s_home.view, &state);
        } else {
            home_tap_text("Scenes");
            if (!strcmp(scene, "design2-scenes-offline")) orb_home_set_connection(false, false);
        }
        return;
    }
    if (!strncmp(scene, "rooms-list-", 11)) {
        rooms_list_fixture();
        if (!strcmp(scene, "rooms-list-end")) scroll_room_tail();
        else if (!strncmp(scene, "rooms-list-centre-", 18)) scroll_to_room((unsigned)strtoul(scene + 18, NULL, 10));
        else if (!strcmp(scene, "rooms-list-off")) scroll_to_room(3);
        else if (!strcmp(scene, "rooms-list-pending") || !strcmp(scene, "rooms-list-failed")) {
            home_check(orb_home_toggle("light-0"), "queue synthetic room-list fixture request");
            if (!strcmp(scene, "rooms-list-failed")) {
                orb_home_request_t request;
                home_check(orb_home_take_request(&request), "take synthetic room-list fixture request");
                orb_home_complete(&request, NULL, "unreachable");
            }
        } else if (!strcmp(scene, "rooms-list-scenes")) home_tap_text("Scenes");
        else if (!strcmp(scene, "rooms-list-stale")) orb_home_set_connection(true, false);
        else if (!strcmp(scene, "rooms-list-offline")) orb_home_set_connection(false, false);
        else if (!strcmp(scene, "rooms-list-empty")) {
            home_check(orb_home_snapshot_chunk(NULL, 0, NULL, 0, NULL, 0, true, false),
                "publish available Home with no rooms");
        }
        return;
    }
    if (!strcmp(scene, "empty-home") || !strcmp(scene, "loading-home")) {
        orb_home_reset();
        orb_home_set_connection(true, false);
        if (!strcmp(scene, "empty-home"))
            home_check(orb_home_snapshot_chunk(NULL, 0, NULL, 0, NULL, 0, true, false),
                "publish an available home with no rooms or devices");
        orb_home_view_select(s_home.view, ORB_HOME_ROOMS);
        return;
    }
    if (!strcmp(scene, "busy-rooms") || !strcmp(scene, "busy-room") ||
        !strcmp(scene, "busy-room-scrolled") ||
        !strcmp(scene, "busy-room-last") || !strcmp(scene, "room-all-off-row") || !strcmp(scene, "empty-room") ||
        !strcmp(scene, "room-actions") || !strncmp(scene, "all-off-", 8)) {
        orb_prefs_t prefs;
        orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_F;
        home_check(orb_model_set_prefs(&prefs), "busy home uses owner Fahrenheit preference");
        home_busy_room_fixture();
        if (!strcmp(scene, "busy-rooms")) return;
        orb_home_view_room(s_home.view, !strcmp(scene, "empty-room") ? "empty" : "busy");
        home_advance(800);
        if (!strcmp(scene, "room-actions") || !strncmp(scene, "all-off-", 8)) {
            home_tap_text("All off");
            orb_home_view_state_t state;
            orb_home_view_save(s_home.view, &state);
            home_check(state.sections && !orb_home_take_request(&(orb_home_request_t){0}),
                "named All off row opens room actions without sending a command");
        }
        if (!strncmp(scene, "all-off-", 8)) {
            home_tap_text("All off");
            if (!strcmp(scene, "all-off-pending")) return;
            orb_home_request_t request;
            unsigned completed = 0;
            while (orb_home_take_request(&request))
                orb_home_complete(&request, &request.values,
                    !strcmp(scene, "all-off-failed") && completed++ == 0 ? "rejected" : NULL);
            return;
        }
        if (!strcmp(scene, "room-all-off-row")) {
            scroll_room_tail();
            lv_obj_t *list = orb_home_view_scroll(s_home.view);
            lv_obj_t *row = home_find_action(orb_home_view_root(s_home.view), "All off");
            lv_area_t bounds, viewport;
            home_check(row && lv_obj_get_parent(row) == list, "room tail contains its named All off row");
            lv_obj_get_coords(row, &bounds); lv_obj_get_coords(list, &viewport);
            home_check(bounds.y1 >= viewport.y1 && bounds.y2 <= viewport.y2,
                "the complete All off row scrolls inside its room viewport");
            orb_home_view_state_t state;
            orb_home_view_save(s_home.view, &state);
            home_check(!state.sections && !orb_home_take_request(&(orb_home_request_t){0}),
                "reaching the All off row leaves room actions closed and sends no command");
        } else if (!strcmp(scene, "busy-room-last")) {
            scroll_room_tail();
            home_check(home_has_text("D24"), "continuation reaches the 24th device");
        } else if (!strcmp(scene, "busy-room-scrolled"))
            scroll_list_step(orb_home_view_scroll(s_home.view));
        return;
    }
    if (!strncmp(scene, "pending-", 8)) {
        asynchronous_fixture();
        const char *id = !strcmp(scene, "pending-switch") || !strcmp(scene, "pending-room") ? "kitchen" :
            !strcmp(scene, "pending-lock") ? "front-door" : "floor-lamp";
        if (!strcmp(scene, "pending-room")) orb_home_view_room(s_home.view, "Kitchen");
        else orb_home_view_open(s_home.view, id);
        home_advance(800);
        if (!strcmp(id, "front-door"))
            home_check(orb_home_set(id, &(orb_home_state_t){.present = ORB_HOME_LOCKED, .locked = false}, true),
                "queue deliberate synthetic-fixture unlock");
        else home_check(orb_home_toggle(id), "queue in-flight fixture toggle");
        return;
    }
    if (!strcmp(scene, "scene-pending") || !strcmp(scene, "scene-accepted") ||
        !strcmp(scene, "scene-failed") || !strcmp(scene, "capsule-pending") ||
        !strcmp(scene, "capsule-accepted") || !strcmp(scene, "capsule-failed")) {
        asynchronous_fixture();
        orb_home_view_hour(s_home.view, 9);
        orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
        if (!strncmp(scene, "scene-", 6)) {
            home_tap_text("Scenes"); home_tap_text("Good morning");
        } else { home_tap_text("Scenes"); home_tap_text("Good Night"); }
        if (strstr(scene, "pending")) return;
        orb_home_request_t request;
        home_check(orb_home_take_request(&request) && request.kind == ORB_HOME_REQUEST_SCENE,
            "physical scene control queues exactly a scene request");
        orb_home_complete(&request, NULL, strstr(scene, "failed") ? "unreachable" : NULL);
        return;
    }
    if (!strncmp(scene, "thermostat-f-", 13)) {
        orb_prefs_t prefs;
        orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_F;
        home_check(orb_model_set_prefs(&prefs), "set owner Fahrenheit fixture");
        const char *mode = scene + 13;
        orb_home_mode_t value = !strcmp(mode, "off") ? ORB_HOME_MODE_OFF :
            !strcmp(mode, "cool") ? ORB_HOME_MODE_COOL : !strcmp(mode, "auto") ? ORB_HOME_MODE_AUTO : ORB_HOME_MODE_HEAT;
        home_check(orb_home_update("climate", &(orb_home_state_t){.present = ORB_HOME_MODE, .mode = value},
            false, true, 0), "publish thermostat mode");
        orb_home_view_open(s_home.view, "climate");
        if (!strcmp(mode, "modes")) { home_advance(800); orb_home_view_detail_page(s_home.view, 1); }
        return;
    }
    if (!strcmp(scene, "sensor-aged") || !strcmp(scene, "sensor-unknown-age") ||
        !strcmp(scene, "sensor-future-age")) {
        orb_prefs_t prefs;
        orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_F;
        home_check(orb_model_set_prefs(&prefs), "sensor age uses owner Fahrenheit preference");
        orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "copy sensor-age fixture");
        time_t now = time(NULL);
        for (unsigned i = 0; i < snapshot->accessory_count; ++i) {
            if (strcmp(snapshot->accessories[i].id, "bed-temp")) continue;
            snapshot->accessories[i].updated = !strcmp(scene, "sensor-aged") ? (uint64_t)now - 125 :
                !strcmp(scene, "sensor-future-age") ? (uint64_t)now + 60 : 0;
        }
        home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
            snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
            snapshot->scene_count, true, false), "publish sensor age");
        free(snapshot);
        orb_home_view_open(s_home.view, "bed-temp");
        return;
    }
    if (!strncmp(scene, "hub-", 4)) {
        home_wire_fixture();
        orb_home_view_select(s_home.view, ORB_HOME_ROOMS);
        home_advance(800);
        const char *surface = scene + 4;
        if (!strcmp(surface, "home")) return;
        if (!strcmp(surface, "rooms")) { orb_home_view_select(s_home.view, ORB_HOME_ROOMS); return; }
        if (!strcmp(surface, "climate")) { orb_home_view_select(s_home.view, ORB_HOME_CLIMATE); return; }
        if (!strcmp(surface, "room")) { orb_home_view_room(s_home.view, "rf3d8994d0b87"); return; }
        orb_home_kind_t kind = !strcmp(surface, "lock") ? ORB_HOME_LOCK :
            !strcmp(surface, "fan") ? ORB_HOME_FAN : ORB_HOME_LIGHT;
        home_open_kind(kind);
        if (!strcmp(surface, "colour")) orb_home_view_detail_page(s_home.view, 1);
        return;
    }
    if (!strcmp(scene, "home")) return;
    if (!strcmp(scene, "room-long-names")) { home_long_room_fixture(); return; }
    if (!strcmp(scene, "offline-climate") || !strcmp(scene, "offline-rooms") || !strcmp(scene, "offline-sensors")) {
        orb_home_set_connection(false, false);
        orb_home_view_select(s_home.view, !strcmp(scene, "offline-rooms") ? ORB_HOME_ROOMS : ORB_HOME_CLIMATE);
        home_advance(800);
        if (!strcmp(scene, "offline-sensors")) home_tap_text("Sensors");
        home_check(home_has_text(!strcmp(scene, "offline-rooms") ? "Connect your" : "Last known"),
            "cached overview readings are visibly qualified");
        return;
    }
    if (!strcmp(scene, "no-favourites") || !strncmp(scene,"lights-",7) ||
        !strcmp(scene,"switch-light-summary")) {
        orb_home_snapshot_t *snapshot = calloc(1,sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "copy summary fixture");
        unsigned lights = !strncmp(scene,"lights-",7) ? (unsigned)strtoul(scene+7,NULL,10) : 0;
        unsigned lit = 0;
        if (!strncmp(scene,"lights-",7)) {
            size_t kept = 0;
            for (size_t i = 0; i < snapshot->accessory_count; ++i) {
                orb_home_accessory_t *a = &snapshot->accessories[i];
                if (a->kind != ORB_HOME_SWITCH && a->kind != ORB_HOME_OUTLET)
                    snapshot->accessories[kept++] = *a;
            }
            snapshot->accessory_count = kept;
        }
        for (unsigned i = 0; i < snapshot->accessory_count; ++i) {
            orb_home_accessory_t *a = &snapshot->accessories[i];
            if (!strcmp(scene,"no-favourites")) a->favourite = false;
            else {
                if (a->state.present & ORB_HOME_ON) a->state.on = false;
                if (a->kind == ORB_HOME_LIGHT && lit < lights) { a->state.on = true; ++lit; }
                if (!strcmp(scene,"switch-light-summary") && !strcmp(a->id,"kitchen")) a->state.on = true;
            }
        }
        home_check(orb_home_snapshot_chunk(snapshot->rooms,snapshot->room_count,
            snapshot->accessories,snapshot->accessory_count,snapshot->scenes,
            snapshot->scene_count,true,false), "publish summary fixture");
        free(snapshot);
        orb_home_view_select(s_home.view,ORB_HOME_ROOMS);
        return;
    }
    if (!strcmp(scene, "morning")) { orb_home_view_hour(s_home.view, 9); return; }
    if (!strcmp(scene, "evening")) { orb_home_view_hour(s_home.view, 19); return; }
    if (!strcmp(scene, "night")) { orb_home_view_hour(s_home.view, 23); return; }
    if (!strcmp(scene, "rooms")) { orb_home_view_select(s_home.view, ORB_HOME_ROOMS); return; }
    if (!strcmp(scene, "rooms-scrolled")) {
        orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
        scroll_to_room(1);
        return;
    }
    if (!strcmp(scene,"rooms-next")) {
        orb_home_view_select(s_home.view,ORB_HOME_ROOMS);
        home_advance(800);
        scroll_to_room(4);
        home_check(home_has_text("Dining"), "stepping Rooms reveals the remaining named room");
        return;
    }
    if (!strcmp(scene, "climate")) { orb_home_view_select(s_home.view, ORB_HOME_CLIMATE); return; }
    if (!strcmp(scene,"climate-sensors")) {
        orb_home_view_select(s_home.view,ORB_HOME_CLIMATE); home_advance(800);
        home_tap_text("Sensors"); return;
    }
    if (!strcmp(scene,"home-scenes")) {
        home_tap_text("Scenes"); home_advance(800); return;
    }
    if (!strcmp(scene,"room-living-next")) {
        orb_home_view_room(s_home.view,"Living Room");
        home_advance(800); scroll_room_tail();
        home_check(home_has_text("Sensor"),
            "scrolling reveals the remaining room accessory with a contextual name");
        return;
    }
    if (!strcmp(scene, "room-thermostat-negative") || !strcmp(scene, "room-thermostat-three-digit")) {
        orb_prefs_t prefs;
        orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_F;
        home_check(orb_model_set_prefs(&prefs), "set Fahrenheit temperature-range fixture");
        float fahrenheit = !strcmp(scene, "room-thermostat-negative") ? -5 : 100;
        home_check(orb_home_update("climate", &(orb_home_state_t){.present = ORB_HOME_TEMP,
            .temp = (fahrenheit - 32) * 5 / 9}, false, true, 0), "publish visible thermostat range fixture");
        orb_home_view_room(s_home.view, "Living Room");
        return;
    }
    if (!strncmp(scene, "room-", 5)) {
        const char *room = !strcmp(scene + 5, "living") ? "Living Room" :
                           !strcmp(scene + 5, "bedroom") ? "Bedroom" :
                           !strcmp(scene + 5, "kitchen") ? "Kitchen" :
                           !strcmp(scene + 5, "office") ? "Office" : "Dining";
        orb_home_view_room(s_home.view, room);
        return;
    }
    if (!strcmp(scene, "offline") || !strcmp(scene,"offline-lock") || !strcmp(scene,"offline-unlocked")) {
        if (!strcmp(scene,"offline-unlocked"))
            home_check(orb_home_update("front-door",&(orb_home_state_t){.present=ORB_HOME_LOCKED,.locked=false},
                false,true,0), "report unlocked before disconnecting");
        orb_home_set_connection(false, false);
        if (strcmp(scene,"offline")) home_open_kind(ORB_HOME_LOCK);
        return;
    }
    if (!strcmp(scene, "unavailable") || !strcmp(scene, "unavailable-lock")) {
        orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "copy unavailable Home fixture");
        for (size_t i = 0; i < snapshot->accessory_count; i++) snapshot->accessories[i].reachable = false;
        home_check(orb_home_snapshot_chunk_available(snapshot->rooms, snapshot->room_count,
            snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
            snapshot->scene_count, false, true, false), "publish unavailable Home snapshot");
        free(snapshot);
        if (!strcmp(scene, "unavailable-lock")) home_open_kind(ORB_HOME_LOCK);
        return;
    }
    if (!strcmp(scene, "waiting")) { orb_home_snapshot_discard(); return; }
    if (!strcmp(scene, "unknown-lock")) {
        asynchronous_fixture();
        orb_home_state_t locked = {.present=ORB_HOME_LOCKED, .locked=true};
        home_check(orb_home_set("front-door", &locked, false), "queue synthetic explicit lock");
        orb_home_request_t request;
        home_check(orb_home_take_request(&request), "take synthetic lock");
        orb_home_complete(&request, &(orb_home_state_t){0}, NULL);
        home_open_kind(ORB_HOME_LOCK); return;
    }
    if (!strncmp(scene, "wire-", 5)) {
        orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "copy canonical sensor fixture");
        const char *id = !strcmp(scene, "wire-temperature") ? "bed-temp" :
            !strcmp(scene, "wire-humidity") ? "kitchen-temp" : !strcmp(scene, "wire-motion") ? "hall-motion" : "window";
        for (size_t i = 0; i < snapshot->accessory_count; i++) {
            orb_home_accessory_t *a = &snapshot->accessories[i];
            if (strcmp(a->id,id)) continue;
            a->kind = ORB_HOME_SENSOR;
            if (!strcmp(scene, "wire-humidity")) a->state.present &= ~ORB_HOME_TEMP;
        }
        home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
            snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
            snapshot->scene_count, true, false), "publish canonical wire sensor");
        free(snapshot); orb_home_view_open(s_home.view, id); return;
    }
    if (!strcmp(scene, "no-response")) { orb_home_view_open(s_home.view, "bookshelf"); return; }
    if (!strcmp(scene, "light") || !strcmp(scene, "light-off") || !strcmp(scene, "light-state-cycle") ||
        !strcmp(scene, "colour") || !strcmp(scene, "scenes")) {
        orb_home_view_open(s_home.view, "floor-lamp");
        home_advance(800);
        if (!strcmp(scene, "light-off") || !strcmp(scene, "light-state-cycle")) {
            unsigned cycles = !strcmp(scene, "light-state-cycle") ? 40 : 1;
            for (unsigned i = 0; i < cycles; i++) {
                orb_home_state_t state = {.present = ORB_HOME_ON, .on = (i % 2) != 0};
                home_check(orb_home_update("floor-lamp", &state, true, true, 0), "update authoritative light power");
                home_advance(40);
            }
        } else if (strcmp(scene, "light")) orb_home_view_detail_page(s_home.view, !strcmp(scene, "colour") ? 1 : 2);
        return;
    }
    if (!strcmp(scene, "white")) {
        orb_home_view_open(s_home.view, "dining");
        home_advance(800);
        orb_home_view_detail_page(s_home.view, 1);
        return;
    }
    if (!strcmp(scene, "thermostat") || !strcmp(scene, "modes")) {
        home_open_kind(ORB_HOME_THERMOSTAT);
        if (!strcmp(scene, "modes")) orb_home_view_detail_page(s_home.view, 1);
        return;
    }
    if (!strcmp(scene, "cover") || !strcmp(scene, "cover-zero") || !strcmp(scene, "cover-full")) {
        orb_home_snapshot_t *fixture = calloc(1, sizeof(*fixture));
        home_check(fixture && orb_home_snapshot(fixture), "copy blind level fixture");
        orb_home_accessory_t *cover = NULL;
        for (unsigned i = 0; i < fixture->accessory_count; i++) {
            if (fixture->accessories[i].kind != ORB_HOME_FAN) continue;
            cover = &fixture->accessories[i]; break;
        }
        home_check(cover != NULL, "fixture has an accessory for the blind level");
        cover->kind = ORB_HOME_COVER;
        cover->writable = ORB_HOME_POSITION;
        cover->state = (orb_home_state_t){.present = ORB_HOME_POSITION, .position = 60};
        snprintf(cover->name, sizeof(cover->name), "Bedroom Blinds");
        char id[33]; snprintf(id, sizeof(id), "%s", cover->id);
        home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count,
            fixture->accessories, fixture->accessory_count, fixture->scenes,
            fixture->scene_count, true, false), "publish blind position fixture");
        free(fixture);
        home_open_kind(ORB_HOME_COVER);
        home_advance(800);
        lv_obj_t *value = home_find_text(orb_home_view_root(s_home.view), "60%");
        orb_kit_value_info_t live;
        home_check(value && orb_kit_value_info(value, &live) && live.tabular && !live.raster_prepares,
            "blinds use a native tabular live value");
        int width = lv_obj_get_width(value);
        unsigned measures = live.template_measures;
        const int positions[] = {0, 100, !strcmp(scene, "cover-zero") ? 0 : !strcmp(scene, "cover-full") ? 100 : 60};
        for (unsigned i = 0; i < sizeof(positions) / sizeof(*positions); i++) {
            orb_home_state_t state = {.present = ORB_HOME_POSITION, .position = positions[i]};
            home_check(orb_home_update(id, &state, false, true, 0), "update authoritative blind position");
            home_advance(800);
            home_check(lv_obj_get_width(value) == width && orb_kit_value_info(value, &live) &&
                live.template_measures == measures && !live.raster_prepares,
                "blind endpoints retain label geometry without raster work");
            lv_area_t bounds; lv_obj_get_coords(value, &bounds);
            lv_point_t text_size;
            lv_text_get_size(&text_size, lv_label_get_text(value), lv_obj_get_style_text_font(value, 0),
                lv_obj_get_style_text_letter_space(value, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            lv_text_align_t align = lv_obj_get_style_text_align(value, 0);
            int center = align == LV_TEXT_ALIGN_CENTER ? (bounds.x1 + bounds.x2 + 1) / 2 :
                align == LV_TEXT_ALIGN_RIGHT ? bounds.x2 + 1 - text_size.x / 2 : bounds.x1 + text_size.x / 2;
            home_check(abs(center - 233) <= 1,
                "actual blind value advances stay centered at zero and full scale");
        }
        return;
    }
    if (!strcmp(scene, "fan")) { home_open_kind(ORB_HOME_FAN); return; }
    if (!strcmp(scene, "lock") || !strcmp(scene, "unlocked")) {
        if (!strcmp(scene, "unlocked")) {
            orb_home_state_t state = {.present = ORB_HOME_LOCKED, .locked = false};
            home_check(orb_home_update("front-door", &state, false, true, 0), "unlock synthetic fixture");
        }
        home_open_kind(ORB_HOME_LOCK);
        return;
    }
    if (!strcmp(scene, "switch")) { home_open_kind(ORB_HOME_SWITCH); return; }
    if (!strcmp(scene, "outlet")) { home_open_kind(ORB_HOME_OUTLET); return; }
    if (!strcmp(scene, "temperature")) { home_open_kind(ORB_HOME_TEMPERATURE); return; }
    if (!strcmp(scene, "humidity")) { home_open_kind(ORB_HOME_HUMIDITY); return; }
    if (!strcmp(scene, "motion") || !strcmp(scene, "low-battery")) { home_open_kind(ORB_HOME_MOTION); return; }
    if (!strcmp(scene, "contact")) { home_open_kind(ORB_HOME_CONTACT); return; }
    if (!strcmp(scene,"long-scene") || !strcmp(scene,"wide-scene")) {
        orb_home_snapshot_t *snapshot = calloc(1,sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "copy long scene fixture");
        snprintf(snapshot->scenes[0].name,sizeof(snapshot->scenes[0].name),"%s",
            !strcmp(scene,"wide-scene") ? "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW" : "Evening Reading with Warm Lights");
        home_check(orb_home_snapshot_chunk(snapshot->rooms,snapshot->room_count,
            snapshot->accessories,snapshot->accessory_count,snapshot->scenes,
            snapshot->scene_count,true,false), "publish maximum-byte scene name");
        free(snapshot);
        orb_home_view_open(s_home.view,"floor-lamp"); home_advance(800);
        orb_home_view_detail_page(s_home.view,2); return;
    }
    if (!strcmp(scene, "error") || !strcmp(scene,"long-error") || !strcmp(scene,"wide-error")) {
        asynchronous_fixture();
        if (strcmp(scene,"error")) {
            orb_home_snapshot_t *snapshot = calloc(1,sizeof(*snapshot));
            home_check(snapshot && orb_home_snapshot(snapshot), "copy maximum-byte failure name");
            snprintf(snapshot->accessories[0].name,sizeof(snapshot->accessories[0].name),"%s",
                !strcmp(scene,"wide-error") ? "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW" : "Living Room Evening Reading Lamp");
            home_check(orb_home_snapshot_chunk(snapshot->rooms,snapshot->room_count,
                snapshot->accessories,snapshot->accessory_count,snapshot->scenes,
                snapshot->scene_count,true,false), "publish maximum-byte accessory name");
            free(snapshot);
        }
        orb_home_view_open(s_home.view, "floor-lamp");
        home_check(orb_home_toggle("floor-lamp"), "optimistic synthetic write");
        orb_home_request_t request;
        home_check(orb_home_take_request(&request), "take synthetic write");
        orb_home_complete(&request, NULL, "unreachable");
        return;
    }
    fprintf(stderr, "unknown Home scene: %s\n", scene);
    exit(2);
}

static void sequences(const char *directory)
{
    /* HIG §§6/8: actual 60 Hz pointer samples, including a bounded overshoot
     * beyond the arc end. Frames preserve the renderer's retained pixels. */
    orb_home_view_open(s_home.view, "floor-lamp");
    home_advance(800);
    save(directory, "rim", 0);
    for (unsigned i = 0; i <= 15; ++i) {
        float angle = (270.f + 10.f * i) * (float)M_PI / 180.f;
        home_press((int)lroundf(233 + 210 * cosf(angle)), (int)lroundf(233 + 210 * sinf(angle)));
        home_advance(16);
        save(directory, "rim", i + 1);
    }
    home_release();
    const unsigned delays[] = {16, 48, 96, 176, 320, 600, 800};
    unsigned previous = 0;
    for (unsigned i = 0; i < sizeof(delays) / sizeof(*delays); ++i) {
        home_advance(delays[i] - previous);
        previous = delays[i];
        save(directory, "rim-settle", i);
    }

    orb_home_view_open(s_home.view, "kitchen");
    home_advance(800);
    save(directory, "toggle", 0);
    home_press(233, 140);
    home_advance(16);
    save(directory, "toggle", 1);
    home_release();
    previous = 0;
    for (unsigned i = 0; i < sizeof(delays) / sizeof(*delays); ++i) {
        home_advance(delays[i] - previous);
        previous = delays[i];
        save(directory, "toggle", i + 2);
    }

    orb_home_view_open(s_home.view, "front-door");
    home_advance(800);
    save(directory, "hold-cancel", 0);
    home_press(233, 140);
    home_advance(600);
    save(directory, "hold-cancel", 1);
    home_release();
    home_advance(800);
    save(directory, "hold-cancel", 2);
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot != NULL && orb_home_snapshot(snapshot), "read lock after cancelled hold");
    const orb_home_accessory_t *lock = orb_home_find(snapshot, "front-door");
    home_check(lock && lock->state.locked, "short hold never unlocks");
    free(snapshot);
    home_press(233, 140);
    save(directory, "hold-unlock", 0);
    for (unsigned i = 1; i <= 8; ++i) {
        home_advance(128);
        save(directory, "hold-unlock", i);
    }
    home_release();
    home_advance(800);
    save(directory, "hold-unlock", 9);
    snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot != NULL && orb_home_snapshot(snapshot), "read lock after deliberate hold");
    lock = orb_home_find(snapshot, "front-door");
    home_check(lock && !lock->state.locked, "deliberate hold visibly unlocks");
    free(snapshot);

    asynchronous_fixture();
    orb_home_view_open(s_home.view, "floor-lamp");
    home_advance(800);
    save(directory, "revert", 0);
    home_tap(233, 395);
    home_advance(96);
    save(directory, "revert", 1);
    orb_home_request_t request;
    home_check(orb_home_take_request(&request), "toggle queues one Home write");
    orb_home_complete(&request, NULL, "unreachable");
    home_advance(16);
    save(directory, "revert", 2);
    home_advance(800);
    save(directory, "revert", 3);
    acknowledge();
}

static void ux_sequences(const char *directory)
{
    /* User-visible controls drive these receipts. API selection establishes
     * only the initial destination; each numbered transition is real input. */
    asynchronous_fixture();
    orb_home_view_select(s_home.view,ORB_HOME_ROOMS); home_advance(800);
    save(directory,"room-back",0);
    home_tap_text("Living Room"); save(directory,"room-back",1);
    scroll_room_tail(); save(directory,"room-back",2);
    int room_scroll = lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view));
    home_tap_text("Sensor"); save(directory,"room-back",3);
    orb_home_view_back(s_home.view); home_advance(800); save(directory,"room-back",4);
    home_check(home_has_text("Living Room") && home_has_text("Sensor") &&
        lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == room_scroll,
        "Back restores room scroll and the focused sensor");
    orb_home_view_back(s_home.view); home_advance(800); save(directory,"room-back",5);
    home_tap_text("Climate"); save(directory,"climate",0);
    if (home_find_action(orb_home_view_root(s_home.view),"Sensors")) {
        home_tap_text("Sensors"); save(directory,"climate-sensors",0);
        if (home_find_action(orb_home_view_root(s_home.view),"More Sensors")) {
            home_tap_text("More Sensors"); save(directory,"climate-sensors",1);
        }
        orb_home_view_back(s_home.view); home_advance(800); save(directory,"climate-sensors",2);
    }
    orb_home_view_back(s_home.view); home_advance(800);
    home_tap_text("Living Room"); home_tap_text("Floor Lamp");
    save(directory,"focus",0);
    orb_home_view_back(s_home.view); home_advance(800);
    orb_home_view_back(s_home.view); home_advance(800);
    home_tap_text("Scenes");
    home_advance(800); save(directory,"scenes",0);
    if (home_find_action(orb_home_view_root(s_home.view),"Evening")) {
        save(directory,"scenes",1);
    }
    const char *scene_names[] = {"Good morning","Evening","Good Night","I'm leaving","I'm home"};
    for (unsigned i = 0; i < sizeof(scene_names)/sizeof(*scene_names); ++i) {
        lv_obj_t *action = home_find_action(orb_home_view_root(s_home.view),scene_names[i]);
        if (!action) continue;
        lv_area_t bounds; lv_obj_get_coords(action,&bounds);
        home_tap((bounds.x1+bounds.x2+1)/2,(bounds.y1+bounds.y2+1)/2);
        home_advance(16); save(directory,"scenes",2);
        home_check(home_has_text("Running") || home_has_text("Requesting"), "scene remains in flight before authority accepts it");
        acknowledge(); home_advance(800); save(directory,"scenes",3);
        home_check(home_has_text("Requested"), "accepted scene shows the truthful request receipt");
        break;
    }
    orb_home_view_open(s_home.view,"dining"); home_advance(800);
    orb_home_view_detail_page(s_home.view,1); home_advance(800);
    save(directory,"white",0);
    home_adjust_step(ORB_ICON_PLUS); home_advance(16); save(directory,"white",1);
    acknowledge(); home_advance(800); save(directory,"white",2);
    home_adjust_step(ORB_ICON_MINUS); home_advance(16); save(directory,"white",3);
    acknowledge(); home_advance(800); save(directory,"white",4);
}

static unsigned object_count(lv_obj_t *object)
{
    unsigned count = 1;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++)
        count += object_count(lv_obj_get_child(object, (int32_t)i));
    return count;
}

static void allocation_receipt(bool steps)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    size_t live = monitor.total_size;
    printf("Home retained: objects=%u live_bytes=%zu blocks=%zu peak_bytes=%zu\n",
        object_count(orb_home_view_root(s_home.view)), live, monitor.used_cnt, monitor.max_used);
    if (steps) {
        size_t before = home_memory_allocations();
        for (unsigned i = 0; i < 80; ++i) {
            home_check(orb_home_update("light-0", &(orb_home_state_t){.present = ORB_HOME_ON,
                .on = i % 2 != 0}, false, true, 0), "publish synthetic room state update");
            orb_home_view_poll(s_home.view);
        }
        size_t allocations = home_memory_allocations() - before;
        printf("Home room list: updates=80 synchronous_allocations=%zu\n", allocations);
        home_check(allocations == 0, "retained room state updates allocate no LVGL payload");
        before = home_memory_allocations();
        home_advance(32);
        printf("Home redraw: renderer_allocations=%zu\n", home_memory_allocations() - before);
    }
    lv_mem_monitor(&monitor); live = monitor.total_size;
    orb_home_view_destroy(s_home.view); s_home.view = NULL;
    lv_mem_monitor(&monitor);
    printf("Home retained: view_owned_bytes=%zu remainder=%zu\n", live - monitor.total_size, monitor.total_size);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: orb_home_sim SCENE OUTPUT.png [default|larger|ax3] [bold] [rt] [rm]\n"
                        "       orb_home_sim --sequences EXISTING_DIRECTORY [flags]\n"
                        "       orb_home_sim --ux-sequences EXISTING_DIRECTORY [flags]\n");
        return 2;
    }
    home_init(argc, argv, 3);
    if (!strcmp(argv[1], "--sequences")) sequences(argv[2]);
    else if (!strcmp(argv[1],"--ux-sequences")) ux_sequences(argv[2]);
    else {
        select_scene(argv[1]);
        home_advance(1000);
        if (!strcmp(argv[1],"wide-scene"))
            verify_complete_name("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
        else if (!strcmp(argv[1],"long-scene")) verify_complete_name("Evening Reading with Warm Lights");
        else if (!strcmp(argv[1], "design2-rooms-wide"))
            verify_complete_name("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
        else if (!strcmp(argv[1], "design2-rooms-long"))
            verify_complete_name("Reading Room & Family Library");
        if (!strcmp(argv[1], "busy-room") || !strncmp(argv[1], "busy-room-", 10)) {
            lv_obj_t *title = home_find_rim_title(orb_home_view_root(s_home.view), "Upstairs");
            lv_obj_t *list = orb_home_view_scroll(s_home.view);
            lv_area_t title_bounds, list_bounds;
            home_check(title && list, "busy-room capture contains its rim title and native list");
            lv_obj_get_coords(title, &title_bounds); lv_obj_get_coords(list, &list_bounds);
            home_check(title_bounds.y2 + 12 < list_bounds.y1,
                "busy-room first/middle/last capture preserves the full title above the viewport");
        }
        /* Failure keeps the accessory's normal contextual name. Long names
         * use the same ellipsis policy as any other dial, with a short receipt. */
        verify_frame();
        home_check(sim_display_save_png(argv[2]), "save Home PNG");
    }
    allocation_receipt(!strcmp(argv[1], "rooms-list-top"));
    printf("Home simulator: %u checks passed\n", s_home.checks);
    home_shutdown();
    return 0;
}
