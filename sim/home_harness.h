#pragma once

/* Deterministic pointer/renderer harness. All Home data is synthetic and all
 * writes terminate in the local model; this executable never opens a link. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_home_view.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "src/widgets/arclabel/lv_arclabel_private.h"
#include <ctype.h>
#include "orb_input.h"
#include "sim_display.h"
#include "link_home.h"

typedef struct {
    uint32_t now;
    lv_display_t *display;
    lv_indev_t *pointer;
    lv_indev_data_t input;
    orb_home_view_t *view;
    unsigned checks, frames, damage_events;
    uint32_t largest_damage, flushed_pixels, largest_frame;
    bool measure;
} home_harness_t;

static home_harness_t s_home;

static inline uint32_t home_tick(void) { return s_home.now; }

static inline void home_check(bool value, const char *why)
{
    ++s_home.checks;
    if (value) return;
    fprintf(stderr, "Home: %s (%u ms)\n", why, s_home.now);
    const char *capture = getenv("ORB_HOME_TEST_FAILURE");
    if (capture && s_home.display) sim_display_save_png(capture);
    exit(EXIT_FAILURE);
}

static inline void home_pointer_read(lv_indev_t *pointer, lv_indev_data_t *data)
{
    (void)pointer;
    *data = s_home.input;
}

static inline void home_display_event(lv_event_t *event)
{
    if (!s_home.measure) return;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_INVALIDATE_AREA: {
        const lv_area_t *area = lv_event_get_param(event);
        uint32_t pixels = lv_area_get_size(area);
        if (pixels > s_home.largest_damage) s_home.largest_damage = pixels;
        ++s_home.damage_events;
        break;
    }
    case LV_EVENT_RENDER_START: s_home.flushed_pixels = 0; break;
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *area = lv_event_get_param(event);
        s_home.flushed_pixels += lv_area_get_size(area);
        break;
    }
    case LV_EVENT_REFR_READY:
        if (s_home.flushed_pixels) {
            ++s_home.frames;
            if (s_home.flushed_pixels > s_home.largest_frame)
                s_home.largest_frame = s_home.flushed_pixels;
        }
        s_home.flushed_pixels = 0;
        break;
    default: break;
    }
}

static inline void home_advance(unsigned milliseconds)
{
    unsigned end = s_home.now + milliseconds;
    do {
        unsigned step = end - s_home.now;
        if (step > 8) step = 8;
        s_home.now += step;
        orb_home_view_poll(s_home.view);
        lv_timer_handler();
        lv_refr_now(s_home.display);
    } while (s_home.now < end);
}

static inline void home_press(int x, int y)
{
    s_home.input.point = (lv_point_t){x, y};
    s_home.input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_home.pointer);
    orb_home_view_poll(s_home.view);
    lv_refr_now(s_home.display);
}

static inline void home_release(void)
{
    s_home.input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_home.pointer);
    orb_home_view_poll(s_home.view);
    lv_refr_now(s_home.display);
}

static inline void home_tap(int x, int y)
{
    home_press(x, y);
    home_advance(32);
    home_release();
}

static inline lv_obj_t *home_find_text(lv_obj_t *object, const char *text)
{
    if (!lv_obj_is_visible(object)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) &&
        lv_obj_get_style_text_opa(object, 0) != LV_OPA_TRANSP &&
        strstr(lv_label_get_text(object), text)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = home_find_text(lv_obj_get_child(object, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

/* Test-only introspection: LVGL exposes arc geometry but no text getter.
 * Keep ordinary-label lookup separate so existing text-fit checks never pass
 * an arclabel to lv_label_get_text(). */
static inline bool home_arc_contains(lv_obj_t *object, const char *text)
{
    if (!lv_obj_is_visible(object)) return false;
    if (lv_obj_check_type(object, &lv_arclabel_class)) {
        const char *actual = ((lv_arclabel_t *)object)->text;
        char caps[128];
        size_t n = strlen(text);
        if (n >= sizeof(caps)) return false;
        for (size_t i = 0; i <= n; ++i) caps[i] = (char)toupper((unsigned char)text[i]);
        if (actual && strstr(actual, caps)) return true;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (home_arc_contains(lv_obj_get_child(object, (int32_t)i), text)) return true;
    return false;
}

static inline lv_obj_t *home_find_rim_title(lv_obj_t *object, const char *text)
{
    orb_kit_rim_text_info_t info;
    if (lv_obj_is_visible(object) && orb_kit_rim_text_info(object, &info) && home_arc_contains(object, text))
        return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = home_find_rim_title(lv_obj_get_child(object, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

static inline lv_obj_t *home_find_id(lv_obj_t *object, const char *id)
{
    const char *actual = lv_obj_get_user_data(object);
    if ((uintptr_t)actual > 4096 && !strcmp(actual, id)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = home_find_id(lv_obj_get_child(object, (int32_t)i), id);
        if (found) return found;
    }
    return NULL;
}

static inline bool home_has_text(const char *text)
{
    lv_obj_t *root = orb_home_view_root(s_home.view);
    return home_find_text(root, text) != NULL || home_arc_contains(root, text);
}

static inline lv_obj_t *home_find_action_match(lv_obj_t *object, const char *text, bool prefix)
{
    if (!lv_obj_is_visible(object)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) &&
        (prefix ? !strncmp(lv_label_get_text(object),text,strlen(text)) :
                  strstr(lv_label_get_text(object),text) != NULL)) {
        if (lv_obj_is_clickable(object)) return object;
        lv_obj_t *parent = lv_obj_get_parent(object);
        if (parent && lv_obj_is_clickable(parent) &&
            lv_obj_get_height(parent) < SIM_D) return parent;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = home_find_action_match(lv_obj_get_child(object, (int32_t)i), text, prefix);
        if (found) return found;
    }
    return NULL;
}

static inline lv_obj_t *home_find_action(lv_obj_t *object, const char *text)
{
    lv_obj_t *found = home_find_action_match(object,text,true);
    if (!found) found = home_find_action_match(object,text,false);
    if (!found) {
        lv_obj_t *title = home_find_rim_title(object, text);
        if (title && lv_obj_is_clickable(title)) found = title;
    }
    return found;
}

static inline void home_adjust_step(const char *direction)
{
    /* Exercise the same visible-content rotary owner used by physical twist;
     * Design 2.0 has no hidden +/- controls to invoke. */
    float value, step;
    home_check(orb_home_view_dial(s_home.view, &value, &step), "visible detail owns its rotary value");
    orb_home_view_rotate(s_home.view, value + (!strcmp(direction, ORB_ICON_MINUS) ? -step : step));
    orb_home_view_rotary_finish(s_home.view, true);
    home_advance(16);
}

static inline void home_long_room_fixture(void)
{
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "copy long room names fixture");
    for (unsigned i = 0; i < snapshot->accessory_count; ++i) {
        orb_home_accessory_t *a = &snapshot->accessories[i];
        if (!strcmp(a->room, "Living Room"))
            snprintf(a->name, sizeof(a->name), "Living Room %u WWWWWWWWWWWWWWWWW", i);
    }
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
        snapshot->scene_count, true, false), "install long room names fixture");
    free(snapshot);
    orb_home_view_room(s_home.view, "Living Room");
    home_advance(800);
}

/* A single busy room exercises continuation without importing the owner's
 * Home. Prefixes stay unique even when the 32-byte names need ellipses. */
static inline void home_busy_room_fixture(void)
{
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot != NULL, "allocate busy-room fixture");
    snapshot->room_count = 2;
    snprintf(snapshot->rooms[0].id, sizeof(snapshot->rooms[0].id), "busy");
    snprintf(snapshot->rooms[0].name, sizeof(snapshot->rooms[0].name), "Upstairs Reading & Family Lounge");
    snprintf(snapshot->rooms[1].id, sizeof(snapshot->rooms[1].id), "empty");
    snprintf(snapshot->rooms[1].name, sizeof(snapshot->rooms[1].name), "Spare Room");
    snapshot->accessory_count = 24;
    for (unsigned i = 0; i < snapshot->accessory_count; ++i) {
        orb_home_accessory_t *a = &snapshot->accessories[i];
        snprintf(a->id, sizeof(a->id), "busy-%02u", i + 1);
        snprintf(a->room, sizeof(a->room), "busy");
        switch (i % 4) {
        case 0: snprintf(a->name, sizeof(a->name), "D%02u WWWWWWWWWWWWWWWWWWWWWWWWWWWW", i + 1); break;
        case 1: snprintf(a->name, sizeof(a->name), "D%02u Reading / Task Lamp (Left)", i + 1); break;
        case 2: snprintf(a->name, sizeof(a->name), "D%02u Lampe été – fenêtre", i + 1); break;
        default: snprintf(a->name, sizeof(a->name), "D%02u #Guest's Lamp: 100%%", i + 1); break;
        }
        a->kind = i < 20 ? ORB_HOME_LIGHT : i < 22 ? ORB_HOME_SWITCH :
            i == 22 ? ORB_HOME_LOCK : ORB_HOME_TEMPERATURE;
        a->reachable = true;
        if (i < 22) {
            a->writable = ORB_HOME_ON | (i < 20 ? ORB_HOME_BRIGHTNESS : 0);
            a->state = (orb_home_state_t){.present = a->writable, .on = true, .brightness = 70};
        } else if (i == 22) {
            a->writable = ORB_HOME_LOCKED;
            a->state = (orb_home_state_t){.present = ORB_HOME_LOCKED, .locked = true};
        } else a->state = (orb_home_state_t){.present = ORB_HOME_TEMP, .temp = 21.5f};
    }
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, NULL, 0, true, false),
        "publish 24-device room without favourites or scenes");
    free(snapshot);
    orb_home_set_connection(true, true);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS);
    home_advance(800);
}

/* Preserve tooling navigation without a redundant on-screen title control. */
static inline void home_tap_text(const char *text)
{
    /* Safe semantic navigation complements the real vertical/rim coverage.
     * These alternatives intentionally have no visible paging control. */
    if (!strcmp(text, "More Devices") || !strcmp(text, "Devices")) {
        lv_obj_t *list = orb_home_view_scroll(s_home.view);
        home_check(list != NULL, "room navigation exposes its native scroll owner");
        lv_obj_scroll_by(list, 0, -lv_obj_get_height(list) / 2, LV_ANIM_OFF);
        home_advance(800);
        return;
    }
    lv_obj_t *action = home_find_action(orb_home_view_root(s_home.view), text);
    if (!action) {
        orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "resolve retained named room row");
        for (unsigned i = 0; i < snapshot->room_count && !action; ++i) {
            if (strcmp(snapshot->rooms[i].name, text)) continue;
            char id[64]; snprintf(id, sizeof(id), "room:%s", snapshot->rooms[i].id);
            action = home_find_id(orb_home_view_root(s_home.view), id);
        }
        free(snapshot);
    }
    if (!action && !strcmp(text, "Scenes")) action = home_find_id(orb_home_view_root(s_home.view), "scenes:open");
    if (!action && !strcmp(text, "Climate")) action = home_find_id(orb_home_view_root(s_home.view), "home:climate");
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    if (!action && list) {
        /* Native lists retain offscreen rows. Locate their canonical label,
         * then scroll before tapping; viewport clipping is not disappearance. */
        for (unsigned pass = 0; pass < 2 && !action; ++pass) {
            for (unsigned i = 0; i < lv_obj_get_child_count(list) && !action; ++i) {
                lv_obj_t *row = lv_obj_get_child(list, i);
                if (!lv_obj_is_clickable(row) || lv_obj_is_hidden(row)) continue;
                for (unsigned j = 0; j < lv_obj_get_child_count(row); ++j) {
                    lv_obj_t *label = lv_obj_get_child(row, j);
                    if (!lv_obj_check_type(label, &lv_label_class)) continue;
                    const char *caption = lv_label_get_text(label);
                    if (pass ? strstr(caption, text) != NULL :
                               !strncmp(caption, text, strlen(text))) { action = row; break; }
                }
            }
        }
    }
    if (!action) fprintf(stderr, "Home action missing: %s\n", text);
    home_check(action != NULL, "named visible control exists");
    lv_area_t bounds;
    if (list && lv_obj_get_parent(action) == list) {
        /* A round row can be visible while its logical centre lies beyond the
         * viewport (especially AX3). Bring the named target into the interior
         * before issuing a real tap, just as the owner scrolls to a row. */
        lv_area_t viewport;
        lv_obj_get_coords(action, &bounds); lv_obj_get_coords(list, &viewport);
        int maximum = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
        int position = lv_obj_get_scroll_y(list) +
            (bounds.y1 + bounds.y2 - viewport.y1 - viewport.y2) / 2;
        lv_obj_scroll_to_y(list, LV_CLAMP(0, position, maximum), LV_ANIM_OFF);
        home_advance(800);
    }
    lv_obj_get_coords(action, &bounds);
    (void)orb_round_list_child_bounds(action, action, &bounds);
    orb_kit_rim_text_info_t rim_info;
    if (orb_kit_rim_text_info(action, &rim_info)) {
        /* Tap the actual top title ink. A curved title's rectangular cache
         * reserve can include empty space already occupied by list content. */
        home_tap(233, 233 - ORB_KIT_TITLE_RADIUS - 8);
    } else home_tap((bounds.x1 + bounds.x2 + 1) / 2, (bounds.y1 + bounds.y2 + 1) / 2);
    home_advance(800);
}

static inline const orb_home_accessory_t *home_fixture_kind(orb_home_kind_t kind,
                                                          orb_home_snapshot_t *snapshot)
{
    home_check(orb_home_snapshot(snapshot), "copy Home fixture");
    for (unsigned i = 0; i < snapshot->accessory_count; ++i)
        if (snapshot->accessories[i].kind == kind) return &snapshot->accessories[i];
    home_check(false, "fixture is missing an accessory kind");
    return NULL;
}

static inline void home_open_kind(orb_home_kind_t kind)
{
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot != NULL, "allocate host fixture scratch");
    const orb_home_accessory_t *accessory = home_fixture_kind(kind, snapshot);
    orb_home_view_open(s_home.view, accessory->id);
    free(snapshot);
    home_advance(800);
}

static inline void home_measure_start(void)
{
    s_home.frames = s_home.damage_events = 0;
    s_home.largest_damage = s_home.flushed_pixels = s_home.largest_frame = 0;
    (void)sim_display_take_changed();
    s_home.measure = true;
}

static inline void home_measure_stop(void) { s_home.measure = false; }

static inline void home_wire_fixture(void)
{
    FILE *file = fopen(HOME_FIXTURE_DIR "/snapshot.jsonl", "r");
    home_check(file != NULL, "open hub-generated real-shaped Home fixture");
    orb_home_reset();
    link_home_reset();
    char line[16386];
    while (fgets(line, sizeof(line), file)) {
        cJSON *root = cJSON_Parse(line);
        home_check(root && link_home_event("home.snapshot",
            cJSON_GetObjectItemCaseSensitive(root, "p")) == LINK_V11_APPLIED,
            "consume exact hub snapshot bytes through firmware parser");
        cJSON_Delete(root);
    }
    home_check(!ferror(file), "read complete Home fixture");
    fclose(file);
    orb_home_set_connection(true, true);
}

static inline void home_init(int argc, char **argv, int flag_start)
{
    lv_init();
    lv_tick_set_cb(home_tick);
    s_home.display = sim_display_create();
    home_check(s_home.display != NULL, "create round display");
    orb_ui_init(NULL);
    home_check(orb_model_init(), "initialize preferences model");
    home_check(orb_home_init(), "initialize Home model");
    orb_home_load_demo();
    orb_home_set_connection(true, true);
    for (int i = flag_start; i < argc; ++i) {
        if (!strcmp(argv[i], "fahrenheit")) {
            orb_prefs_t prefs;
            orb_model_prefs_defaults(&prefs);
            prefs.temperature = ORB_TEMPERATURE_F;
            home_check(orb_model_set_prefs(&prefs), "Fahrenheit preference");
        } else if (!strcmp(argv[i], "wire")) {
            home_wire_fixture();
        } else if (!strcmp(argv[i], "default")) {
            home_check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "default type");
        } else if (!strcmp(argv[i], "larger")) {
            home_check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_LARGER) == ORB_FONTS_OK, "Larger type");
        } else if (!strcmp(argv[i], "ax3")) {
            home_check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "AX3 type");
        } else if (!strcmp(argv[i], "bold")) {
            home_check(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "Bold Text");
        } else if (!strcmp(argv[i], "rt") || !strcmp(argv[i], "opaque")) {
            orb_ui_set_reduce_transparency(true);
        } else if (!strcmp(argv[i], "rm")) {
            orb_ui_set_reduce_motion(true);
        } else {
            fprintf(stderr, "unknown flag: %s\n", argv[i]);
            exit(2);
        }
    }
    s_home.pointer = lv_indev_create();
    home_check(s_home.pointer != NULL, "create pointer");
    lv_indev_set_type(s_home.pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_home.pointer, s_home.display);
    lv_indev_set_read_cb(s_home.pointer, home_pointer_read);
    s_home.view = orb_home_view_create(lv_screen_active(), NULL);
    home_check(s_home.view != NULL, "create Home view");
    lv_display_add_event_cb(s_home.display, home_display_event, LV_EVENT_ALL, NULL);
    home_advance(800);
}

static inline void home_shutdown(void)
{
    orb_home_view_destroy(s_home.view);
    lv_indev_delete(s_home.pointer);
    lv_deinit();
}
