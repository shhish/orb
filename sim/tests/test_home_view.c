/* Security and retained-rendering contracts exercised through LVGL's real
 * pointer dispatch, with the same model/view and banded renderer as firmware. */
#include "../home_harness.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/core/lv_obj_raster_private.h"
#include <time.h>

/* Every accessory template's power/mode/lock control is its identity disc
 * at (233,140), independent of which font identity glyph it contains. */
/* A fresh list fills the circle from under the title: the first row rests
 * on a shoulder, not centred below an empty band. */
static bool landing_at_rest(orb_home_view_t *view)
{
    orb_home_view_state_t state;
    orb_home_view_save(view, &state);
    lv_obj_t *list = orb_home_view_scroll(view);
    return list && lv_obj_get_scroll_y(list) > 0 && state.rooms_scroll == 0;
}

static lv_obj_t *identity_disc(void)
{
    lv_obj_t *disc = lv_indev_search_obj(orb_home_view_root(s_home.view), &(lv_point_t){233, 140});
    while (disc && !lv_obj_is_clickable(disc)) disc = lv_obj_get_parent(disc);
    return disc;
}

static uint8_t s_partial[SIM_D * SIM_D * 3], s_full[sizeof(s_partial)];

/* Counts every write the presenter decides to send through its host port. */
static unsigned s_page_commands;
static bool page_command(void *ctx, const orb_home_command_t *command)
{ (void)ctx; ++s_page_commands; return orb_home_command_execute(command); }


static lv_obj_t *semantic_action(lv_obj_t *root, const char *id)
{
    const char *data = lv_obj_get_user_data(root);
    if ((uintptr_t)data > 4096 && !strcmp(data, id)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = semantic_action(lv_obj_get_child(root, i), id);
        if (found) return found;
    }
    return NULL;
}


static void complete_text(const char *text)
{
    lv_obj_t *label = home_find_text(orb_home_view_root(s_home.view),text);
    if (!label) {
        lv_obj_t *title = home_find_rim_title(orb_home_view_root(s_home.view), text);
        orb_kit_rim_text_info_t info;
        home_check(title && orb_kit_rim_text_info(title, &info) && !info.ellipsised &&
            info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES,
            "complete identity fits the kit rim without truncation");
        return;
    }
    lv_point_t required;
    lv_text_get_size(&required,lv_label_get_text(label),lv_obj_get_style_text_font(label,0),
        lv_obj_get_style_text_letter_space(label,0),lv_obj_get_style_text_line_space(label,0),
        lv_obj_get_width(label),LV_TEXT_FLAG_NONE);
    home_check(required.y <= lv_obj_get_height(label), "text retains every wrapped line at the current size");
}


static void frozen_frame(const char *surface)
{
    lv_refr_now(s_home.display);
    sim_display_rgb(s_partial);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_home.display);
    sim_display_rgb(s_full);
    if (memcmp(s_partial, s_full, sizeof(s_full))) {
        int x1 = SIM_D, y1 = SIM_D, x2 = -1, y2 = -1;
        unsigned pixels = 0;
        for (int y = 0; y < SIM_D; y++) for (int x = 0; x < SIM_D; x++) {
            unsigned at = (unsigned)(y * SIM_D + x) * 3;
            if (!memcmp(s_partial + at, s_full + at, 3)) continue;
            x1 = LV_MIN(x1, x); y1 = LV_MIN(y1, y);
            x2 = LV_MAX(x2, x); y2 = LV_MAX(y2, y); pixels++;
        }
        fprintf(stderr, "partial/full mismatch: %s, %u pixels in %d,%d..%d,%d\n",
            surface, pixels, x1, y1, x2, y2);
        home_check(false, "retained pixels differ from full frozen redraw");
    }
    home_check(true, "partial/full pixels match");
}

static void quiet(const char *surface)
{
    home_advance(1600);
    home_measure_start();
    home_advance(2000);
    home_measure_stop();
    printf("Home idle %-18s frames=%u invalidations=%u\n", surface, s_home.frames, s_home.damage_events);
    home_check(s_home.frames == 0, "idle Home renders zero frames");
    home_check(s_home.damage_events == 0, "idle Home invalidates nothing");
    frozen_frame(surface);
}

static void authoritative_fixture(void)
{
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot != NULL && orb_home_snapshot(snapshot), "copy Home fixture");
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
        snapshot->scene_count, true, false), "install non-demo authoritative fixture");
    free(snapshot);
    orb_home_set_connection(true, true);
    home_advance(800);
}

static void round_list_metrics(lv_obj_t *list);

static void settled_screens(void)
{
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS);
    quiet("Rooms");
    home_check(home_has_text("Living Room"), "Rooms shows real room names");
    lv_obj_t *name = home_find_text(orb_home_view_root(s_home.view), "Living Room");
    home_check(name && lv_obj_get_width(name) > 100, "room name has a visible line box");
    orb_home_view_select(s_home.view, ORB_HOME_CLIMATE);
    quiet("Climate");
    lv_obj_t *temperature = home_find_text(orb_home_view_root(s_home.view), "21°");
    if (!temperature) temperature = home_find_text(orb_home_view_root(s_home.view), "22°");
    home_check(temperature && lv_obj_get_width(temperature) > 60,
               "Climate's current temperature has a visible line box");
    const char *details[] = {"floor-lamp", "dining", "climate", "front-door", "kitchen",
        "coffee", "bed-temp", "kitchen-temp", "hall-motion", "window", "bookshelf", "fan"};
    for (unsigned i = 0; i < sizeof(details) / sizeof(*details); ++i) {
        orb_home_view_open(s_home.view, details[i]);
        quiet(details[i]);
    }
    orb_home_view_open(s_home.view, "floor-lamp");
    home_advance(800);
    orb_home_view_detail_page(s_home.view, 1);
    quiet("Light colour");
    home_check(home_has_text("Brightness 62%") || home_has_text("Brightness"), "Light colour page is actually selected");
    orb_home_view_detail_page(s_home.view, 2);
    quiet("Light scenes");
    home_check(home_has_text("Good morning") && home_has_text("Evening"),
               "Light scene page lists its scenes instead of an on-screen pager");
    orb_home_view_open(s_home.view, "climate");
    home_advance(800);
    orb_home_view_detail_page(s_home.view, 1);
    quiet("Thermostat modes");
    home_check(home_has_text(ORB_ICON_CHECK) && home_has_text("Heat") && home_has_text("Auto"),
               "selected thermostat mode has a noncolour check and Auto is listed");
    lv_obj_t *modes = orb_home_view_scroll(s_home.view);
    home_check(modes != NULL, "mode picker exposes its native list to the rim");
    round_list_metrics(modes);
    orb_home_view_open(s_home.view, "hall-motion");
    home_advance(800);
    home_check(home_has_text("Low battery"), "low battery has words as well as colour");
    orb_home_view_open(s_home.view, "kitchen-temp");
    home_advance(800);
    home_check(home_has_text("49%"), "humidity sensor presents humidity as its primary value");
    home_check(!home_has_text("Last change unavailable"), "missing sensor history is omitted");
    orb_home_view_open(s_home.view, "bookshelf");
    home_advance(800);
    home_check(home_has_text("No response"), "unreachable uses Apple's No Response wording");
}

static void round_list_metrics(lv_obj_t *list)
{
    home_check(list && lv_obj_get_child_count(list) > 0, "Home exposes its populated native round list");
    lv_obj_update_layout(list);
    int origin = lv_obj_get_y(lv_obj_get_child(list, 0));
    int next = origin;
    for (unsigned i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_obj_t *row = lv_obj_get_child(list, i);
        if (lv_obj_is_floating(row)) continue;
        /* Dynamic Type may grow rows while retaining the kit's 12 px gap. */
        home_check(lv_obj_get_width(row) == 418 && lv_obj_get_height(row) >= ORB_KIT_ROW_H &&
            lv_obj_get_y(row) == next,
            "Home uses the kit's minimum 72-pixel rows and 12-pixel gaps at every content size");
        next = lv_obj_get_y(row) + lv_obj_get_height(row) + 12;
        lv_obj_t *disc = lv_obj_get_child(row, 0);
        home_check(disc && lv_obj_get_width(disc) == ORB_KIT_DISC_ROW &&
            lv_obj_get_height(disc) == ORB_KIT_DISC_ROW,
            "every kit row has its shared 40-pixel leading disc");
    }
}

static bool rooms_destination(void)
{
    orb_home_view_state_t state;
    orb_home_view_save(s_home.view, &state);
    return state.surface == ORB_HOME_ROOMS && !state.selected[0] &&
        !state.sections && !state.sensors && !state.scene_page &&
        orb_home_view_scroll(s_home.view) != NULL;
}

static void rooms_entry(void)
{
    orb_home_view_destroy(s_home.view);
    s_home.view = orb_home_view_create(lv_screen_active(), NULL);
    home_check(s_home.view != NULL, "fresh Home entry");
    home_advance(800);
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    home_check(rooms_destination() && list && landing_at_rest(s_home.view),
        "Home opens the named room list at its top");
    home_check(home_find_id(orb_home_view_root(s_home.view), "room:Living Room") &&
        home_find_id(orb_home_view_root(s_home.view), "room:Kitchen"),
        "every room is retained as a named reachable row");
    home_check(home_find_id(orb_home_view_root(s_home.view), "scenes:open") &&
        home_find_id(orb_home_view_root(s_home.view), "home:climate"),
        "contentful Climate and Scenes are discoverable rows");
    home_check(!orb_home_view_can_back(s_home.view), "Rooms is the top-level Home destination");
    lv_obj_t *title = home_find_rim_title(orb_home_view_root(s_home.view), "HOME");
    home_check(title && !lv_obj_is_clickable(title), "landing rim title never hides a section picker");
    home_tap(233, 233 - ORB_KIT_TITLE_RADIUS - 8); home_advance(100);
    home_check(rooms_destination(), "landing rim title tap leaves the room list in place");
    home_tap_text("Kitchen");
    home_check(home_has_text("Coffee Machine") && !home_has_text("Floor Lamp"),
        "one Kitchen row tap opens exactly Kitchen's devices");
    home_check(orb_home_view_back(s_home.view), "room Back returns to Rooms");
    home_advance(800);
    home_tap_text("Living Room");
    home_check(home_has_text("Floor Lamp"), "one Living Room row tap opens its device list");
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "room navigation never writes accessories");
    frozen_frame("Rooms entry and room navigation");
}

static lv_obj_t *row_live_text(lv_obj_t *object, const lv_area_t *row_bounds, const char *text)
{
    if (!lv_obj_is_visible(object)) return NULL;
    lv_obj_t *parent = lv_obj_get_parent(object);
    if (lv_obj_check_type(object, &lv_label_class) && parent && lv_obj_is_floating(parent) &&
        lv_obj_get_style_text_opa(object, 0) != LV_OPA_TRANSP && !strcmp(lv_label_get_text(object), text)) {
        lv_area_t clip; lv_obj_get_coords(parent, &clip);
        if (clip.y1 >= row_bounds->y1 && clip.y2 <= row_bounds->y2) return object;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = row_live_text(lv_obj_get_child(object, i), row_bounds, text);
        if (found) return found;
    }
    return NULL;
}

static unsigned content_row_count(lv_obj_t *list)
{
    unsigned count = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(list); ++i)
        count += !lv_obj_is_floating(lv_obj_get_child(list, i));
    return count;
}

static unsigned object_count(lv_obj_t *root)
{
    unsigned count = 1;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        count += object_count(lv_obj_get_child(root, i));
    return count;
}


static unsigned primary_actions(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return 0;
    unsigned count = orb_kit_is_primary(root);
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i)
        count += primary_actions(lv_obj_get_child(root, (int32_t)i));
    return count;
}


static void inset_title(lv_obj_t *title)
{
    orb_kit_rim_text_info_t info;
    home_check(title && orb_kit_rim_text_info(title, &info) &&
        info.angle_degrees <= 91,
        "accessory identity uses Home's ~90-degree rim title budget");
    for (unsigned slice = 0; slice < lv_obj_get_child_count(title); ++slice) {
        lv_obj_t *arc = lv_obj_get_child(lv_obj_get_child(title, (int32_t)slice), 0);
        home_check(arc && lv_obj_check_type(arc, &lv_arclabel_class) &&
            lv_arclabel_get_radius(arc) == ORB_KIT_TITLE_RADIUS,
            "every accessory title shares the kit's 200-pixel rim baseline");
    }
}

static void kit_action_hierarchy(void)
{
    /* The public role query counts tinted glyph controls as well as capsules;
     * fixed dimensions alone cannot establish the one-primary contract. */
    const char *ids[] = {"floor-lamp", "dining", "climate", "front-door", "kitchen", "coffee", "fan", "bed-temp"};
    const char *titles[] = {"Floor Lamp", "Dining Pendant", "Thermostat", NULL, NULL, NULL, "Bedroom Fan", NULL};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(*ids); ++i) {
        orb_home_view_open(s_home.view, ids[i]); home_advance(800);
        home_check(primary_actions(orb_home_view_root(s_home.view)) <= 1,
            "every accessory screen has at most one semantic primary action");
        if (titles[i]) inset_title(home_find_rim_title(orb_home_view_root(s_home.view), titles[i]));
        if (i < 2) {
            orb_home_view_detail_page(s_home.view, 1); home_advance(800);
            inset_title(home_find_rim_title(orb_home_view_root(s_home.view), titles[i]));
            home_check(primary_actions(orb_home_view_root(s_home.view)) <= 1,
                "colour dial has at most one semantic primary action");
        }
        if (i < 3) {
            orb_home_view_detail_page(s_home.view, i < 2 ? 2 : 1); home_advance(800);
            home_check(primary_actions(orb_home_view_root(s_home.view)) <= 1,
                "every scene/mode detail page has at most one semantic primary action");
        }
    }
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    home_check(primary_actions(orb_home_view_root(s_home.view)) <= 1,
        "Rooms has at most one semantic primary action");
    home_tap_text("Scenes");
    home_check(primary_actions(orb_home_view_root(s_home.view)) <= 1,
        "Scenes has at most one semantic primary action");
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    round_list_metrics(list);
    for (unsigned i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_obj_t *row = lv_obj_get_child(list, (int32_t)i);
        if (lv_obj_is_floating(row)) continue;
        lv_obj_t *name = lv_obj_get_child(row, 1);
        home_check(name && lv_obj_check_type(name, &lv_label_class) &&
            lv_label_get_max_lines(name) == 1 &&
            lv_obj_get_height(name) == lv_obj_get_style_text_font(name, 0)->line_height,
            "every scene row uses the kit's bounded identity lane");
    }
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
}


static lv_area_t text_required_bounds(lv_obj_t *label)
{
    lv_area_t bounds; lv_obj_get_coords(label, &bounds);
    lv_point_t required;
    lv_text_get_size(&required, lv_label_get_text(label), lv_obj_get_style_text_font(label, 0),
        lv_obj_get_style_text_letter_space(label, 0), lv_obj_get_style_text_line_space(label, 0),
        lv_obj_get_width(label), LV_TEXT_FLAG_NONE);
    if (required.x > lv_obj_get_width(label) || required.y > lv_obj_get_height(label))
        fprintf(stderr, "Home text fit: '%s' needs%d×%d box%d×%d mode%d\n", lv_label_get_text(label),
            required.x, required.y, lv_obj_get_width(label), lv_obj_get_height(label), lv_label_get_long_mode(label));
    home_check(required.x <= lv_obj_get_width(label) && required.y <= lv_obj_get_height(label),
        "complete room text fits its real-font line box");
    bounds.x1 += (lv_obj_get_width(label) - required.x) / 2;
    bounds.x2 = bounds.x1 + required.x - 1;
    bounds.y2 = bounds.y1 + required.y - 1;
    return bounds;
}


static void full_room_names(void)
{
    static const char *names[] = {"Dining", "Living Room", "Kitchen", "Bedroom", "Office", "Bathroom", "Garage",
        "Reading\nRoom", "Office\tWest\rFloor\x7F", "Reading Room & Family Library",
        "Living Room and Downstairs Study", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW",
        "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII", "Chambre été — bibliothèque", ""};
    orb_home_snapshot_t *original = calloc(1, sizeof(*original));
    orb_home_snapshot_t *fixture = calloc(1, sizeof(*fixture));
    home_check(original && fixture && orb_home_snapshot(original), "save Home before named row fixtures");
    fixture->room_count = sizeof(names) / sizeof(*names);
    for (unsigned i = 0; i < fixture->room_count; ++i) {
        snprintf(fixture->rooms[i].id, sizeof(fixture->rooms[i].id), "full-name-%u", i);
        snprintf(fixture->rooms[i].name, sizeof(fixture->rooms[i].name), "%s", names[i]);
    }
    fixture->accessory_count = 1;
    orb_home_accessory_t *sensor = &fixture->accessories[0];
    strcpy(sensor->id, "living-temp"); strcpy(sensor->room, "full-name-1");
    strcpy(sensor->name, "Temperature"); sensor->kind = ORB_HOME_TEMPERATURE; sensor->reachable = true;
    sensor->state = (orb_home_state_t){.present=ORB_HOME_TEMP, .temp=21.5f};
    home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count, fixture->accessories,
        fixture->accessory_count, NULL, 0, true, false),
        "publish realistic room names with control-character sanitisation and a temperature lane");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    unsigned objects = object_count(orb_home_view_root(s_home.view));
    for (unsigned i = 0; i < fixture->room_count; ++i) {
        char id[64]; snprintf(id, sizeof(id), "room:full-name-%u", i);
        lv_obj_t *row = home_find_id(orb_home_view_root(s_home.view), id);
        home_check(row && lv_obj_get_height(row) >= 72, "every room has its own safe row target");
        lv_obj_scroll_to_y(list, LV_MAX(0, lv_obj_get_y(row) -
            (lv_obj_get_height(list) - lv_obj_get_height(row)) / 2), LV_ANIM_OFF);
        home_advance(100);
        const char *display = i == 7 ? "Reading Room" : i == 8 ? "Office West Floor " : i == 14 ? "Unnamed room" : names[i];
        lv_obj_t *label = home_find_text(row, display);
        if (!label || strcmp(lv_label_get_text(label), display)) {
            lv_obj_t *title = lv_obj_get_child(row, 1);
            fprintf(stderr, "centred room name: expected='%s' actual='%s' row=%dx%d at y=%d scroll=%d\n",
                display, title && lv_obj_check_type(title, &lv_label_class) ? lv_label_get_text(title) : "missing",
                (int)lv_obj_get_width(row), (int)lv_obj_get_height(row), (int)lv_obj_get_y(row),
                (int)lv_obj_get_scroll_y(list));
        }
        home_check(label && !strcmp(lv_label_get_text(label), display),
            "centred named row preserves its complete sanitised room identity");
        (void)text_required_bounds(label);
        home_check(object_count(orb_home_view_root(s_home.view)) == objects,
            "scrolling names keeps all prepared objects");
        frozen_frame(display);
    }
    home_check(orb_home_snapshot_chunk(original->rooms, original->room_count,
        original->accessories, original->accessory_count, original->scenes,
        original->scene_count, true, false), "restore Home after named room fixture");
    orb_home_set_connection(true, true); home_advance(800);
    free(original); free(fixture);
}

static void context_clears_labels(lv_obj_t *object, lv_obj_t *context, const lv_area_t *bounds)
{
    if (!lv_obj_is_visible(object)) return;
    if (object != context && lv_obj_check_type(object, &lv_label_class) && lv_label_get_text(object)[0]) {
        lv_area_t other = text_required_bounds(object);
        home_check(bounds->y2 < other.y1 || other.y2 < bounds->y1 ||
            bounds->x2 < other.x1 || other.x2 < bounds->x1,
            "bounded dial context clears the value, power glyph and bottom action text");
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        context_clears_labels(lv_obj_get_child(object, (int32_t)i), context, bounds);
}

static void long_scene_and_detail_context(void)
{
    orb_home_snapshot_t *original = calloc(1, sizeof(*original));
    orb_home_snapshot_t *fixture = calloc(1, sizeof(*fixture));
    home_check(original && fixture && orb_home_snapshot(original), "save Home before long scene/context fixtures");
    *fixture = *original;
    for (unsigned i = 0; i < fixture->room_count; ++i)
        snprintf(fixture->rooms[i].name, sizeof(fixture->rooms[i].name), "Living Room and Downstairs Study");
    home_check(fixture->scene_count > 0, "long-scene fixture retains a safe scene");
    memset(fixture->scenes[0].name, 'W', ORB_HOME_NAME_MAX);
    fixture->scenes[0].name[ORB_HOME_NAME_MAX] = 0;
    home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count,
        fixture->accessories, fixture->accessory_count, fixture->scenes,
        fixture->scene_count, true, false), "publish maximum-byte scene and real room names");
    orb_home_view_destroy(s_home.view);
    s_home.view = orb_home_view_create(lv_screen_active(), NULL);
    home_check(s_home.view != NULL, "create long-scene initial viewport"); home_advance(800);
    home_tap_text("Scenes");
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    home_check(list && lv_obj_get_scroll_y(list) == 0, "long-scene entry starts at the initial scroll position");
    lv_obj_t *row = lv_obj_get_child(list, 0), *name = lv_obj_get_child(row, 1);
    lv_area_t title_bounds, row_bounds;
    home_check(lv_obj_is_visible(row) && lv_obj_raster_presentation_visible(row) &&
        orb_round_list_child_bounds(row, name, &title_bounds) &&
        orb_round_list_child_bounds(row, row, &row_bounds),
        "first maximum-byte scene paints its prepared title at initial scroll zero");
    lv_point_t tap = {(row_bounds.x1+row_bounds.x2+1)/2, (row_bounds.y1+row_bounds.y2+1)/2};
    home_check(orb_round_list_hit_test(row, &tap), "first long scene is an admitted kit hit target before scrolling");
    frozen_frame("first long scene initial viewport");
    home_tap(tap.x, tap.y); home_advance(800);
    orb_home_request_t request;
    home_check(orb_home_take_request(&request) && request.kind == ORB_HOME_REQUEST_SCENE &&
        !strcmp(request.id, fixture->scenes[0].id),
        "actual initial-position tap runs the first long scene without scrolling it into view");
    orb_home_complete(&request, NULL, NULL); home_advance(800);

    const char *ids[] = {"floor-lamp", "climate"};
    const char *contexts[] = {"Living", "Now"};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(*ids); ++i) {
        orb_home_view_open(s_home.view, ids[i]); home_advance(800);
        lv_obj_t *context = home_find_text(orb_home_view_root(s_home.view), contexts[i]);
        /* The template's context lane: Caption 2, at most two wrapped lines
         * (a long accessory name), fitted to its own chord. */
        lv_area_t cb; lv_obj_get_coords(context, &cb);
        home_check(context && lv_label_get_max_lines(context) == 2 &&
            lv_obj_get_style_text_font(context, 0) == orb_font_variant(ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR) &&
            lv_obj_get_height(context) <= 2 * lv_obj_get_style_text_font(context, 0)->line_height &&
            lv_obj_get_width(context) <= orb_chord_width(cb.y1, cb.y2, ORB_RIM_MARGIN_TEXT),
            "long room/current context retains one bounded Caption2 lane");
        lv_area_t bounds = text_required_bounds(context);
        context_clears_labels(orb_home_view_root(s_home.view), context, &bounds);
        home_check(primary_actions(orb_home_view_root(s_home.view)) <= 1,
            "long-context detail preserves the one-primary hierarchy");
        frozen_frame("long room dial context");
    }
    home_check(orb_home_snapshot_chunk(original->rooms, original->room_count,
        original->accessories, original->accessory_count, original->scenes,
        original->scene_count, true, false), "restore Home after long scene/context fixtures");
    orb_home_set_connection(true, true);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    free(original); free(fixture);
}

extern size_t sim_memory_allocation_calls(void);

static void landing_poll_without_allocations(const char *reason)
{
    size_t before = sim_memory_allocation_calls();
    orb_home_view_poll(s_home.view);
    size_t allocated = sim_memory_allocation_calls() - before;
    if (allocated) fprintf(stderr, "landing update allocated %zu times: %s\n", allocated, reason);
    home_check(!allocated, reason);
}

static void landing_return_positions(void)
{
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    int rest = lv_obj_get_scroll_y(list);
    home_check(rest > 0 && landing_at_rest(s_home.view), "fresh landing uses its nonzero resting offset");
    if (orb_fonts_content_size() == ORB_CONTENT_SIZE_DEFAULT) {
        lv_area_t viewport, first;
        lv_obj_get_coords(list, &viewport);
        lv_obj_t *row = lv_obj_get_child(list, 0);
        home_check(orb_round_list_row_bounds(row, &first) && first.y1 >= 104,
            "first resting room begins at or below y104");
        unsigned whole = 0;
        for (unsigned i = 0; i < lv_obj_get_child_count(list); i++) {
            row = lv_obj_get_child(list, i);
            lv_area_t bounds;
            if (!lv_obj_is_floating(row) && orb_round_list_row_bounds(row, &bounds) &&
                bounds.y1 >= viewport.y1 && bounds.y2 <= viewport.y2) whole++;
        }
        home_check(viewport.y1 == 66 && viewport.y2 == 439 && whole >= 3,
            "fresh default landing shows at least three whole rows in y66..440");
    }
    /* Revisit rest after rebuilding at another offset; its sentinel must not
     * depend on whether that particular page was originally fresh. */
    const int positions[] = {rest, rest + 47, rest, 0};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(*positions); i++) {
        list = orb_home_view_scroll(s_home.view);
        lv_obj_scroll_to_y(list, positions[i], LV_ANIM_OFF); home_advance(100);
        orb_home_view_state_t saved;
        orb_home_view_save(s_home.view, &saved);
        home_check(saved.scroll == positions[i] &&
            saved.rooms_scroll == (positions[i] == rest ? 0 : positions[i] == 0 ? -1 : positions[i]),
            "landing context distinguishes resting, scrolled and pulled-to-equator positions");
        orb_home_view_room(s_home.view, "room-0"); home_advance(800);
        home_check(orb_home_view_back(s_home.view), "room Back consumes one navigation level"); home_advance(800);
        home_check(lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == positions[i],
            "Back restores exact native landing scroll including rest and equator");
        orb_home_view_restore(s_home.view, &saved); home_advance(800);
        home_check(lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == positions[i],
            "serialized landing context restores its exact native scroll");
    }
    orb_home_view_restore(s_home.view, &(orb_home_view_state_t){.surface = ORB_HOME_ROOMS}); home_advance(800);
    home_check(lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == rest && landing_at_rest(s_home.view),
        "zero-initialized fresh context restores the resting pose");
}

static void room_temperature_subtitle(void)
{
    orb_home_snapshot_t *original = calloc(1, sizeof(*original));
    home_check(original && orb_home_snapshot(original), "save Home before subtitle fixture");
    orb_home_room_t room = {.id = "subtitle", .name = "Living Room"};
    orb_home_accessory_t accessories[] = {
        {.id = "subtitle-light", .room = "subtitle", .name = "Light", .kind = ORB_HOME_LIGHT,
         .reachable = true, .writable = ORB_HOME_ON, .state = {.present = ORB_HOME_ON, .on = false}},
        {.id = "subtitle-temp", .room = "subtitle", .name = "Thermostat", .kind = ORB_HOME_THERMOSTAT,
         .reachable = true, .state = {.present = ORB_HOME_TEMP, .temp = 22}},
    };
    home_check(orb_home_snapshot_chunk(&room, 1, accessories, 2, NULL, 0, true, false),
        "publish room with a light and reported temperature");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    lv_obj_t *root = orb_home_view_root(s_home.view), *list = orb_home_view_scroll(s_home.view);
    lv_obj_t *row = home_find_id(root, "room:subtitle"), *climate = home_find_id(root, "home:climate");
    home_check(row && climate && lv_obj_get_child_count(row) == 3 && home_find_text(row, "Living Room"),
        "room owns only its disc, whole-name title and subtitle with no trailing value lane");
    int row_y = lv_obj_get_y(row), climate_y = lv_obj_get_y(climate);
    home_check(orb_home_update("subtitle-light", &(orb_home_state_t){.present = ORB_HOME_ON, .on = true},
        false, true, 0), "turn on the room light");
    landing_poll_without_allocations("temperature subtitle updates without allocating"); home_advance(800);
    lv_area_t bounds;
    home_check(orb_round_list_row_bounds(row, &bounds) &&
        row_live_text(list, &bounds, "1 light on · 22°"), "temperature follows light state in the painted subtitle");
    home_check(home_find_id(root, "room:subtitle") == row && lv_obj_get_y(row) == row_y &&
        home_find_id(root, "home:climate") == climate && lv_obj_get_y(climate) == climate_y,
        "turning on a light retains both row objects and positions");
    lv_obj_scroll_to_view(climate, LV_ANIM_OFF); home_advance(800);
    home_check(orb_round_list_row_bounds(climate, &bounds) && row_live_text(list, &bounds, "22°"),
        "Climate keeps its independent trailing temperature value");
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); home_advance(800);
    home_check(orb_home_toggle("subtitle-light"), "queue pending subtitle fixture");
    orb_home_request_t request;
    home_check(orb_home_take_request(&request), "take pending subtitle request");
    home_advance(800); orb_round_list_row_bounds(row, &bounds);
    home_check(row_live_text(list, &bounds, "Updating…") && !row_live_text(list, &bounds, "Updating… · 22°"),
        "pending subtitle shows only its state without temperature");
    orb_home_complete(&request, NULL, "unreachable"); home_advance(800);
    home_check(row_live_text(list, &bounds, "Couldn’t update") &&
        !row_live_text(list, &bounds, "Couldn’t update · 22°") && lv_obj_get_y(row) == row_y &&
        lv_obj_get_y(climate) == climate_y && lv_obj_get_child_count(row) == 3,
        "failed subtitle omits temperature and preserves row positions and lane layout");
    home_check(orb_home_snapshot_chunk(original->rooms, original->room_count, original->accessories,
        original->accessory_count, original->scenes, original->scene_count, true, false), "restore Home after subtitle fixture");
    free(original); orb_home_set_connection(true, true);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
}

static void rooms_list_contract(void)
{
    orb_home_snapshot_t *original = calloc(1, sizeof(*original));
    orb_home_snapshot_t *fixture = calloc(1, sizeof(*fixture));
    home_check(original && fixture && orb_home_snapshot(original), "save demo before list fixture");
    fixture->room_count = fixture->accessory_count = 10;
    for (unsigned i = 0; i < fixture->room_count; ++i) {
        snprintf(fixture->rooms[i].id, sizeof(fixture->rooms[i].id), "room-%u", i);
        snprintf(fixture->rooms[i].name, sizeof(fixture->rooms[i].name), "Room %u", i);
        orb_home_accessory_t *a = &fixture->accessories[i];
        snprintf(a->id, sizeof(a->id), "lamp-%u", i);
        snprintf(a->room, sizeof(a->room), "%s", fixture->rooms[i].id);
        snprintf(a->name, sizeof(a->name), "Lamp %u", i);
        a->kind = ORB_HOME_LIGHT; a->reachable = true; a->writable = ORB_HOME_ON;
        a->state = (orb_home_state_t){.present=ORB_HOME_ON, .on=i == 2};
    }
    home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count,
        fixture->accessories, fixture->accessory_count, NULL, 0, true, false), "publish ten-room list fixture");
    orb_home_view_destroy(s_home.view);
    s_page_commands = 0;
    s_home.view = orb_home_view_create(lv_screen_active(), &(orb_home_commands_t){page_command, NULL}); home_advance(800);
    home_check(s_home.view != NULL && rooms_destination(), "fresh entry creates room list");
    landing_return_positions();
    lv_obj_t *root = orb_home_view_root(s_home.view), *list = orb_home_view_scroll(s_home.view);
    int rest = lv_obj_get_scroll_y(list);
    home_check(!orb_home_view_dial(s_home.view, NULL, NULL) && !orb_home_view_dial_writes(s_home.view),
        "landing list never claims a value dial or writes");
    home_check(landing_at_rest(s_home.view) && content_row_count(list) == fixture->room_count,
        "all rooms are retained in one list with no empty Climate or Scenes rows");
    home_check(!home_find_id(root, "home:climate") && !home_find_id(root, "scenes:open"),
        "Climate and Scenes are omitted when they have no content");
    lv_obj_t *rows[ORB_HOME_MAX_ROOMS]; int positions[ORB_HOME_MAX_ROOMS];
    for (unsigned i = 0; i < fixture->room_count; ++i) {
        char id[64]; snprintf(id, sizeof(id), "room:room-%u", i);
        rows[i] = home_find_id(root, id);
        home_check(rows[i] && lv_obj_get_parent(rows[i]) == list, "every model room has a named retained row");
        positions[i] = lv_obj_get_y(rows[i]);
        if (i) home_check(positions[i] > positions[i-1], "room row order follows the model");
    }
    unsigned objects = object_count(root);
    size_t hour_allocations = sim_memory_allocation_calls();
    orb_home_view_hour(s_home.view, 20);
    home_check(sim_memory_allocation_calls() == hour_allocations && !orb_home_view_moving(s_home.view),
        "hour rollover allocates nothing and leaves the landing list still");
    for (unsigned i = 0; i < fixture->room_count; ++i) {
        char id[64]; snprintf(id, sizeof(id), "room:room-%u", i);
        home_check(home_find_id(root, id) == rows[i] && lv_obj_get_y(rows[i]) == positions[i],
            "hour rollover retains every room row in the same place");
    }
    home_check(orb_home_update("lamp-9", &(orb_home_state_t){.present=ORB_HOME_ON, .on=true}, false, true, 0),
        "turn on an offscreen light");
    landing_poll_without_allocations("first light On update allocates no LVGL payload"); home_advance(800);
    home_check(orb_home_update("lamp-9", &(orb_home_state_t){.present=ORB_HOME_ON, .on=false}, false, true, 0),
        "turn the same light off");
    landing_poll_without_allocations("first light Off update allocates no LVGL payload"); home_advance(100);
    home_check(orb_home_update("lamp-9", &(orb_home_state_t){.present=ORB_HOME_ON, .on=true}, false, true, 0),
        "restore the reported light state");
    landing_poll_without_allocations("repeated light update allocates no LVGL payload"); home_advance(100);
    for (unsigned i = 0; i < fixture->room_count; ++i) {
        char id[64]; snprintf(id, sizeof(id), "room:room-%u", i);
        home_check(home_find_id(root, id) == rows[i] && lv_obj_get_y(rows[i]) == positions[i],
            "light state update retains each row object and position");
    }
    home_check(object_count(root) == objects, "live state update creates no objects");
    lv_obj_scroll_to_view(rows[9], LV_ANIM_OFF); home_advance(100);
    home_check(orb_home_toggle("lamp-9"), "queue synthetic pending room feedback");
    orb_home_request_t feedback;
    home_check(orb_home_take_request(&feedback), "take synthetic pending room write");
    landing_poll_without_allocations("first pending room update allocates no LVGL payload"); home_advance(800);
    home_check(home_find_text(root, "Updating") != NULL, "pending room row has explicit Updating words");
    orb_home_complete(&feedback, NULL, "unreachable");
    landing_poll_without_allocations("first failed room update allocates no LVGL payload"); home_advance(800);
    lv_obj_t *failed = home_find_text(root, "Couldn’t update");
    home_check(failed && lv_color_eq(lv_obj_get_style_text_color(failed, 0), ORB_RED),
        "failed room row has Failed wording in red");
    for (unsigned i = 0; i < fixture->room_count; ++i)
        home_check(lv_obj_get_y(rows[i]) == positions[i], "pending and failed text never moves rows");
    home_check(object_count(root) == objects, "pending and failed feedback retain prepared objects");
    s_home.now += 3600;
    landing_poll_without_allocations("room failure expiry allocates no LVGL payload"); home_advance(0);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); home_advance(100);
    orb_home_view_rotary(s_home.view, 240); home_advance(100);
    home_check(lv_obj_get_scroll_y(list) > 0, "rim rotary scrolls the room list");
    for (unsigned i = 0; i < fixture->room_count; ++i) {
        char name[32]; snprintf(name, sizeof(name), "Room %u", i);
        home_tap_text(name);
        orb_home_view_state_t state; orb_home_view_save(s_home.view, &state);
        char id[32]; snprintf(id, sizeof(id), "room-%u", i);
        home_check(state.surface == ORB_HOME_ROOM && !strcmp(state.room, id),
            "one tap opens exactly the named room");
        home_check(s_page_commands == 0 && !orb_home_take_request(&(orb_home_request_t){0}),
            "room row tap sends no command through the presenter port or model");
        int saved = state.rooms_scroll ? LV_MAX(0, state.rooms_scroll) : rest;
        home_check(orb_home_view_back(s_home.view), "room Back is local"); home_advance(800);
        list = orb_home_view_scroll(s_home.view);
        home_check(list && lv_obj_get_scroll_y(list) == saved, "Back preserves landing list scroll position");
    }
    orb_home_view_state_t saved, restored; orb_home_view_save(s_home.view, &saved);
    orb_home_view_destroy(s_home.view); s_home.view = orb_home_view_create(lv_screen_active(), NULL); home_advance(800);
    home_check(landing_at_rest(s_home.view), "fresh Home entry starts at top");
    orb_home_view_restore(s_home.view, &saved); home_advance(800); orb_home_view_save(s_home.view, &restored);
    home_check(restored.rooms_scroll == saved.rooms_scroll, "saved state restores landing scroll position");
    fixture->scene_count = 1; strcpy(fixture->scenes[0].id, "unsafe"); strcpy(fixture->scenes[0].name, "Unlock");
    fixture->scenes[0].contains_unlock = true;
    fixture->accessories[9].kind = ORB_HOME_SENSOR;
    home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count, fixture->accessories,
        fixture->accessory_count, fixture->scenes, fixture->scene_count, true, false), "publish sensor and unsafe scene");
    home_advance(800); root = orb_home_view_root(s_home.view);
    home_check(home_find_id(root, "home:climate") && !home_find_id(root, "scenes:open"),
        "sensor exposes Climate while unlock-only scenes stay hidden");
    lv_obj_t *temperature_row = home_find_id(root, "room:room-9");
    home_check(temperature_row != NULL, "temperature sensor retains its named room row");
    lv_obj_scroll_to_view(temperature_row, LV_ANIM_OFF); home_advance(100);
    unsigned sensor_objects = object_count(root);
    int sensor_position = lv_obj_get_y(temperature_row);
    home_check(orb_home_update("lamp-9", &(orb_home_state_t){.present=ORB_HOME_TEMP, .temp=37},
        false, true, 0), "sensor reports its first temperature");
    landing_poll_without_allocations("first sensor temperature allocates no LVGL payload"); home_advance(800);
    home_check(home_find_id(root, "room:room-9") == temperature_row &&
        home_find_text(root, "Off · 37°") && lv_obj_get_y(temperature_row) == sensor_position &&
        object_count(root) == sensor_objects,
        "first sensor temperature joins the prepared subtitle without rebuilding or moving the row");
    fixture->accessories[9].state.present = 0;
    home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count, fixture->accessories,
        fixture->accessory_count, fixture->scenes, fixture->scene_count, true, false), "sensor loses its temperature field");
    landing_poll_without_allocations("missing sensor temperature allocates no LVGL payload"); home_advance(800); root = orb_home_view_root(s_home.view);
    home_check(home_find_id(root, "room:room-9") == temperature_row &&
        !home_find_text(root, "Off · 37°") && lv_obj_get_y(temperature_row) == sensor_position &&
        object_count(root) == sensor_objects,
        "missing sensor temperature clears stale text while retaining the row and rail");
    fixture->scenes[0].contains_unlock = false;
    home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count, fixture->accessories,
        fixture->accessory_count, fixture->scenes, fixture->scene_count, true, false), "publish safe scene");
    home_advance(800);
    home_check(home_find_id(orb_home_view_root(s_home.view), "scenes:open") != NULL,
        "safe scene exposes Scenes row");
    home_check(orb_home_snapshot_chunk(original->rooms, original->room_count,
        original->accessories, original->accessory_count, original->scenes,
        original->scene_count, true, false), "restore demo after list fixture");
    orb_home_set_connection(true, true); free(original); free(fixture);
    orb_home_view_destroy(s_home.view); s_home.view = orb_home_view_create(lv_screen_active(), NULL); home_advance(800);
}

static void room_icon_mapping(void)
{
    const struct {const char *name, *icon;} cases[] = {
        {"BATHroom", ORB_ICON_DROP}, {"Wash Room", ORB_ICON_DROP}, {"Kitchen", ORB_ICON_FLAME},
        {"GARAGE", ORB_ICON_GARAGE}, {"Office", ORB_ICON_DESKTOP}, {"Study", ORB_ICON_DESKTOP},
        {"Bedroom", ORB_ICON_MOON}, {"Entry", ORB_ICON_DOOR}, {"Door", ORB_ICON_DOOR},
        {"Foyer", ORB_ICON_DOOR}, {"Hallway", ORB_ICON_DOOR}, {"Garden", ORB_ICON_SUN},
        {"Patio", ORB_ICON_SUN}, {"Yard", ORB_ICON_SUN}, {"Porch", ORB_ICON_SUN},
        {"Outdoor", ORB_ICON_SUN}, {"Dining", ORB_ICON_HOUSE}, {"", ORB_ICON_HOUSE},
        {NULL, ORB_ICON_HOUSE},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i)
        home_check(!strcmp(orb_home_room_icon(cases[i].name), cases[i].icon), "room identity keyword maps to existing icon");
}

static void summary_consistency(void)
{
    orb_home_snapshot_t *original = calloc(1, sizeof(*original));
    orb_home_snapshot_t *fixture = calloc(1, sizeof(*fixture));
    home_check(original && fixture && orb_home_snapshot(original), "copy summary fixture");
    for (unsigned count = 0; count <= 2; ++count) {
        *fixture = *original;
        fixture->accessory_count = 2;
        for (unsigned i = 0; i < 2; ++i) {
            fixture->accessories[i].state.on = i < count;
            fixture->accessories[i].state.present |= ORB_HOME_ON;
        }
        home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count,
            fixture->accessories, fixture->accessory_count, NULL, 0, true, false), "publish light count case");
        orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
        char expected[32]; snprintf(expected, sizeof(expected), "%u on", count);
        lv_obj_t *room = home_find_text(orb_home_view_root(s_home.view), "Living Room");
        home_check(room && home_find_text(orb_home_view_root(s_home.view), count ? "light" : "Off"),
            "focused room reports its active lights or Off");
    }
    *fixture = *original;
    fixture->accessory_count = 16;
    for (unsigned i = 0; i < fixture->accessory_count; i++) {
        fixture->accessories[i] = original->accessories[0];
        snprintf(fixture->accessories[i].id, sizeof(fixture->accessories[i].id), "count-%u", i);
        fixture->accessories[i].state.on = true;
    }
    home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count,
        fixture->accessories, fixture->accessory_count, NULL, 0, true, false), "publish two-digit room count");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    lv_obj_t *room_name = home_find_text(orb_home_view_root(s_home.view), "Living Room");
    lv_obj_t *room_count = room_name ? home_find_text(orb_home_view_root(s_home.view), "16 lights on") : NULL;
    home_check(room_count != NULL, "two-digit room state is present beside the room name");
    lv_point_t required;
    lv_text_get_size(&required, lv_label_get_text(room_count), lv_obj_get_style_text_font(room_count, 0),
        lv_obj_get_style_text_letter_space(room_count, 0), lv_obj_get_style_text_line_space(room_count, 0),
        LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (required.x > lv_obj_get_width(room_count) || required.y > lv_obj_get_height(room_count))
        fprintf(stderr, "Room count required=%dx%d label=%dx%d\n", required.x, required.y,
            lv_obj_get_width(room_count), lv_obj_get_height(room_count));
    home_check(required.x <= lv_obj_get_width(room_count) && required.y <= lv_obj_get_height(room_count),
        "two-digit state fits its label at every Dynamic Type size");
    *fixture = *original;
    fixture->accessory_count = 1;
    fixture->accessories[0] = original->accessories[3];
    fixture->accessories[0].state.on = true;
    home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count,
        fixture->accessories, 1, NULL, 0, true, false), "publish switch-controlled light");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    lv_obj_t *switch_list = orb_home_view_scroll(s_home.view);
    char room_id[64]; snprintf(room_id, sizeof(room_id), "room:%s", fixture->accessories[0].room);
    lv_obj_t *switch_row = home_find_id(orb_home_view_root(s_home.view), room_id);
    home_check(switch_row && switch_list, "switch summary belongs to its named room row");
    lv_obj_scroll_to_view(switch_row, LV_ANIM_OFF); home_advance(800);
    home_check(home_has_text("1 device on"),
        "focused room includes a reported on switch");
    fixture->accessories[0].state.present &= ~ORB_HOME_ON;
    home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count,
        fixture->accessories, 1, NULL, 0, true, false), "publish unknown cached on flag");
    home_advance(800);
    home_check(home_has_text("Off"), "focused state never invents power without field presence");
    *fixture = *original;
    for (unsigned i = 0; i < fixture->accessory_count; ++i) fixture->accessories[i].favourite = false;
    home_check(orb_home_snapshot_chunk(fixture->rooms, fixture->room_count,
        fixture->accessories, fixture->accessory_count, fixture->scenes,
        fixture->scene_count, true, false), "publish Home without favourite devices");
    home_advance(800);
    home_check(home_has_text("Kitchen") && !home_has_text("No favourites yet"),
        "Home keeps all named rooms independently of favourite membership");
    home_check(orb_home_snapshot_chunk(original->rooms, original->room_count,
        original->accessories, original->accessory_count, original->scenes,
        original->scene_count, true, false), "restore complete summary fixture");
    free(original); free(fixture);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
}

static void accessory_damage(void)
{
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    lv_obj_t *room = home_find_text(orb_home_view_root(s_home.view), "Living Room");
    home_check(room && home_find_text(orb_home_view_root(s_home.view), "2 devices on"), "initial room count is current");
    home_check(orb_home_update("floor-lamp", &(orb_home_state_t){.present = ORB_HOME_ON, .on = false},
        false, true, 0), "authoritative light turns off");
    home_advance(800);
    room = home_find_text(orb_home_view_root(s_home.view), "Living Room");
    home_check(room && home_find_text(orb_home_view_root(s_home.view), "1 device on"), "room count refreshes after a device update");
    frozen_frame("room count update");
    home_check(orb_home_update("floor-lamp", &(orb_home_state_t){.present = ORB_HOME_ON, .on = true},
        false, true, 0), "restore reported light state");
    quiet("Rooms count settled");
}

static void navigation_contract(void)
{
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    home_tap_text("Living Room");
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    home_check(list && lv_obj_get_child_count(list) > 5,
        "room scroll owner contains every accessory beyond the initial rows");
    home_check(!home_find_action(orb_home_view_root(s_home.view), "More Devices"),
        "continuous room list has no hidden paging target over its last row");
    lv_obj_update_layout(list);
    int bottom = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
    home_check(bottom > 0, "room list has native vertical scroll range");
    lv_obj_scroll_to_y(list, bottom, LV_ANIM_OFF); home_advance(800);
    home_check(home_has_text("Sensor") && home_has_text("Living Room"),
        "scrolling reveals the final accessory while retaining the room title");
    complete_text("Living Room");
    lv_obj_t *sensor = home_find_action(orb_home_view_root(s_home.view), "Sensor");
    lv_area_t sensor_bounds;
    home_check(sensor && orb_round_list_row_bounds(sensor, &sensor_bounds),
        "final accessory is a painted hit target above the All off row");
    home_tap((sensor_bounds.x1 + sensor_bounds.x2) / 2, (sensor_bounds.y1 + sensor_bounds.y2) / 2);
    home_advance(800);
    home_check(home_has_text("21°"), "scrolled room disclosure opens that sensor");
    orb_home_view_back(s_home.view); home_advance(800);
    home_check(home_has_text("Living Room") && home_has_text("Sensor") &&
        lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == bottom,
        "detail Back restores the same room and accessory scroll position");
    orb_home_view_back(s_home.view); home_advance(800);
    home_check(rooms_destination() && !home_has_text("Sensor"), "second Back returns to Rooms");
    home_tap_text("Climate");
    orb_home_view_state_t landing_before; orb_home_view_save(s_home.view, &landing_before);
    home_check(home_has_text("Climate") && home_has_text("Heating to 22°"),
        "section selector retains Climate access");
    home_check(orb_home_view_back(s_home.view), "Climate Back returns to Rooms"); home_advance(800);
    home_check(rooms_destination(), "Climate returns to the room list");
    orb_home_view_state_t landing_after; orb_home_view_save(s_home.view, &landing_after);
    home_check(landing_after.rooms_scroll == landing_before.rooms_scroll,
        "Climate Back retains the row's landing scroll context");
    home_tap_text("Scenes");
    orb_home_view_state_t scenes_state;
    orb_home_view_save(s_home.view, &scenes_state);
    home_check(scenes_state.scene_page && orb_home_view_scroll(s_home.view) &&
        lv_obj_get_child_count(orb_home_view_scroll(s_home.view)) == 5 && !home_has_text(ORB_ICON_STAR),
        "scene round list retains every safe scene without the floating star");
    orb_home_view_back(s_home.view); home_advance(800);
    orb_home_view_save(s_home.view, &landing_after);
    home_check(rooms_destination() && landing_after.rooms_scroll == scenes_state.rooms_scroll,
        "scene list Back returns to the same room-list scroll");
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "section navigation never writes Home");
    frozen_frame("section and detail navigation");
}

static void climate_reading_order(void)
{
    orb_home_view_select(s_home.view,ORB_HOME_CLIMATE); home_advance(800);
    lv_obj_t *current = home_find_text(orb_home_view_root(s_home.view),"71°");
    if (!current) current = home_find_text(orb_home_view_root(s_home.view),"22°");
    if (!current) current = home_find_text(orb_home_view_root(s_home.view),"21°");
    home_check(current != NULL, "Climate keeps its dominant current temperature");
    if (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3) {
        home_check(!home_has_text("Bedroom"), "AX3 Climate does not merge room labels into its central reading");
    } else {
        const char *rooms[] = {"Living Room","Bedroom","Kitchen","Office","Dining"};
        for (unsigned i = 0; i < sizeof(rooms)/sizeof(*rooms); ++i) {
            lv_obj_t *label = home_find_text(orb_home_view_root(s_home.view),rooms[i]);
            if (!label) continue;
            lv_area_t box; lv_obj_get_coords(label,&box);
            home_check(box.x2 <= 136 || box.x1 >= 329 || box.y2 <= 165 || box.y1 >= 308,
                "Default room labels clear the reserved Climate block by sixteen pixels");
        }
    }
    home_tap_text("Sensors");
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    orb_home_view_state_t state;
    orb_home_view_save(s_home.view, &state);
    home_check(state.sensors && list && home_has_text("Sensors"),
        "room sensors open their separate native list");
    round_list_metrics(list);
    /* AX3 rows can push Bedroom below the initial viewport. Its retained
     * subtitle is navigation content; scroll it into view before testing ink. */
    lv_obj_t *bedroom = NULL;
    for (unsigned i = 0; i < lv_obj_get_child_count(list) && !bedroom; i++) {
        lv_obj_t *row = lv_obj_get_child(list, (int32_t)i);
        if (lv_obj_is_floating(row)) continue;
        for (unsigned j = 0; j < lv_obj_get_child_count(row); j++) {
            lv_obj_t *label = lv_obj_get_child(row, (int32_t)j);
            if (lv_obj_check_type(label, &lv_label_class) &&
                strstr(lv_label_get_text(label), "Bedroom")) { bedroom = row; break; }
        }
    }
    home_check(bedroom != NULL, "sensor list retains its Bedroom reading row");
    lv_obj_scroll_to_view(bedroom, LV_ANIM_OFF);
    home_advance(800);
    home_check(home_has_text("Bedroom") && home_has_text("Sensors"),
        "room sensor readings remain visible through native vertical scrolling");
    orb_home_view_state_t sensors_state; orb_home_view_save(s_home.view, &sensors_state);
    orb_home_view_back(s_home.view); home_advance(800);
    orb_home_view_state_t climate_state; orb_home_view_save(s_home.view, &climate_state);
    home_check(!climate_state.sensors && climate_state.surface == ORB_HOME_CLIMATE && home_has_text("Sensors"),
        "sensor-list Back returns to Climate, its parent");
    orb_home_view_back(s_home.view); home_advance(800);
    orb_home_view_state_t rooms_state; orb_home_view_save(s_home.view, &rooms_state);
    home_check(rooms_destination() && rooms_state.rooms_scroll == sensors_state.rooms_scroll,
        "Climate Back restores the same landing list scroll position");
    home_tap_text("Climate");
    home_tap_text("Thermostat");
    home_check(home_has_text("Thermostat"), "Climate's kit action opens its thermostat directly");
    orb_home_view_back(s_home.view); home_advance(800);
    home_check(home_has_text("Climate"), "thermostat Back preserves Climate as its source");
}

static void scene_identity(void)
{
    orb_home_view_open(s_home.view, "floor-lamp");
    home_advance(800);
    orb_home_view_detail_page(s_home.view, 2);
    home_advance(800);
    home_check(home_has_text("Good morning"), "scene page shows its initial stable scene");
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot) && snapshot->scene_count >= 2,
               "copy scene reorder fixture");
    orb_home_scene_t first = snapshot->scenes[0];
    snapshot->scenes[0] = snapshot->scenes[1];
    snapshot->scenes[1] = first;
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
        snapshot->scene_count, true, false), "publish reordered scenes");
    free(snapshot);
    home_advance(800);
    home_check(home_has_text("Good morning"), "scene disclosure follows its stable identity after authoritative reorder");
    home_tap_text("Good morning");
    orb_home_request_t request;
    home_check(orb_home_take_request(&request) && request.kind == ORB_HOME_REQUEST_SCENE &&
        !strcmp(request.id,"morning"), "scene tap queues the exact currently displayed stable ID");
    orb_home_complete(&request,NULL,NULL);
    home_advance(800);
}

static void toggle_and_rollback(void)
{
    orb_home_view_open(s_home.view, "kitchen");
    home_advance(1000);
    orb_home_snapshot_t *before = calloc(1, sizeof(*before));
    orb_home_snapshot_t *after = calloc(1, sizeof(*after));
    home_check(before && after && orb_home_snapshot(before), "read switch before toggle");
    const orb_home_accessory_t *old = orb_home_find(before, "kitchen");
    home_check(old != NULL, "switch exists");
    home_measure_start();
    home_tap(233, 140);
    home_advance(96);
    home_measure_stop();
    home_check(orb_home_snapshot(after), "read optimistic switch");
    const orb_home_accessory_t *changed = orb_home_find(after, "kitchen");
    home_check(changed && changed->state.on != old->state.on && changed->pending,
               "switch changes immediately with pending delivery");
    home_check(s_home.frames > 0, "toggle supplies visual feedback");
    orb_home_request_t request;
    home_check(orb_home_take_request(&request) && !strcmp(request.id, "kitchen"),
               "tap queues exact switch once");
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "tap never duplicates a request");
    frozen_frame("optimistic switch");
    orb_home_complete(&request, NULL, "unreachable");
    home_advance(800);
    home_check(orb_home_snapshot(after), "read failed switch");
    changed = orb_home_find(after, "kitchen");
    home_check(changed && changed->state.on == old->state.on && !changed->pending,
               "failed write honestly restores authoritative state");
    home_check(home_has_text("Couldn’t update") && home_has_text("Kitchen Lights"),
               "write failure preserves the reverted value and accessory identity");
    frozen_frame("failed switch");
    free(before);
    free(after);
}

static void white_temperature_roundtrip(void)
{
    orb_home_view_open(s_home.view, "dining");
    home_advance(800);
    orb_home_view_detail_page(s_home.view, 1);
    home_advance(800);
    home_check(home_has_text("Warm White") && !home_has_text("mired"),
               "300-mired white light presents a useful colour name without protocol units");
    home_check(home_has_text("3330 K"), "white temperature converts 300 mired to rounded kelvin");
    lv_obj_t *kelvin = home_find_text(orb_home_view_root(s_home.view), "3330 K");
    orb_kit_value_info_t kelvin_initial;
    home_check(orb_kit_value_info(kelvin, &kelvin_initial) && kelvin_initial.live && kelvin_initial.tabular,
        "white-temperature supporting value uses the same live tabular API as its hero");
    home_adjust_step(ORB_ICON_PLUS); home_advance(800);
    orb_home_request_t request;
    home_check(orb_home_take_request(&request) && !strcmp(request.id,"dining") &&
        request.values.present == ORB_HOME_CT && request.values.ct > 300,
        "white Plus adjusts and queues the exact fixture's canonical mired value");
    orb_home_snapshot_t *snapshot = calloc(1,sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "read optimistic white temperature");
    orb_home_state_t accepted = orb_home_find(snapshot,"dining")->state;
    free(snapshot);
    orb_home_complete(&request,&accepted,NULL); home_advance(800);
    home_adjust_step(ORB_ICON_MINUS); home_advance(800);
    home_check(orb_home_take_request(&request) && request.values.present == ORB_HOME_CT &&
        fabsf(request.values.ct - 300) < .1f, "white Minus returns to the original canonical value");
    accepted.ct = request.values.ct;
    orb_home_complete(&request,&accepted,NULL); home_advance(800);
    home_check(home_has_text("Warm White") && home_has_text("3330 K"),
        "accepted state round-trips the chosen white-temperature presentation");
    /* The shared level occupies145°→35° along the bottom, replacing the
     * old full270° Home dial. Exercise the actual visible end caps. */
    home_press(61,355); home_advance(32);
    home_press(405,355); home_advance(32);
    orb_kit_value_info_t kelvin_drag;
    home_check(lv_obj_is_valid(kelvin) && orb_kit_value_info(kelvin, &kelvin_drag) &&
        kelvin_drag.width == kelvin_initial.width &&
        kelvin_drag.template_measures == kelvin_initial.template_measures &&
        kelvin_drag.raster_prepares == kelvin_initial.raster_prepares,
        "Kelvin detents retain their label, reservation and native ink without new preparation");
    frozen_frame("white temperature drag recolours the whole gauge");
    home_release(); home_advance(800);
    home_check(orb_home_take_request(&request) && request.values.present == ORB_HOME_CT &&
        !strcmp(request.id,"dining") && request.values.ct >= 140 && request.values.ct <= 500,
        "white rim gesture retains the canonical bounded temperature contract");
    accepted.ct = request.values.ct;
    orb_home_complete(&request,&accepted,NULL); home_advance(800);
    home_check(!home_has_text("mired"), "authoritative white rim result keeps owner-facing units");
    frozen_frame("white temperature roundtrip");
}

static void secure_lock(void)
{
    orb_home_view_open(s_home.view, "front-door");
    home_advance(1000);
    home_tap(233, 140);
    home_advance(300);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "a lock tap never unlocks");
    home_press(233, 140);
    home_advance(600);
    home_release();
    home_advance(600);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "short hold never unlocks");
    home_press(233, 140);
    home_advance(600);
    home_press(400, 390);
    home_advance(600);
    home_release();
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "leaving the lock cancels its hold");
    home_advance(800);
    home_press(233,140);
    home_advance(500);
    lv_obj_set_hidden(orb_home_view_root(s_home.view),true);
    home_advance(600);
    home_release();
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "hiding a parent cancels its lock hold");
    lv_obj_set_hidden(orb_home_view_root(s_home.view),false);
    home_advance(800);
    home_press(233, 140);
    home_advance(1040);
    home_release();
    orb_home_request_t request;
    home_check(orb_home_take_request(&request), "one-second hold queues unlock");
    home_check(request.kind == ORB_HOME_REQUEST_SET && !strcmp(request.id, "front-door") &&
               (request.values.present & ORB_HOME_LOCKED) && !request.values.locked,
               "hold requests only the intended lock's unlock value");
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "hold queues only one unlock");
    frozen_frame("held lock");
    orb_home_complete(&request, NULL, "unreachable");
    home_advance(800);
    orb_home_snapshot_t *snapshot = calloc(1,sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "read reverted lock state");
    const orb_home_accessory_t *lock = orb_home_find(snapshot,"front-door");
    home_check(lock && lock->state.locked && home_has_text("Couldn’t update") && home_has_text("Front Door"),
        "failed unlock restores Locked and identifies the failed accessory");
    free(snapshot);
}

static void content_contact_guards(void)
{
    orb_home_load_demo(); authoritative_fixture();
    orb_home_view_open(s_home.view, "kitchen"); home_advance(800);
    /* Returning to the original point does not turn a drag into a tap. */
    home_press(233, 140); home_press(245, 140); home_press(233, 140);
    home_release(); home_advance(800);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}),
        "accessory centre drag returning to its start never toggles");
    home_press(233, 140); home_advance(500); home_release(); home_advance(800);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}),
        "accessory hold is not a tap when released after 500ms");
    home_tap(233, 140); home_advance(800);
    orb_home_request_t request;
    home_check(orb_home_take_request(&request), "short stationary accessory tap remains usable");
    orb_home_complete(&request, &request.values, NULL); home_advance(800);

    orb_home_view_open(s_home.view, "front-door"); home_advance(800);
    home_press(233, 140); home_advance(480); home_press(245, 140);
    home_press(233, 140); home_advance(700); home_release(); home_advance(800);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}),
        "a 12px centre drag cancels a lock hold even inside its disc");

    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    lv_obj_t *focus = semantic_action(orb_home_view_root(s_home.view), "room:Living Room");
    home_check(focus != NULL, "room row target exists for pointer arbitration");
    lv_obj_t *landing = orb_home_view_scroll(s_home.view);
    lv_obj_scroll_to_y(landing, 0, LV_ANIM_OFF); home_advance(100);
    lv_area_t bounds;
    home_check(orb_round_list_child_bounds(focus, focus, &bounds), "room row exposes its painted target");
    int x = (bounds.x1 + bounds.x2)/2, y = (bounds.y1 + bounds.y2)/2;
    home_press(x, y); home_press(x, y + 12); home_press(x, y);
    home_release(); home_advance(800);
    home_check(rooms_destination(), "Rooms centre drag returning to its start never opens a room");
    home_press(x, y); home_advance(500); home_release(); home_advance(800);
    home_check(rooms_destination(), "Rooms centre hold never becomes a late room tap");
    home_tap(x, y); home_advance(800);
    orb_home_view_state_t state; orb_home_view_save(s_home.view, &state);
    home_check(state.surface == ORB_HOME_ROOM, "short room row tap opens its room");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
}

static void rim_damage(void)
{
    orb_home_view_open(s_home.view, "floor-lamp");
    home_advance(1600);
    home_measure_start();
    for (unsigned i = 0; i < 8; ++i) {
        float angle = (145.f - i * 7.f) * (float)M_PI / 180.f;
        home_press((int)lroundf(233 + 210 * cosf(angle)), (int)lroundf(233 + 210 * sinf(angle)));
        home_advance(16);
    }
    home_measure_stop();
    printf("Home rim frames=%u max_damage=%u max_flush=%u\n", s_home.frames,
           s_home.largest_damage, s_home.largest_frame);
    home_check(s_home.frames > 0, "rim responds to real pointer samples");
    home_check(s_home.largest_damage < SIM_D * SIM_D / 2, "rim never invalidates the full dial rectangle");
    frozen_frame("moving rim");
    home_release();
    home_advance(1000);
    orb_home_request_t request;
    home_check(orb_home_take_request(&request), "rim release queues a bounded write");
    home_check(request.kind == ORB_HOME_REQUEST_SET && !strcmp(request.id, "floor-lamp") &&
               (request.values.present & ORB_HOME_BRIGHTNESS), "rim adjusts the exact light's brightness");
    orb_home_complete(&request, &request.values, NULL);
    home_advance(1200);
    quiet("Rim settled");
}

static void system_navigation_handoff(void)
{
    /* The preceding compact brightness result deliberately removed unknown
     * fields. This independent scenario supplies a reported power value. */
    home_check(orb_home_update("floor-lamp", &(orb_home_state_t){.present=ORB_HOME_ON, .on=true},
               false, true, 0), "publish a known power state for the system-input fixture");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS);
    home_advance(800);
    orb_home_view_system_navigation(s_home.view, true);
    for (unsigned direction = 0; direction < 2; ++direction) {
        home_press(direction ? 140 : 340, 320);
        home_advance(16);
        home_press(direction ? 320 : 160, 320);
        home_release(); home_advance(1000);
        orb_home_view_state_t state; orb_home_view_save(s_home.view, &state);
        home_check(state.surface == ORB_HOME_ROOMS,
            "embedded horizontal swipe leaves the room list in place");
    }
    for (unsigned direction = 0; direction < 2; ++direction) {
        home_press(direction ? 430 : 36, 233);
        home_press(direction ? 250 : 216, 233);
        home_release(); home_advance(1000);
        home_check(rooms_destination(), "outer 40px contacts remain exclusively shell-owned");
    }
    home_check(!orb_home_take_request(&(orb_home_request_t){0}),
        "system-owned Home drags never actuate a device");
    home_check(orb_home_view_scroll(s_home.view) != NULL, "Rooms exposes its native list scroll owner");
    orb_home_view_state_t before, after;
    orb_home_view_save(s_home.view, &before);
    orb_home_view_rotary(s_home.view, 72);
    home_advance(32);
    orb_home_view_save(s_home.view, &after);
    home_check(after.rooms_scroll > before.rooms_scroll, "system rotary scrolls the room list");
    orb_home_view_rotary(s_home.view, -100000);
    home_advance(32);
    orb_home_view_save(s_home.view, &after);
    home_check(after.scroll == 0 && after.rooms_scroll == -1,
        "system rotary clamps at native zero while preserving the explicit equator context");
    orb_home_view_open(s_home.view, "floor-lamp");
    home_advance(800);
    for (unsigned i = 0; i < 8; ++i) {
        float angle = (145.f - i * 7.f) * (float)M_PI / 180.f;
        home_press((int)lroundf(233 + 210 * cosf(angle)), (int)lroundf(233 + 210 * sinf(angle)));
        home_advance(16);
    }
    /* Crossing inward after rim capture must not arm the independent dial. */
    home_press(340, 200);
    home_release();
    /* The primary's outer corner is still within the system rim. A short
     * captured drag that ends inside it must not become a late power click. */
    home_press(300,430); home_advance(16);
    home_press(285,420); home_advance(16); home_release();
    orb_home_view_rotary(s_home.view, 240);
    home_advance(800);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}),
               "system rim capture/rotary never queues a Home action");
    lv_obj_t *power = identity_disc();
    home_check(power != NULL && !lv_obj_has_state(power, LV_STATE_DISABLED),
               "the explicit power button is available after system capture");
    lv_area_t power_bounds; lv_obj_get_coords(power, &power_bounds);
    home_tap((power_bounds.x1 + power_bounds.x2)/2, (power_bounds.y1 + power_bounds.y2)/2);
    home_advance(32);
    orb_home_request_t deliberate;
    bool has_deliberate = orb_home_take_request(&deliberate);
    if (!has_deliberate) fprintf(stderr,"Power tap: bounds=%d,%d..%d,%d no request\n",
        power_bounds.x1,power_bounds.y1,power_bounds.x2,power_bounds.y2);
    home_check(has_deliberate && deliberate.kind == ORB_HOME_REQUEST_TOGGLE &&
               (deliberate.values.present & ORB_HOME_ON),
               "a deliberate button remains usable after cancelled system rim capture");
    orb_home_snapshot_t *power_state = calloc(1, sizeof(*power_state));
    home_check(power_state && orb_home_snapshot(power_state), "copy the complete fixture state");
    const orb_home_accessory_t *power_accessory = orb_home_find(power_state, "floor-lamp");
    home_check(power_accessory != NULL, "the deliberate control retains its exact accessory");
    orb_home_complete(&deliberate, &power_accessory->state, NULL);
    free(power_state); home_advance(800);
    orb_home_view_detail_page(s_home.view, 1);
    home_advance(800);
    home_check(orb_home_view_back(s_home.view), "system Back consumes the accessory's colour page");
    home_advance(800);
    home_check(home_has_text("Floor Lamp"), "nested Back stays in the accessory");
    home_check(orb_home_view_can_back(s_home.view), "nested Home exposes Back without mutating state");
    home_check(orb_home_view_back(s_home.view), "system Back returns to the exact Home list");
    home_advance(800);
    home_check(rooms_destination(), "Home restores the room list");
    home_check(!orb_home_view_can_back(s_home.view), "top-level Home query belongs to the shell");
    home_check(!orb_home_view_back(s_home.view), "top-level Home Back belongs to the shell");
    orb_home_view_system_navigation(s_home.view, false);
}

static void connection_gates(void)
{
    orb_home_view_open(s_home.view, "bookshelf");
    home_advance(800);
    home_tap(233, 395);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "No Response cannot write");
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "copy availability fixture");
    orb_home_accessory_t accessories[ORB_HOME_MAX_ACCESSORIES];
    memcpy(accessories, snapshot->accessories, sizeof(accessories));
    for (size_t i = 0; i < snapshot->accessory_count; i++) accessories[i].reachable = false;
    home_check(orb_home_snapshot_chunk_available(snapshot->rooms, snapshot->room_count,
        accessories, snapshot->accessory_count, snapshot->scenes, snapshot->scene_count,
        false, true, false), "relay unavailable while pinned hub remains online");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    home_check(home_has_text("Unavailable") && home_has_text("Your Mac can't reach Home right now."),
               "unavailable Home names that one state and explains it");
    lv_obj_t *scene = home_find_action(orb_home_view_root(s_home.view), "Scenes");
    home_check(!scene, "unavailable Home omits its Scenes action");
    quiet("Relay unavailable");
    orb_home_view_open(s_home.view, "front-door"); home_advance(800);
    home_check(home_has_text("Unavailable") && home_has_text("Last known") && home_has_text("Locked") &&
        !home_has_text("Hold to unlock"),
               "unavailable relay removes the unlock affordance");
    home_press(233, 140); home_advance(1100); home_release();
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "unavailable relay hold cannot unlock");
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
        snapshot->scene_count, true, false), "relay recovery publishes fresh available snapshot");
    orb_home_snapshot_discard();
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    home_check(home_has_text("Loading…") || home_has_text("Unavailable"),
               "rejected Home state is visibly stale without losing the Mac link");
    scene = home_find_action(orb_home_view_root(s_home.view), "Scenes");
    home_check(!scene, "loading Home omits its Scenes action");
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
        snapshot->scene_count, true, false), "fresh snapshot repairs stale Home state");
    free(snapshot);
    orb_home_set_connection(false, false);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS);
    home_advance(800);
    home_check(home_has_text("Connect your Mac to load Home."), "offline surface is explicit");
    home_check(!home_find_action(orb_home_view_root(s_home.view), "More Devices"),
        "offline Mac explanation owns the footer without an overlapping paging target");
    frozen_frame("Offline navigation");
    orb_home_view_open(s_home.view, "front-door");
    home_advance(800);
    home_check(home_has_text("Last known") && home_has_text("Locked"), "offline lock qualifies its cached state");
    home_press(233, 140);
    home_advance(1100);
    home_release();
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "offline hold cannot unlock");
    quiet("Offline lock");
    const char *dial_ids[] = {"floor-lamp", "fan", "climate"};
    const char *contexts[] = {"Living Room", "Bedroom", "Now"};
    for (unsigned i = 0; i < sizeof(dial_ids) / sizeof(*dial_ids); ++i) {
        orb_home_set_connection(false, false);
        orb_home_view_open(s_home.view, dial_ids[i]); home_advance(800);
        home_check(home_has_text("Last known"), "offline dial qualifies the cached value");
        orb_home_set_connection(true, true); home_advance(800);
        lv_obj_t *context = home_find_text(orb_home_view_root(s_home.view), contexts[i]);
        home_check(context && !home_has_text("Last known"),
            "reconnected dial restores its room/current context");
        lv_area_t cb = text_required_bounds(context);
        home_check(cb.y1 > 180, "reconnected context stays in its template lane below the identity disc");
        frozen_frame("reconnected dial context");
    }
}
static void unknown_lock_and_wire_sensors(void)
{
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "copy unknown-state fixture");
    home_check(orb_home_set("front-door", &(orb_home_state_t){.present=ORB_HOME_LOCKED,.locked=true}, false),
               "queue an explicit lock write");
    orb_home_request_t request;
    home_check(orb_home_take_request(&request), "take explicit lock write");
    orb_home_complete(&request, &(orb_home_state_t){0}, NULL);
    orb_home_view_open(s_home.view, "front-door"); home_advance(800);
    home_check(home_has_text("—") && !home_has_text("Read-only") &&
        !home_has_text("Hold to unlock") && !home_has_text("Tap to lock"),
        "empty accepted lock state presents unknown without inventing unlocked");
    frozen_frame("Unknown lock");
    home_press(233,140); home_advance(1100); home_release();
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "unknown lock never offers a security action");
    for (size_t i = 0; i < snapshot->accessory_count; i++) {
        orb_home_accessory_t *a = &snapshot->accessories[i];
        if (a->kind < ORB_HOME_TEMPERATURE || a->kind > ORB_HOME_SENSOR) continue;
        if (a->kind == ORB_HOME_HUMIDITY) a->state.present &= ~ORB_HOME_TEMP;
        a->kind = ORB_HOME_SENSOR;
    }
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
        snapshot->scene_count, true, false), "canonical sensor kinds come from reported fields");
    const struct { const char *id, *value; } sensors[] = {
        {"bed-temp", "20°"}, {"kitchen-temp", "49%"}, {"hall-motion", "Motion"}, {"window", "Closed"},
    };
    for (size_t i = 0; i < sizeof(sensors)/sizeof(sensors[0]); i++) {
        orb_home_view_open(s_home.view, sensors[i].id); home_advance(800);
        home_check(home_has_text(sensors[i].value), "canonical sensor renders its reported primary field");
    }
    free(snapshot);
}

static void interrupted_fill(void)
{
    orb_home_view_open(s_home.view,"kitchen");
    home_advance(800);
    home_tap(233,140);
    /* Navigation may delete a disc while its finite fill timer is live. */
    orb_home_view_select(s_home.view,ORB_HOME_ROOMS);
    home_advance(1200);
    home_check(rooms_destination(),"leaving an active fill keeps the complete Rooms destination usable");
    orb_home_request_t request;
    home_check(orb_home_take_request(&request)&&!strcmp(request.id,"kitchen"),
               "leaving an active fill preserves its single delivery request");
    orb_home_complete(&request,&request.values,NULL);
    home_advance(800);
    frozen_frame("deleted active fill");
}

static void fahrenheit_controls(void)
{
    orb_home_load_demo();
    authoritative_fixture();
    orb_prefs_t prefs;
    orb_model_prefs_defaults(&prefs);
    prefs.temperature = ORB_TEMPERATURE_F;
    home_check(orb_model_set_prefs(&prefs), "apply hub Fahrenheit preference");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    home_check(home_has_text("71°") || home_has_text("70°"), "focused room state converts Celsius at every text size");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    home_check(home_has_text("71°") || home_has_text("70°"), "Rooms converts Celsius");
    orb_home_view_select(s_home.view, ORB_HOME_CLIMATE); home_advance(800);
    home_check((home_has_text("71°") || home_has_text("70°")) && home_has_text("Heating to 72°"), "Climate converts current and target");
    quiet("Fahrenheit Climate");
    orb_home_view_open(s_home.view, "bed-temp"); home_advance(800);
    home_check(home_has_text("68°"), "sensor detail converts Celsius");
    orb_home_view_open(s_home.view, "climate"); home_advance(800);
    home_check(home_has_text("72°") && home_has_text("Heat · Now 71°"), "thermostat names target and the converted current reading (one formatter)");
    home_adjust_step(ORB_ICON_PLUS); home_advance(800);
    orb_home_request_t request;
    home_check(orb_home_take_request(&request), "Fahrenheit Plus queues a request");
    home_check(request.values.present == ORB_HOME_TARGET && fabsf(request.values.target - 22.8f) < .001f,
        "Plus advances exactly one Fahrenheit and sends tenth-Celsius");
    orb_home_state_t accepted = {.present = ORB_HOME_TARGET | ORB_HOME_TEMP | ORB_HOME_MODE,
        .target = request.values.target, .temp = 21, .mode = ORB_HOME_MODE_HEAT};
    orb_home_complete(&request, &accepted, NULL); home_advance(800);
    home_check(home_has_text("73°") && home_has_text("Heat · Now 70°"), "accepted Celsius target displays chosen Fahrenheit");
    home_adjust_step(ORB_ICON_MINUS); home_advance(800);
    home_check(orb_home_take_request(&request) && fabsf(request.values.target - 22.2f) < .001f,
        "Minus returns to 72 Fahrenheit as 22.2 Celsius");
    accepted.target = request.values.target;
    orb_home_complete(&request, &accepted, NULL); home_advance(800);
    home_check(home_has_text("72°") && home_has_text("Heat · Now"), "72 stays 72 after accepted tenth-Celsius rounding");
    home_check(orb_home_update("climate", &(orb_home_state_t){.present = ORB_HOME_TARGET, .target = 21.7f}, false, true, 0),
        "hub reports a different thermostat target");
    home_advance(800);
    home_check(home_has_text("71°") && home_has_text("Heat · Now"), "authoritative different target replaces chosen Fahrenheit");
    home_adjust_step(ORB_ICON_PLUS); home_advance(800);
    home_check(orb_home_take_request(&request), "queue failed thermostat write");
    orb_home_complete(&request, NULL, "timeout"); home_advance(1600);
    home_check(home_has_text("Couldn’t update") && home_has_text("71°"), "failed thermostat write preserves confirmed target beside failure");
    /* Unit changes arrive without a Home revision. Both live sensor and
     * climate labels must update and retain correct partial-redraw pixels. */
    orb_home_view_select(s_home.view, ORB_HOME_CLIMATE); home_advance(800);
    prefs.temperature = ORB_TEMPERATURE_C;
    home_check(orb_model_set_prefs(&prefs), "live preference switches to Celsius"); home_advance(800);
    home_check(home_has_text("21°") && home_has_text("Heating to 21.5°"), "live units refresh Climate without Home events (the detail's half-step target)");
    frozen_frame("live Celsius Climate");
    orb_home_view_open(s_home.view, "bed-temp"); home_advance(800);
    home_check(home_has_text("20°"), "sensor switches back to Celsius");
    prefs.temperature = ORB_TEMPERATURE_F;
    home_check(orb_model_set_prefs(&prefs), "live preference switches to Fahrenheit"); home_advance(800);
    home_check(home_has_text("68°"), "live units refresh an already open sensor");
    frozen_frame("live Fahrenheit sensor");
    orb_home_view_open(s_home.view, "climate"); home_advance(800);
    home_press(61,355); home_advance(32);
    prefs.temperature = ORB_TEMPERATURE_C;
    home_check(orb_model_set_prefs(&prefs), "change units during a rim gesture"); home_advance(32);
    home_release(); home_advance(800);
    home_check(!orb_home_take_request(&request), "changing scale cancels the old rim gesture");
    orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_C;
    home_check(orb_model_set_prefs(&prefs), "restore Celsius fixture");
}


static void position_content_changed(lv_event_t *event)
{
    bool *change = lv_event_get_user_data(event);
    if (!*change) return;
    *change = false;
    lv_obj_set_style_bg_color(lv_event_get_target_obj(event), lv_color_hex(0x30D158), 0);
}

static void retained_position_contract(void)
{
    /* Exercise the generic X/Y correction as well as Home: a cached ancestor
     * must rebuild when its child moves, while that child's source survives.
     * A style callback's actual content mutation must never be suppressed. */
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 240, 100); lv_obj_set_pos(parent, 54, 170);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t *child = lv_obj_create(parent);
    lv_obj_remove_style_all(child);
    lv_obj_set_size(child, 40, 40); lv_obj_set_pos(child, 12, 20);
    lv_obj_set_style_bg_color(child, lv_color_hex(0x0091FF), 0);
    lv_obj_set_style_bg_opa(child, LV_OPA_COVER, 0);
    lv_obj_set_raster_matte(child, lv_color_black());
    lv_obj_set_raster_matte(parent, lv_color_black());
    lv_obj_raster_prepare(parent);
    lv_refr_now(s_home.display);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    lv_obj_set_pos(child, 70, 28);
    lv_obj_update_layout(parent);
    lv_obj_raster_prepare(parent);
    lv_obj_raster_get_stats(&after);
    home_check(after.builds == before.builds + 1,
        "position-only style rebuilds cached ancestor and preserves child source");
    lv_area_t area; lv_obj_get_coords(child, &area);
    home_check(area.x1 == 124 && area.y1 == 198,
        "style coordinates remain authoritative after forced layout");
    frozen_frame("cached-child position style");
    bool change = true;
    lv_obj_add_event_cb(child, position_content_changed, LV_EVENT_STYLE_CHANGED, &change);
    before = after;
    lv_obj_set_x(child, 90);
    lv_obj_update_layout(parent);
    lv_obj_raster_prepare(parent);
    lv_obj_raster_get_stats(&after);
    home_check(!change && after.builds == before.builds + 2,
        "position callback content change rebuilds both child and ancestor");
    frozen_frame("position callback content mutation");
    lv_obj_delete(parent);
    lv_refr_now(s_home.display);
}


static void positioned_parent_contract(void)
{
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent); lv_obj_set_scrollable(parent, false);
    lv_obj_set_pos(parent, 13, 9); lv_obj_set_size(parent, 466, 466);
    orb_home_view_t *view = orb_home_view_create(parent, NULL);
    home_check(view != NULL, "create Home inside a positioned parent");
    lv_obj_t *root = orb_home_view_root(view); lv_obj_update_layout(root);
    lv_area_t bounds; lv_obj_get_coords(root, &bounds);
    home_check(bounds.x1 == 13 && bounds.y1 == 9, "Home root respects its hosting parent's coordinates");
    home_check(semantic_action(root, "room:Kitchen") != NULL,
        "positioned Home retains its named room row targets");
    orb_home_view_destroy(view); lv_obj_delete(parent);
}

static void landing_model_updates(void)
{
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "copy landing update fixture");
    const orb_home_accessory_t *lamp = orb_home_find(snapshot, "floor-lamp");
    home_check(lamp != NULL, "landing fixture contains its known light");
    bool on = lamp->state.on;
    orb_home_view_t *saved = s_home.view;
    for(unsigned slice = 0; slice < 3; slice++) {
        s_home.view = orb_home_view_create(lv_screen_active(), NULL);
        home_check(s_home.view != NULL, "create a fresh Home landing list");
        home_advance(slice * 8);
        home_check(orb_home_update("floor-lamp",
            &(orb_home_state_t){.present = ORB_HOME_ON, .on = !on}, false, true, 0),
            "change content just after landing creation");
        home_advance(80);
        home_check(!orb_home_view_moving(s_home.view), "model changes keep the landing list settled");
        frozen_frame("Home update after landing creation");
        orb_home_view_destroy(s_home.view);
        s_home.view = saved;
        home_check(orb_home_update("floor-lamp",
            &(orb_home_state_t){.present = ORB_HOME_ON, .on = on}, false, true, 0),
            "restore authoritative light after landing update");
        home_advance(800);
    }
    free(snapshot);
}


static void landing_horizontal_contacts(void)
{
    unsigned commands_before = s_page_commands;
    orb_home_view_destroy(s_home.view);
    s_home.view = orb_home_view_create(lv_screen_active(), &(orb_home_commands_t){page_command, NULL});
    home_check(s_home.view != NULL, "create horizontal-contact fixture"); home_advance(800);
    for (unsigned direction = 0; direction < 2; ++direction) {
        home_press(direction ? 140 : 330, 233); home_advance(16);
        home_press(direction ? 330 : 140, 233); home_release(); home_advance(1000);
        home_check(rooms_destination() && !orb_home_view_moving(s_home.view),
            "horizontal swipes do not change the landing page");
    }
    home_check(s_page_commands == commands_before && !orb_home_take_request(&(orb_home_request_t){0}),
        "landing contacts never send a command");
    quiet("Rooms horizontal contacts settled");
}

static void saved_navigation_context(void)
{
    for (unsigned context = 0; context < 3; context++) {
        orb_home_view_select(s_home.view, ORB_HOME_ROOMS);
        home_advance(800);
        if (context == 1) { orb_home_view_room(s_home.view, "Living Room"); home_advance(800); }
        if (context == 2) {
            orb_home_view_open(s_home.view, "floor-lamp"); home_advance(800);
            orb_home_view_detail_page(s_home.view, 2); home_advance(800);
        }
        lv_obj_t *list = orb_home_view_scroll(s_home.view);
        if (list) { lv_obj_scroll_to_y(list, 96, LV_ANIM_OFF); home_advance(100); }
        orb_home_view_state_t before, after;
        orb_home_view_save(s_home.view, &before);
        orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
        orb_home_view_restore(s_home.view, &before); home_advance(800);
        orb_home_view_save(s_home.view, &after);
        home_check(before.surface == after.surface && before.offset == after.offset &&
            before.detail_page == after.detail_page &&
            before.scene_index == after.scene_index && before.scroll == after.scroll &&
            before.rooms_scroll == after.rooms_scroll &&
            before.sections == after.sections && before.sensors == after.sensors &&
            before.scene_page == after.scene_page && !strcmp(before.room, after.room) &&
            !strcmp(before.selected, after.selected),
            "temporary benchmark navigation restores Home section, detail and scroll");
        home_check(!orb_home_take_request(&(orb_home_request_t){0}),
            "restoring Home navigation never queues a Home write");
        frozen_frame("restored Home navigation context");
    }
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
}

static void saved_list_anchors(void)
{
    home_busy_room_fixture();
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "copy room-anchor fixture");
    snprintf(snapshot->rooms[1].id, sizeof(snapshot->rooms[1].id), "other");
    snprintf(snapshot->rooms[1].name, sizeof(snapshot->rooms[1].name), "Downstairs");
    for (unsigned i = 12; i < snapshot->accessory_count; ++i)
        snprintf(snapshot->accessories[i].room, sizeof(snapshot->accessories[i].room), "other");
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
        snapshot->scene_count, true, false), "publish two populated rooms");
    free(snapshot); home_advance(800);
    const char *ids[] = {"busy", "other"};
    const int positions[] = {160, 240};
    for (unsigned i = 0; i < 2; ++i) {
        orb_home_view_room(s_home.view, ids[i]); home_advance(800);
        lv_obj_t *list = orb_home_view_scroll(s_home.view);
        home_check(list != NULL, "every room exposes a native anchor owner");
        lv_obj_scroll_to_y(list, positions[i], LV_ANIM_OFF); home_advance(64);
        home_check(orb_home_view_back(s_home.view), "room Back preserves its anchor"); home_advance(800);
    }
    for (unsigned i = 0; i < 2; ++i) {
        orb_home_view_room(s_home.view, ids[i]); home_advance(800);
        home_check(lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == positions[i],
            "revisiting a room restores its independent stable-ID scroll anchor");
        home_check(orb_home_view_back(s_home.view), "restored room returns to Rooms"); home_advance(800);
    }
    orb_home_load_demo(); authoritative_fixture();
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    home_tap_text("Scenes");
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    home_check(list != NULL, "Scenes exposes a native scroll owner");
    lv_obj_scroll_to_y(list, 80, LV_ANIM_OFF); home_advance(64);
    int saved = lv_obj_get_scroll_y(list);
    home_check(saved > 0, "Scenes fixture has a nontrivial reading position");
    home_check(orb_home_view_back(s_home.view), "Scenes Back returns to Rooms"); home_advance(800);
    home_tap_text("Scenes");
    home_check(lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == saved,
        "Scenes Back and re-entry restore the previous reading position");
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "anchor restoration never writes Home");
    orb_home_load_demo(); authoritative_fixture();
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
}

static void polish_contract(void)
{
    orb_home_load_demo(); authoritative_fixture();
    const char *ids[] = {"floor-lamp", "dining", "fan", "climate"};
    for (unsigned i = 0; i < 4; ++i) {
        orb_home_view_open(s_home.view, ids[i]); home_advance(800);
        if (i == 1) { orb_home_view_detail_page(s_home.view, 1); home_advance(800); }
        float value, step;
        home_check(orb_home_view_dial(s_home.view, &value, &step), "adjustable kind exposes its rotary owner");
        home_check(!home_has_text(ORB_ICON_MINUS) && !home_has_text(ORB_ICON_PLUS) &&
            !home_has_text(ORB_ICON_CHEVRON_UP), "dial has no redundant visible adjustment/navigation buttons");
        lv_obj_t *power = identity_disc();
        home_check(power && lv_obj_get_width(power) == ORB_KIT_DISC && lv_obj_get_height(power) == ORB_KIT_DISC,
            "every dial uses the 80-pixel kit glyph power control");
        orb_home_view_rotate(s_home.view, .25f);
        orb_home_view_rotate(s_home.view, .75f);
        home_check(!orb_home_take_request(&(orb_home_request_t){0}), "rotary preview queues no writes");
        orb_home_view_rotary_finish(s_home.view, false); home_advance(1000);
        home_check(!orb_home_take_request(&(orb_home_request_t){0}), "cancelled rotary edit queues no writes");
        frozen_frame("cancelled dial rotary");
        orb_home_view_rotate(s_home.view, .6f);
        orb_home_view_rotary_finish(s_home.view, true); home_advance(800);
        orb_home_request_t request;
        home_check(orb_home_take_request(&request) && !strcmp(request.id, ids[i]), "completed rotary edit queues its accessory once");
        home_check(!orb_home_take_request(&(orb_home_request_t){0}), "no duplicate rotary delivery");
        orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "read optimistic dial authority");
        orb_home_state_t accepted = orb_home_find(snapshot, ids[i])->state;
        free(snapshot);
        orb_home_complete(&request, &accepted, NULL); home_advance(800);
        /* More refreshes than the material's 16 binding slots catch leaked
         * bindings while state changes may replace the identity disc. */
        for (unsigned cycle = 0; cycle < 40; ++cycle) {
            orb_home_state_t update = i == 3 ?
                (orb_home_state_t){.present = ORB_HOME_MODE, .mode = cycle % 2 ? ORB_HOME_MODE_HEAT : ORB_HOME_MODE_OFF} :
                (orb_home_state_t){.present = ORB_HOME_ON, .on = cycle % 2 != 0};
            home_check(orb_home_update(ids[i], &update, true, true, 0), "update authoritative dial power");
            home_advance(40);
            power = identity_disc();
            home_check(power && lv_obj_is_visible(power) && lv_obj_is_clickable(power) &&
                lv_obj_get_width(power) == ORB_KIT_DISC && lv_obj_get_height(power) == ORB_KIT_DISC,
                "kit power disc and its hit target survive repeated authority refresh");
        }
        home_tap(233, 140); home_advance(800);
        if (i < 3) {
            home_check(orb_home_take_request(&request) && !strcmp(request.id, ids[i]) &&
                request.kind == ORB_HOME_REQUEST_TOGGLE,
                "physical power tap still queues the exact accessory after forty authority refreshes");
            home_check(!orb_home_take_request(&(orb_home_request_t){0}), "refreshed power target sends exactly one command");
            orb_home_snapshot_t *confirmed = calloc(1, sizeof(*confirmed));
            home_check(confirmed && orb_home_snapshot(confirmed), "copy refreshed accessory authority");
            const orb_home_accessory_t *current = orb_home_find(confirmed, ids[i]);
            home_check(current != NULL, "refreshed power target retains its accessory identity");
            orb_home_complete(&request, &current->state, NULL); free(confirmed); home_advance(800);
        } else {
            home_check(orb_home_view_scroll(s_home.view) && home_has_text("Heat") &&
                !orb_home_take_request(&(orb_home_request_t){0}),
                "physical thermostat identity tap still opens modes without sending a command");
            home_check(orb_home_view_back(s_home.view), "refreshed mode picker returns to its thermostat"); home_advance(800);
        }
        frozen_frame("retained dial material");
    }
    orb_home_load_demo(); authoritative_fixture();
}

static void long_row_fit_contract(void)
{
    home_long_room_fixture();
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "read long room fixture");
    unsigned rows = 0;
    for (unsigned i = 0; i < snapshot->accessory_count; ++i) {
        const orb_home_accessory_t *a = &snapshot->accessories[i];
        if (strcmp(a->room, "Living Room")) continue;
        char prefix[12]; snprintf(prefix, sizeof(prefix), "%u ", i);
        /* LVGL's DOT mode replaces the displayed label's tail in place. */
        lv_obj_t *name = home_find_text(orb_home_view_root(s_home.view), prefix);
        if (!name) continue;
        lv_obj_t *row = lv_obj_get_parent(name), *state = NULL;
        lv_area_t presented;
        if (!orb_round_list_child_bounds(row, name, &presented)) continue;
        for (unsigned j = 0; j < lv_obj_get_child_count(row); ++j) {
            lv_obj_t *child = lv_obj_get_child(row, j);
            if (child != name && lv_obj_check_type(child, &lv_label_class)) state = child;
        }
        lv_area_t nb, vb, rb;
        home_check(state && lv_label_get_long_mode(name) == LV_LABEL_LONG_DOT, "every long row name ellipsizes");
        lv_obj_get_coords(name, &nb); lv_obj_get_coords(state, &vb); lv_obj_get_coords(row, &rb);
        bool stacked = lv_obj_get_style_text_align(state, 0) == LV_TEXT_ALIGN_LEFT;
        home_check(nb.x1 >= rb.x1 + 16 && vb.x2 <= rb.x2 - 16 &&
            (stacked ? nb.x1 == vb.x1 && nb.y1 < vb.y1 : nb.x2 + 12 <= vb.x1),
            "long name reserves its separate state lane inside the row chord");
        int32_t lines = lv_label_get_max_lines(name);
        home_check(lines >= 1 && lines <= (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? 2 : 1) &&
            lv_label_get_max_lines(state) == 1 &&
            lv_obj_get_height(name) == lines * lv_obj_get_style_text_font(name, 0)->line_height +
                (lines - 1) * lv_obj_get_style_text_line_space(name, 0) &&
            lv_obj_get_height(state) == lv_obj_get_style_text_font(state, 0)->line_height,
            "kit row identity and state have explicit one-line bounds at every content size");
        ++rows;
    }
    printf("Home long room: %u visible rows checked\n", rows);
    home_check(rows >= 2 && rows <= 5, "every rendered room row checked within the round viewport");
    frozen_frame("long room names");

    /* Both real transport states used to overflow the fixed 92px value rail.
     * Centre their canonical row to measure the prepared native lanes, rather
     * than a deliberately compressed edge preview. */
    /* Offline rows keep their cached value; the header says Last known once
     * (polish review #3), so only No response occupies a value lane. */
    const char *states[] = {"No response"};
    char target[33] = "";
    orb_home_state_t current = {0};
    for (unsigned i = 0; i < snapshot->accessory_count; ++i) {
        const orb_home_accessory_t *a = &snapshot->accessories[i];
        if (!strcmp(a->room, "Living Room")) {
            snprintf(target, sizeof(target), "%s", a->id);
            current = a->state;
            break;
        }
    }
    home_check(target[0] && orb_home_update(target, &current, true, false, 0),
        "publish a real unreachable long-name device");
    for (unsigned i = 0; i < sizeof(states) / sizeof(*states); ++i) {
        if (i) orb_home_set_connection(false, false);
        home_advance(800);
        lv_obj_t *list = orb_home_view_scroll(s_home.view);
        lv_obj_t *row = home_find_id(list, target);
        home_check(row != NULL, "transport-state fixture retains its canonical row");
        lv_obj_scroll_to_y(list, LV_MAX(0, lv_obj_get_y(row) -
            (lv_obj_get_height(list) - lv_obj_get_height(row)) / 2), LV_ANIM_OFF); home_advance(100);
        /* Live value ink is a floating list sibling; the row only owns its
         * transparent geometry reservation. Check the actual centred ink. */
        lv_area_t row_bounds;
        home_check(orb_round_list_row_bounds(row, &row_bounds), "transport target has painted row bounds");
        lv_obj_t *value = row_live_text(list, &row_bounds, states[i]);
        home_check(value && !strcmp(lv_label_get_text(value), states[i]),
            "No response remains complete in the actual centred Home value lane");
        lv_point_t required;
        lv_text_get_size(&required, lv_label_get_text(value), lv_obj_get_style_text_font(value, 0),
            lv_obj_get_style_text_letter_space(value, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
        home_check(required.x <= lv_obj_get_width(value) &&
            required.y <= lv_obj_get_height(value) && lv_label_get_max_lines(value) == 1,
            "real transport state fits its bounded lane without wrapping or clipping");
        lv_obj_t *name = lv_obj_get_child(row, 1), *anchor = lv_obj_get_child(row, 2);
        lv_area_t nb, vb;
        home_check(orb_round_list_child_bounds(row, name, &nb) &&
            orb_round_list_child_bounds(row, anchor, &vb),
            "centred transport state exposes its painted name and value lanes");
        bool stacked = lv_obj_get_style_text_align(value, 0) == LV_TEXT_ALIGN_LEFT;
        /* Stacked line boxes include leading below the title's ink. Keep
         * their shared left edge and text order; inline rails stay disjoint. */
        home_check(stacked ? nb.x1 == vb.x1 && nb.y1 < vb.y1 : nb.x2 < vb.x1,
            "complete transport state keeps its separate lane beside or below the device name");
    }
    orb_home_set_connection(false, false); home_advance(800);
    lv_obj_t *offline_row = home_find_id(orb_home_view_scroll(s_home.view), target);
    home_check(home_has_text("Last known \u00b7 Offline") && offline_row && !home_find_text(offline_row, "Last known"),
        "offline states Last known once in the header and keeps the row's identity and value");
    free(snapshot);
    orb_home_load_demo(); authoritative_fixture();
}

static lv_obj_t *room_row_id(lv_obj_t *list, const char *id)
{
    for (unsigned i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_obj_t *row = lv_obj_get_child(list, i);
        const char *row_id = lv_obj_get_user_data(row);
        if (row_id && !strcmp(row_id, id)) return row;
    }
    return NULL;
}

static void busy_room_continuation(void)
{
    home_busy_room_fixture();
    home_check(home_has_text("Upstairs"), "room remains discoverable without favourites");
    orb_home_view_room(s_home.view, "busy"); home_advance(800);
    lv_obj_t *title = home_find_rim_title(orb_home_view_root(s_home.view), "Upstairs");
    home_check(title != NULL, "long room title uses the shared curved identity component");
    orb_kit_rim_text_info_t info;
    home_check(orb_kit_rim_text_info(title, &info) && info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES,
        "room rim title stays inside the shared 120-degree span");
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    lv_area_t title_bounds, list_bounds;
    lv_obj_get_coords(title, &title_bounds); lv_obj_get_coords(list, &list_bounds);
    home_check(title_bounds.y2 + 12 < list_bounds.y1,
        "long room rim title clears the opaque native list viewport by twelve pixels");
    unsigned row_count = 0;
    for (unsigned i = 0; list && i < lv_obj_get_child_count(list); ++i)
        if (!lv_obj_is_floating(lv_obj_get_child(list, i))) ++row_count;
    home_check(list && row_count == 25,
        "all 24 accessory rows and the final All off row belong to their native scroll owner");
    round_list_metrics(list);
    for (unsigned i = 0; i < 24; ++i) {
        char prefix[8]; snprintf(prefix, sizeof(prefix), "D%02u", i + 1);
        char id[16]; snprintf(id, sizeof(id), "busy-%02u", i + 1);
        list = orb_home_view_scroll(s_home.view);
        lv_obj_t *action = room_row_id(list, id);
        home_check(action != NULL, "continuous room contains every stable device row");
        int maximum = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
        lv_obj_scroll_to_y(list, LV_CLAMP(0, lv_obj_get_y(action), maximum), LV_ANIM_OFF);
        home_advance(800);
        lv_obj_t *label = home_find_text(action, prefix);
        home_check(label != NULL, "scrolling exposes every device's matching name");
        lv_area_t presented;
        home_check(orb_round_list_child_bounds(action, label, &presented),
            "native scrolling renders each of the 24 device labels");
        int position = lv_obj_get_scroll_y(list);
        /* Long press is the room's detail route for simple switches as well
         * as dials, so browsing never changes an accessory's state. */
        lv_obj_send_event(action, LV_EVENT_LONG_PRESSED, NULL); home_advance(800);
        orb_home_view_state_t detail;
        orb_home_view_save(s_home.view, &detail);
        home_check(!strcmp(detail.selected, id), "every named row opens its own stable device ID");
        home_check(orb_home_view_back(s_home.view), "busy-room detail returns one level"); home_advance(800);
        home_check(home_has_text(prefix) &&
            lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == position,
            "Back restores the same busy-room device and native scroll position");
    }
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "browsing a busy room never actuates devices");
    frozen_frame("busy-room final row");

    /* A synthetic pointer can navigate, but cannot toggle a room row. */
    orb_home_view_room(s_home.view, "busy"); home_advance(800);
    list = orb_home_view_scroll(s_home.view);
    lv_obj_t *switch_row = room_row_id(list, "busy-21");
    home_check(switch_row != NULL, "native list retains the safe switch target");
    lv_obj_scroll_to_y(list, lv_obj_get_y(switch_row), LV_ANIM_OFF);
    home_advance(800);
    home_check(home_has_text("D21"), "switch row is reachable in busy room");
    void *original_data = lv_indev_get_user_data(s_home.pointer);
    orb_input_mark_synthetic(s_home.pointer);
    home_tap_text("D21");
    lv_indev_set_user_data(s_home.pointer, original_data); home_advance(1600);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "synthetic room-row tap cannot write Home");

    orb_home_view_room(s_home.view, "empty"); home_advance(800);
    home_check(home_has_text("Spare") && home_has_text("No accessories") &&
        !home_find_action(orb_home_view_root(s_home.view), "More Devices"),
        "empty room has identity without a dead continuation control");
    quiet("empty room");
    orb_home_load_demo(); authoritative_fixture();
}

static void embedded_busy_room_navigation(void)
{
    home_busy_room_fixture();
    orb_home_view_system_navigation(s_home.view, true);
    orb_home_view_room(s_home.view, "busy"); home_advance(800);
    home_check(!orb_home_view_dial(s_home.view, NULL, NULL),
        "embedded busy room exposes its native list instead of a paged value owner");
    orb_home_view_rotary(s_home.view, 150);
    orb_home_view_rotary_finish(s_home.view, true); home_advance(800);
    lv_obj_t *scroll_owner = orb_home_view_scroll(s_home.view);
    home_check(scroll_owner && lv_obj_get_scroll_y(scroll_owner) > 0,
        "rotary scroll reports a position so shell downswipe stays local");
    orb_home_view_rotary(s_home.view, -10000);
    orb_home_view_rotary_finish(s_home.view, true); home_advance(800);
    scroll_owner = orb_home_view_scroll(s_home.view);
    home_check(scroll_owner && lv_obj_get_scroll_y(scroll_owner) == 0,
        "first room row reports the top position to shell navigation");
    home_press(233, 365); home_advance(16); home_press(233, 190); home_advance(16);
    home_release(); home_advance(800);
    home_check(lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) > 0,
        "embedded vertical room swipe stays with the room's native scroll owner");
    orb_home_view_state_t before, after;
    orb_home_view_save(s_home.view, &before);
    home_press(330, 233); home_advance(16); home_press(140, 233); home_advance(16);
    home_release(); home_advance(800);
    orb_home_view_save(s_home.view, &after);
    home_check(before.scroll == after.scroll && !after.selected[0] && !orb_home_view_moving(s_home.view),
        "embedded horizontal room drag neither scrolls locally nor opens a device");
    orb_home_view_rotary(s_home.view, 10000);
    orb_home_view_rotary_finish(s_home.view, true); home_advance(800);
    home_check(lv_obj_get_scroll_bottom(orb_home_view_scroll(s_home.view)) <= 1,
        "shell rotary can reach the final busy-room row");
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "embedded room navigation never writes Home");
    orb_home_view_system_navigation(s_home.view, false);
    orb_home_load_demo(); authoritative_fixture();
}

static void scene_feedback_and_provenance(void)
{
    orb_home_load_demo(); authoritative_fixture();
    orb_home_view_hour(s_home.view, 9);
    orb_home_request_t request;
    home_check(orb_home_toggle("floor-lamp") && orb_home_take_request(&request), "queue unrelated device failure fixture");
    orb_home_complete(&request, NULL, "unreachable");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    void *original_data = lv_indev_get_user_data(s_home.pointer);
    orb_input_mark_synthetic(s_home.pointer);
    home_tap_text("Scenes"); home_tap_text("Good morning");
    lv_indev_set_user_data(s_home.pointer, original_data); home_advance(1600);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "synthetic scene capsule cannot run a scene");
    home_check(!home_has_text("Couldn’t run") && !home_has_text("Requested"),
        "unrelated accessory failure never becomes a scene failure or receipt");
    orb_input_mark_synthetic(s_home.pointer);
    home_tap_text("Good morning");
    lv_indev_set_user_data(s_home.pointer, original_data); home_advance(1600);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "synthetic scene-list tap cannot run a scene");
    home_tap_text("Good morning");
    home_check(orb_home_take_request(&request) && request.kind == ORB_HOME_REQUEST_SCENE &&
        !strcmp(request.id, "morning"), "physical scene-list tap queues the displayed safe scene");
    home_check(home_has_text("Running") || home_has_text("Running"), "scene has visible in-flight feedback");
    home_tap_text("Good morning");
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "pending scene cannot be submitted twice");
    orb_home_complete(&request, NULL, "unreachable"); home_advance(800);
    home_check(!home_has_text("Requested") && home_has_text("Couldn’t run") && home_has_text("Good morning"),
        "rejected scene never displays a success receipt");
    home_tap_text("Good morning");
    home_check(orb_home_take_request(&request), "failed scene can be retried deliberately");
    orb_home_complete(&request, NULL, NULL); home_advance(800);
    home_check(home_has_text("Requested"), "completed scene reports the relay receipt");
    frozen_frame("scene completion receipt");
    orb_home_load_demo(); authoritative_fixture();
}

static void room_all_off_contract(void)
{
    for (unsigned fail = 0; fail < 2; ++fail) {
        home_busy_room_fixture();
        orb_home_view_destroy(s_home.view);
        s_home.view = orb_home_view_create(lv_screen_active(), &(orb_home_commands_t){page_command, NULL});
        home_advance(800);
        orb_home_view_room(s_home.view, "busy"); home_advance(800);
        lv_obj_t *title = home_find_rim_title(orb_home_view_root(s_home.view), "Upstairs");
        home_check(title && !lv_obj_is_clickable(title), "room rim title is passive identity");
        home_tap(233, 233 - ORB_KIT_TITLE_RADIUS - 8); home_advance(800);
        orb_home_view_state_t state; orb_home_view_save(s_home.view, &state);
        home_check(!state.sections && state.surface == ORB_HOME_ROOM && !state.selected[0],
            "room rim title tap leaves its devices in place");
        lv_obj_t *list = orb_home_view_scroll(s_home.view);
        lv_obj_t *row = NULL;
        for (unsigned i = 0; i < lv_obj_get_child_count(list); i++)
            if (!lv_obj_is_floating(lv_obj_get_child(list, i))) row = lv_obj_get_child(list, i);
        lv_obj_t *label = row ? lv_obj_get_child(row, 1) : NULL;
        home_check(row && lv_obj_get_parent(row) == list && !orb_kit_is_primary(row) && label &&
            lv_obj_check_type(label, &lv_label_class) && !strcmp(lv_label_get_text(label), "All off"),
            "switchable room exposes All off as its final named navigation row");
        orb_home_view_rotary(s_home.view, 10000);
        orb_home_view_rotary_finish(s_home.view, true); home_advance(800);
        lv_area_t viewport, bounds, ink;
        lv_obj_get_coords(list, &viewport);
        label = home_find_text(row, "All off");
        home_check(orb_round_list_row_bounds(row, &bounds) && bounds.y1 >= viewport.y1 &&
            bounds.y2 <= viewport.y2 && label && orb_round_list_child_bounds(row, label, &ink) &&
            ink.y1 >= viewport.y1 && ink.y2 <= viewport.y2,
            "room can scroll the entire All off row and its text into view at this text size");
        home_check(!orb_home_take_request(&(orb_home_request_t){0}), "reaching All off never writes an accessory");
        s_page_commands = 0;
        home_tap_text("All off");
        orb_home_view_save(s_home.view, &state);
        home_check(state.sections && !state.selected[0] && state.surface == ORB_HOME_ROOM &&
            !orb_home_view_scroll(s_home.view) && !s_page_commands &&
            !orb_home_take_request(&(orb_home_request_t){0}),
            "All off row opens room_actions without a presenter command or model request");
        lv_obj_t *all_off = home_find_action(orb_home_view_root(s_home.view), "All off");
        home_check(all_off && orb_kit_is_primary(all_off) &&
            primary_actions(orb_home_view_root(s_home.view)) == 1 &&
            lv_obj_get_height(all_off) == ORB_KIT_PRIMARY_H &&
            lv_obj_get_y(all_off) + lv_obj_get_height(all_off) / 2 == 380,
            "room All off is the single kit primary on the bottom arc");
        if (!fail) {
            void *original_data = lv_indev_get_user_data(s_home.pointer);
            orb_input_mark_synthetic(s_home.pointer);
            home_tap_text("All off");
            lv_indev_set_user_data(s_home.pointer, original_data); home_advance(1600);
            home_check(!orb_home_take_request(&(orb_home_request_t){0}), "synthetic All off cannot control a room");
        }
        home_tap_text("All off");
        home_check(home_has_text("Turning off"), "room All off has visible in-flight feedback");
        unsigned delivered = 0;
        orb_home_request_t request;
        while (orb_home_take_request(&request)) {
            home_check(request.kind == ORB_HOME_REQUEST_SET && request.values.present == ORB_HOME_ON &&
                !request.values.on && strcmp(request.id, "busy-23") && strcmp(request.id, "busy-24"),
                "room All off sets explicit off only on lights and switches");
            orb_home_complete(&request, &request.values, fail && delivered == 0 ? "rejected" : NULL);
            ++delivered;
            home_check(delivered <= 22, "All off never retries a completed provider write");
        }
        home_advance(800);
        orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "read complete room All off result");
        home_check(delivered == 22 && snapshot->room_off_total == 22 && snapshot->room_off_completed == 22 &&
            !snapshot->room_off_pending && snapshot->room_off_failed == fail,
            "bounded four-request queue reaches every safe device and retains failures");
        const orb_home_accessory_t *lock = orb_home_find(snapshot, "busy-23");
        home_check(lock && lock->state.locked && !lock->pending, "room All off never affects the lock");
        home_check(home_has_text(fail ? ORB_ICON_WARNING_FILL " Couldn’t turn off some accessories" : "All off requested"),
            "room receipt distinguishes complete delivery from partial failure");
        lv_obj_t *notice = home_find_text(orb_home_view_root(s_home.view), fail ? ORB_ICON_WARNING_FILL " Couldn’t turn off some accessories" : "All off requested");
        lv_area_t notice_bounds;
        home_check(notice != NULL, "room receipt remains readable");
        lv_obj_get_coords(notice, &notice_bounds);
        home_check(notice_bounds.y2 < 440 && lv_obj_get_width(notice) <=
            orb_chord_width(notice_bounds.y1, notice_bounds.y2, ORB_RIM_MARGIN_TEXT),
            "room receipt stays inside its round safe area at AX3");
        home_check(!home_find_action(orb_home_view_root(s_home.view), "Devices"),
            "room action sheet uses physical Back without a redundant navigation button");
        free(snapshot);
        frozen_frame(fail ? "room All off partial failure" : "room All off completion");
        home_check(orb_home_view_back(s_home.view), "All off page Back returns to its room"); home_advance(800);
        home_check(orb_home_view_scroll(s_home.view) &&
            lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == state.room_scroll,
            "All off page Back preserves the room's scrolled final-row position");
    }
    orb_home_view_room(s_home.view, "empty"); home_advance(800);
    home_check(!home_find_action(orb_home_view_root(s_home.view), "All off"),
        "empty room omits the All off row");
    orb_home_room_t room = {.id = "read-only", .name = "Sensors"};
    orb_home_accessory_t accessories[] = {
        {.id = "only-fan", .room = "read-only", .name = "Fan", .kind = ORB_HOME_FAN, .reachable = true},
        {.id = "only-sensor", .room = "read-only", .name = "Sensor", .kind = ORB_HOME_TEMPERATURE, .reachable = true},
    };
    home_check(orb_home_snapshot_chunk(&room, 1, accessories, 2, NULL, 0, true, false),
        "publish room without lights, switches or outlets"); home_advance(800);
    orb_home_view_room(s_home.view, "read-only"); home_advance(800);
    home_check(home_has_text("Fan") && content_row_count(orb_home_view_scroll(s_home.view)) == 2 &&
        !home_find_action(orb_home_view_root(s_home.view), "All off"),
        "room with only a fan and sensor omits the All off row");
    orb_home_load_demo(); authoritative_fixture();
}

static void thermostat_modes_and_sensor_age(void)
{
    orb_home_load_demo(); authoritative_fixture();
    orb_prefs_t prefs;
    orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_F;
    home_check(orb_model_set_prefs(&prefs), "mode test uses owner Fahrenheit preference");
    orb_home_view_open(s_home.view, "climate"); home_advance(800);
    /* The thermostat's identity disc opens its mode list. */
    home_tap(233, 140); home_advance(400);
    home_check(home_has_text("Auto") && home_has_text("Cool"), "identity disc discovers every thermostat mode");
    void *original_data = lv_indev_get_user_data(s_home.pointer);
    orb_input_mark_synthetic(s_home.pointer);
    home_tap_text("Cool");
    lv_indev_set_user_data(s_home.pointer, original_data); home_advance(1600);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "synthetic mode tap cannot change thermostat");
    home_tap_text("Cool");
    orb_home_request_t request;
    home_check(orb_home_take_request(&request) && request.values.present == ORB_HOME_MODE &&
        request.values.mode == ORB_HOME_MODE_COOL, "physical mode tap queues only its selected mode");
    orb_home_complete(&request, NULL, "rejected"); home_advance(800);
    home_check(home_has_text("72°") && home_has_text("Couldn’t update"),
        "rejected mode preserves the target temperature beside the failure receipt");
    home_advance(3600);
    home_check(!home_has_text("Couldn’t update") && home_has_text("72°") && home_has_text("Heat · Now"),
        "brief thermostat failure clears back to confirmed mode and target");
    quiet("mode failure settled");
    time_t now = time(NULL);
    orb_home_state_t temperature = {.present = ORB_HOME_TEMP, .temp = 20};
    home_check(orb_home_update("bed-temp", &temperature, false, true, (uint64_t)now + 60), "publish sensor with unknown clock age");
    orb_home_view_open(s_home.view, "bed-temp"); home_advance(800);
    home_check(!home_has_text("Updated") && !home_has_text("Age unknown"), "a healthy sensor never invents or recites an update time");
    home_check(orb_home_update("bed-temp", &temperature, true, false, (uint64_t)now - 125), "report timestamped stale sensor state");
    home_advance(800);
    home_check(home_has_text("Updated 2 min ago"), "a stale sensor shows the reported update age");
    frozen_frame("timestamped sensor");
    home_check(orb_home_update("bed-temp", &temperature, false, true, (uint64_t)now + 60), "report future-skewed timestamp");
    home_advance(800);
    home_check(home_has_text("Last known") && !home_has_text("Updated"), "future clock skew never becomes a negative age");
    prefs.temperature = ORB_TEMPERATURE_C;
    home_check(orb_model_set_prefs(&prefs), "restore Celsius test fixture");
    orb_home_load_demo(); authoritative_fixture();
}

static void unrelated_home_update_keeps_room(void)
{
    orb_home_load_demo(); authoritative_fixture();
    orb_home_view_room(s_home.view, "Living Room"); home_advance(800);
    lv_obj_t *row = home_find_action(orb_home_view_root(s_home.view), "Floor Lamp");
    home_check(row != NULL, "retain named row before another room changes");
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "copy unrelated Home update fixture");
    const orb_home_accessory_t *other = NULL;
    for (unsigned i = 0; i < snapshot->accessory_count; i++) {
        if (strcmp(snapshot->accessories[i].room, "Living Room") &&
            (snapshot->accessories[i].state.present & ORB_HOME_ON)) { other = &snapshot->accessories[i]; break; }
    }
    home_check(other != NULL, "fixture contains another room's power device");
    home_measure_start();
    home_check(orb_home_update(other->id, &(orb_home_state_t){.present=ORB_HOME_ON,.on=!other->state.on}, false, true, 0),
        "publish another room's live state");
    home_advance(800); home_measure_stop();
    home_check(lv_obj_is_valid(row) && home_find_action(orb_home_view_root(s_home.view), "Floor Lamp") == row,
        "another room's Home event preserves the displayed row and its press target");
    home_check(!s_home.frames && !s_home.damage_events, "another room's state causes zero displayed-room redraw work");
    free(snapshot);
    orb_home_load_demo(); authoritative_fixture();
}

static void thermostat_room_live_range(void)
{
    orb_home_load_demo(); authoritative_fixture();
    orb_prefs_t prefs;
    orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_F;
    home_check(orb_model_set_prefs(&prefs), "thermostat room test uses Fahrenheit");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    orb_home_view_room(s_home.view, "Living Room"); home_advance(800);
    /* Room navigation correctly restores prior scroll. This fixture owns the
     * first row, so explicitly expose it instead of depending on test order. */
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); home_advance(800);
    lv_obj_t *root = orb_home_view_root(s_home.view);
    lv_obj_t *label = home_find_text(root, "71°");
    orb_kit_value_info_t before;
    home_check(label && orb_kit_value_info(label, &before), "thermostat current reading owns a live room rail");
    orb_round_list_cache_stats_t cache_before, cache_after;
    home_check(orb_round_list_cache_stats(list, &cache_before), "room exposes retained row-cache diagnostics");
    const int readings[] = {-5, 100, -148, 212};
    for (unsigned i = 0; i < sizeof(readings) / sizeof(*readings); ++i) {
        float celsius = (readings[i] - 32) * 5.f / 9.f;
        home_check(orb_home_update("climate", &(orb_home_state_t){.present = ORB_HOME_TEMP, .temp = celsius},
            false, true, 0), "publish valid negative/three-digit thermostat reading");
        home_advance(32);
        char expected[24]; snprintf(expected, sizeof(expected), "%s%d°", readings[i] < 0 ? "\u2212" : "", abs(readings[i]));
        orb_kit_value_info_t after;
        home_check(lv_obj_is_valid(label) && home_find_text(root, expected) == label &&
            orb_kit_value_info(label, &after) && after.tabular && before.width == after.width &&
            before.template_measures == after.template_measures && before.raster_prepares == after.raster_prepares,
            "valid temperature extremes retain one tabular lane without measuring or preparing per patch");
        lv_point_t text_size;
        lv_text_get_size(&text_size, expected, lv_obj_get_style_text_font(label, 0),
            lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
        home_check(text_size.x <= lv_obj_get_width(label), "complete thermostat reading fits the actual live row lane");
        home_check(orb_round_list_cache_stats(list, &cache_after) &&
            cache_after.source_builds == cache_before.source_builds,
            "temperature telemetry never rebuilds the room row sources");
    }
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "copy long-room thermostat fixture");
    for (unsigned i = 0; i < snapshot->room_count; ++i)
        snprintf(snapshot->rooms[i].name, sizeof(snapshot->rooms[i].name),
            "Living Room and Downstairs Study");
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count,
        snapshot->accessories, snapshot->accessory_count, snapshot->scenes,
        snapshot->scene_count, true, false), "publish long-room thermostat context");
    free(snapshot);
    orb_home_view_open(s_home.view, "climate"); home_advance(800);
    label = home_find_text(orb_home_view_root(s_home.view), "Heat · Now 212°");
    home_check(label != NULL,
        "long room name never corrupts the complete thermostat context");
    for (unsigned i = 0; i < sizeof(readings) / sizeof(*readings); ++i) {
        home_check(orb_home_update("climate", &(orb_home_state_t){.present = ORB_HOME_TEMP,
            .temp = (readings[i] - 32) * 5.f / 9.f}, false, true, 0),
            "patch thermostat detail current reading");
        home_advance(32);
        char expected[48]; snprintf(expected, sizeof(expected), "Heat · Now %s%d°", readings[i] < 0 ? "\u2212" : "", abs(readings[i]));
        home_check(lv_obj_is_valid(label) && !strcmp(lv_label_get_text(label), expected) &&
            lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_WRAP,
            "thermostat detail retains its one context lane with clean, never-dotted text");
        lv_point_t text_size;
        lv_text_get_size(&text_size, expected, lv_obj_get_style_text_font(label, 0),
            lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
        home_check(text_size.x <= lv_obj_get_width(label), "complete thermostat context fits its live lane");
    }
    prefs.temperature = ORB_TEMPERATURE_C;
    home_check(orb_model_set_prefs(&prefs), "restore Celsius fixture");
    orb_home_load_demo(); authoritative_fixture();
}

/* QA sweep: use a tiny authoritative Home to keep source identity, stale
 * values and dynamic reading membership observable through real pointers. */
static void qa_capture(const char *name)
{
    const char *directory = getenv("ORB_HOME_QA_SHOTS");
    if (!directory) return;
    char path[512]; snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    home_check(sim_display_save_png(path), "capture Home QA design evidence");
}

static orb_home_snapshot_t *climate_qa_fixture(void)
{
    orb_home_snapshot_t *s = calloc(1, sizeof(*s));
    home_check(s != NULL, "allocate QA Home fixture");
    s->room_count = 1;
    strcpy(s->rooms[0].id, "qa-room"); strcpy(s->rooms[0].name, "Reading Room");
    s->accessory_count = 2;
    for (unsigned i = 0; i < 2; i++) {
        orb_home_accessory_t *a = &s->accessories[i];
        snprintf(a->id, sizeof(a->id), "qa-thermostat-%u", i);
        snprintf(a->name, sizeof(a->name), "%s Thermostat", i ? "Second" : "First");
        strcpy(a->room, "qa-room"); a->kind = ORB_HOME_THERMOSTAT; a->reachable = true;
        a->writable = ORB_HOME_TARGET | ORB_HOME_MODE;
        a->state = (orb_home_state_t){.present = ORB_HOME_TEMP | ORB_HOME_TARGET | ORB_HOME_MODE,
            .temp = i ? 30 : 21, .target = i ? 29 : 22, .mode = ORB_HOME_MODE_HEAT};
    }
    return s;
}

static void climate_qa_publish(const orb_home_snapshot_t *s)
{
    home_check(orb_home_snapshot_chunk(s->rooms, s->room_count, s->accessories,
        s->accessory_count, s->scenes, s->scene_count, true, false), "publish QA Home snapshot");
    orb_home_set_connection(true, true); home_advance(800);
}

static unsigned climate_qa_rows(void)
{
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    unsigned n = 0;
    home_check(list != NULL, "room sensors expose their list");
    for (unsigned i = 0; i < lv_obj_get_child_count(list); i++)
        n += !lv_obj_is_floating(lv_obj_get_child(list, i));
    return n;
}

static void climate_source_and_staleness(void)
{
    orb_prefs_t prefs; orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_C;
    home_check(orb_model_set_prefs(&prefs), "QA climate uses Celsius");
    orb_home_snapshot_t *s = climate_qa_fixture(); climate_qa_publish(s);
    orb_home_view_select(s_home.view, ORB_HOME_CLIMATE); home_advance(800);
    home_check(home_has_text("21°") && home_has_text("Heating to 22°"),
        "Climate summary and its action refer to the same first thermostat");
    qa_capture("climate-source");
    home_tap_text("First"); /* the row is named after its accessory; AX3 may elide its tail */
    orb_home_view_state_t context; orb_home_view_save(s_home.view, &context);
    home_check(!strcmp(context.selected, "qa-thermostat-0"), "Climate action opens the displayed source");
    home_check(orb_home_view_back(s_home.view), "return from QA thermostat"); home_advance(800);
    s->accessory_count = 1; climate_qa_publish(s);
    home_check(orb_home_update("qa-thermostat-0", &(orb_home_state_t){0}, true, false, 0), "report unreachable thermostat");
    home_advance(800);
    home_check(home_has_text("No response"), "unreachable Climate reading has visible stale feedback");
    lv_obj_t *stale = home_find_text(orb_home_view_root(s_home.view), "No response");
    lv_point_t needed;
    lv_text_get_size(&needed, lv_label_get_text(stale), lv_obj_get_style_text_font(stale, 0),
        lv_obj_get_style_text_letter_space(stale, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    home_check(needed.x <= lv_obj_get_width(stale), "complete Climate reachability phrase fits its actual lane at AX3");
    frozen_frame("Climate reachability update");
    qa_capture("climate-unreachable");
    home_tap_text("Sensors");
    home_check(home_has_text("No response"), "unreachable room reading never looks fresh");
    qa_capture("sensors-unreachable");
    s->accessory_count = 1; s->accessories[0].kind = ORB_HOME_SENSOR;
    s->accessories[0].state = (orb_home_state_t){.present = ORB_HOME_TEMP, .temp = 21};
    climate_qa_publish(s);
    home_check(climate_qa_rows() == 1, "one reported temperature creates one room-sensor row");
    home_check(orb_home_update("qa-thermostat-0", &(orb_home_state_t){
        .present = ORB_HOME_HUMIDITY_VALUE, .humidity = 45}, false, true, 0), "add humidity to an existing sensor");
    home_advance(800);
    home_check(climate_qa_rows() == 2 && home_has_text("45%"),
        "newly reported quantity becomes discoverable without reopening Room Sensors");
    frozen_frame("Room sensor membership update");
    qa_capture("sensors-new-quantity");
    s->accessories[0].state = (orb_home_state_t){.present = ORB_HOME_HUMIDITY_VALUE, .humidity = 46};
    climate_qa_publish(s);
    home_check(climate_qa_rows() == 1 && home_has_text("46%"), "removed quantity retires its stale row");
    free(s); orb_home_load_demo(); authoritative_fixture();
}

static void authoritative_target_extremes(void)
{
    orb_home_snapshot_t *s = climate_qa_fixture(); s->accessory_count = 1;
    s->accessories[0].state.target = -20.5f; climate_qa_publish(s);
    orb_home_view_open(s_home.view, "qa-thermostat-0"); home_advance(800);
    home_check(home_has_text("\u221220.5°"),
        "thermostat shows authoritative target outside its editable dial range");
    qa_capture("thermostat-extreme-celsius");
    orb_prefs_t prefs; orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_F;
    home_check(orb_model_set_prefs(&prefs), "QA target changes to Fahrenheit"); home_advance(800);
    home_check(home_has_text("\u22125°"), "out-of-range target uses actual converted units");
    home_check(orb_home_update("qa-thermostat-0", &(orb_home_state_t){
        .present = ORB_HOME_TARGET, .target = 100}, false, true, 0), "report high valid target");
    home_advance(1600); home_check(home_has_text("212°"), "large authoritative target is not silently clamped to 90°F");
    qa_capture("thermostat-extreme-fahrenheit");
    free(s); prefs.temperature = ORB_TEMPERATURE_C; home_check(orb_model_set_prefs(&prefs), "restore Celsius");
    orb_home_load_demo(); authoritative_fixture();
}

static void unlock_hold_lifecycle(void)
{
    orb_home_snapshot_t *s = climate_qa_fixture(); s->accessory_count = 1;
    orb_home_accessory_t *a = &s->accessories[0]; a->kind = ORB_HOME_LOCK;
    strcpy(a->name, "QA Front Door"); a->writable = ORB_HOME_LOCKED;
    a->state = (orb_home_state_t){.present = ORB_HOME_LOCKED, .locked = true};
    for (unsigned boundary = 0; boundary < 2; boundary++) {
        climate_qa_publish(s); orb_home_view_open(s_home.view, a->id); home_advance(800);
        home_press(233, 140); home_advance(500);
        home_check(home_has_text("Keep holding"), "physical unlock hold is in progress");
        if (boundary) { orb_home_set_connection(false, false); orb_home_set_connection(true, false); }
        /* No UI poll between the loss/reset and its complete replacement. */
        home_check(orb_home_snapshot_chunk(s->rooms, s->room_count, s->accessories,
            s->accessory_count, NULL, 0, true, false), "replace Home during a held contact");
        orb_home_set_connection(true, true); home_advance(600); home_release(); home_advance(100);
        home_check(!orb_home_take_request(&(orb_home_request_t){0}),
            "unlock hold is retired across same-ID snapshot or fast reconnect");
    }
    a->writable = 0; climate_qa_publish(s); home_advance(800);
    home_check(!home_has_text("Hold to unlock") && home_has_text("Read-only"),
        "read-only locks do not advertise an unlock affordance");
    qa_capture("lock-readonly");
    home_press(233, 140); home_advance(1100); home_release(); home_advance(100);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "read-only lock cannot acquire an unlock hold");
    a->kind = ORB_HOME_LIGHT; a->writable = ORB_HOME_BRIGHTNESS;
    a->state = (orb_home_state_t){.present = ORB_HOME_ON | ORB_HOME_BRIGHTNESS, .on = true, .brightness = 50};
    climate_qa_publish(s); home_advance(800);
    lv_obj_t *power = identity_disc();
    home_check(power && lv_obj_has_state(power, LV_STATE_DISABLED),
        "a readable but nonwritable power field never advertises an active toggle");
    free(s); orb_home_load_demo(); authoritative_fixture();
}

static void live_scene_identity(void)
{
    orb_home_snapshot_t *s = climate_qa_fixture(); s->accessory_count = 1;
    s->accessories[0].kind = ORB_HOME_LIGHT; s->accessories[0].writable = ORB_HOME_ON;
    s->accessories[0].state = (orb_home_state_t){.present = ORB_HOME_ON, .on = true};
    s->scene_count = 2;
    strcpy(s->scenes[0].id, "scene-first"); strcpy(s->scenes[0].name, "Reading");
    strcpy(s->scenes[1].id, "scene-second"); strcpy(s->scenes[1].name, "Evening");
    climate_qa_publish(s); orb_home_view_open(s_home.view, s->accessories[0].id); home_advance(800);
    orb_home_view_detail_page(s_home.view, 2); home_advance(800);
    home_check(home_has_text("Evening") && home_has_text("Reading"), "the light's scenes are one list, not a pager");
    orb_home_scene_t first = s->scenes[0]; s->scenes[0] = s->scenes[1]; s->scenes[1] = first;
    climate_qa_publish(s);
    home_check(home_has_text("Evening") && home_has_text("Reading"),
        "live scene reorder keeps every scene listed");
    home_tap_text("Evening"); orb_home_request_t request;
    home_check(orb_home_take_request(&request) && !strcmp(request.id, "scene-second"),
        "physical scene action still targets the same visible scene after reordering");
    orb_home_complete(&request, NULL, NULL); home_advance(800);
    s->scenes[0].contains_unlock = true; climate_qa_publish(s);
    home_check(!home_has_text("Evening") && home_has_text("Reading"),
        "new safety metadata retires the unlock scene immediately");
    free(s); orb_home_load_demo(); authoritative_fixture();
}

static void cancelled_list_contact(void)
{
    orb_home_view_system_navigation(s_home.view, false);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    home_press(330, 233); home_advance(16); home_press(120, 233); home_advance(16);
    orb_home_view_rotary_finish(s_home.view, false);
    lv_indev_reset(s_home.pointer, NULL); home_release(); home_advance(1800);
    orb_home_view_state_t state; orb_home_view_save(s_home.view, &state);
    home_check(state.surface == ORB_HOME_ROOMS && !orb_home_view_moving(s_home.view),
        "cancelled horizontal list contact never changes page");
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "cancelled list contact never writes Home");
}

static void rejected_pointer_preview(void)
{
    home_busy_room_fixture(); orb_home_view_system_navigation(s_home.view, false);
    orb_home_view_open(s_home.view, "busy-01"); home_advance(800);
    for (unsigned i = 2; i <= 5; i++) {
        char id[24]; snprintf(id, sizeof(id), "busy-%02u", i);
        home_check(orb_home_set(id, &(orb_home_state_t){.present = ORB_HOME_ON, .on = false}, false),
            "occupy each bounded pending-intent slot");
    }
    home_advance(800); home_press(61, 355); home_advance(32);
    home_press(405, 355); home_advance(32); home_release(); home_advance(1800);
    home_check(home_has_text("70%") && home_has_text("Home is busy"),
        "rejected pointer commit restores authority and supplies explicit busy feedback");
    orb_home_request_t request;
    home_check(orb_home_take_request(&request), "free one pending slot for an explicit retry");
    orb_home_complete(&request, &request.values, NULL); home_advance(100);
    home_press(61, 355); home_advance(32); home_press(405, 355); home_advance(32);
    home_release(); home_advance(800);
    home_check(!home_has_text("Home is busy"), "explicit retry clears the queue rejection");
    unsigned taken = 0, retried = 0;
    while (orb_home_take_request(&request)) {
        retried += !strcmp(request.id, "busy-01");
        orb_home_complete(&request, &request.values, NULL); taken++;
    }
    home_check(taken == 4 && retried == 1, "only one deliberate retry is accepted after capacity returns");
    orb_home_load_demo(); authoritative_fixture();
}

static void rejected_scene_feedback(void)
{
    orb_home_snapshot_t *s = climate_qa_fixture(); s->accessory_count = 5; s->scene_count = 1;
    strcpy(s->scenes[0].id, "qa-scene"); strcpy(s->scenes[0].name, "Reading");
    for (unsigned i = 0; i < 5; i++) {
        orb_home_accessory_t *a = &s->accessories[i];
        snprintf(a->id, sizeof(a->id), "qa-light-%u", i); strcpy(a->name, "Reading Lamp");
        strcpy(a->room, "qa-room"); a->kind = ORB_HOME_LIGHT; a->reachable = true; a->writable = ORB_HOME_ON;
        a->state = (orb_home_state_t){.present = ORB_HOME_ON, .on = true};
    }
    climate_qa_publish(s); orb_home_view_open(s_home.view, "qa-light-0"); home_advance(800);
    orb_home_view_detail_page(s_home.view, 2); home_advance(800);
    for (unsigned i = 1; i < 5; i++) home_check(orb_home_toggle(s->accessories[i].id), "fill scene-request queue");
    home_advance(800); home_tap_text("Reading");
    home_check(home_has_text("Home is busy") && !home_has_text("Requested"),
        "scene rejected before admission shows busy feedback without a false receipt");
    orb_home_request_t request; home_check(orb_home_take_request(&request), "free one scene-request slot");
    orb_home_complete(&request, &request.values, NULL); home_advance(100); home_tap_text("Reading");
    home_check(!home_has_text("Home is busy") && home_has_text("Running"),
        "deliberate scene retry clears busy feedback and awaits authoritative completion");
    unsigned scenes = 0;
    while (orb_home_take_request(&request)) {
        scenes += request.kind == ORB_HOME_REQUEST_SCENE;
        orb_home_complete(&request, &request.values, NULL);
    }
    home_check(scenes == 1, "scene retry never duplicates the rejected attempt");
    free(s); orb_home_load_demo(); authoritative_fixture();
}

static void cover_rim_security(void)
{
    orb_home_snapshot_t *s = climate_qa_fixture(); s->accessory_count = 1;
    orb_home_accessory_t *a = &s->accessories[0]; a->kind = ORB_HOME_COVER;
    /* The hub publishes doors and windows without a writable position. */
    strcpy(a->name, "Garage Door"); a->writable = 0;
    a->state = (orb_home_state_t){.present = ORB_HOME_POSITION, .position = 0};
    climate_qa_publish(s); orb_home_view_open(s_home.view, a->id); home_advance(800);
    home_check(!orb_home_view_dial(s_home.view, NULL, NULL), "read-only security cover never offers a rim owner");
    qa_capture("cover-readonly");
    orb_home_view_rotate(s_home.view, 1); orb_home_view_rotary_finish(s_home.view, true); home_advance(800);
    home_press(400, 346); home_advance(32); home_press(66, 346); home_advance(32); home_release(); home_advance(800);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "cover pointer/rim sequences cannot open a garage door");
    strcpy(a->name, "Bedroom Blinds"); a->writable = ORB_HOME_POSITION;
    climate_qa_publish(s); orb_home_view_open(s_home.view, a->id); home_advance(800);
    home_check(orb_home_view_dial(s_home.view, NULL, NULL), "writable blinds keep their position control");
    free(s); orb_home_load_demo(); authoritative_fixture();
}

static void unknown_field_write_guards(void)
{
    orb_home_snapshot_t *s = climate_qa_fixture();
    s->room_count = 1;
    strcpy(s->rooms[0].id, "qa-room");
    strcpy(s->rooms[0].name, "Reading Room");
    s->accessory_count = 3;

    /* A light whose brightness is writable but whose reported state is unknown. */
    orb_home_accessory_t *light = &s->accessories[0];
    snprintf(light->id, sizeof(light->id), "qa-unk-light");
    snprintf(light->name, sizeof(light->name), "Dimmable Lamp");
    strcpy(light->room, "qa-room");
    light->kind = ORB_HOME_LIGHT;
    light->reachable = true;
    light->writable = ORB_HOME_ON | ORB_HOME_BRIGHTNESS;
    light->state = (orb_home_state_t){.present = ORB_HOME_ON, .on = true};

    /* A switch whose on field is writable but has never been reported. */
    orb_home_accessory_t *sw = &s->accessories[1];
    snprintf(sw->id, sizeof(sw->id), "qa-unk-switch");
    snprintf(sw->name, sizeof(sw->name), "Desk Switch");
    strcpy(sw->room, "qa-room");
    sw->kind = ORB_HOME_SWITCH;
    sw->reachable = true;
    sw->writable = ORB_HOME_ON;
    sw->state = (orb_home_state_t){.present = 0};

    /* A thermostat whose target is writable but whose target is not reported. */
    orb_home_accessory_t *thermo = &s->accessories[2];
    snprintf(thermo->id, sizeof(thermo->id), "qa-unk-thermo");
    snprintf(thermo->name, sizeof(thermo->name), "Thermostat");
    strcpy(thermo->room, "qa-room");
    thermo->kind = ORB_HOME_THERMOSTAT;
    thermo->reachable = true;
    thermo->writable = ORB_HOME_TARGET | ORB_HOME_MODE;
    thermo->state = (orb_home_state_t){.present = ORB_HOME_MODE, .mode = ORB_HOME_MODE_HEAT};

    climate_qa_publish(s);

    /* 1. Detail dial and rim-drag write guards on unknown field. */
    orb_home_view_open(s_home.view, light->id);
    home_advance(800);
    home_check(!orb_home_view_dial(s_home.view, NULL, NULL),
               "accessory with unknown brightness offers no dial");
    home_check(!orb_home_view_dial_writes(s_home.view),
               "accessory with unknown brightness dial does not write");
    qa_capture("unknown-brightness");

    /* Attempt dial twist (rotary): preview nothing, queue nothing. */
    orb_home_view_rotate(s_home.view, .6f);
    orb_home_view_rotary_finish(s_home.view, true);
    home_advance(800);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}),
               "no orb_home request is queued after a dial twist");

    /* Attempt rim drag: preview nothing, queue nothing. */
    home_press(61, 355);
    home_advance(32);
    home_press(405, 355);
    home_advance(32);
    home_release();
    home_advance(800);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}),
               "no orb_home request is queued after a rim drag");

    /* Thermostat target twist also refuses when target is unknown. */
    orb_home_view_open(s_home.view, thermo->id);
    home_advance(800);
    home_check(!orb_home_view_dial(s_home.view, NULL, NULL),
               "thermostat with unknown target offers no dial");
    orb_home_view_rotate(s_home.view, .5f);
    orb_home_view_rotary_finish(s_home.view, true);
    home_advance(800);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}),
               "no orb_home request is queued after a thermostat dial twist");

    /* 2. Room row tap guard on switch with unknown on. */
    orb_home_view_room(s_home.view, "qa-room");
    home_advance(800);
    home_tap_text("Desk Switch");
    home_advance(800);
    home_check(!orb_home_take_request(&(orb_home_request_t){0}),
               "no request is queued after tapping the room row of a switch with unknown on");

    free(s);
    orb_home_load_demo();
    authoritative_fixture();
}

static void off_level_captions(void)
{
    orb_home_load_demo(); authoritative_fixture();
    home_check(orb_home_update("floor-lamp", &(orb_home_state_t){
        .present = ORB_HOME_ON | ORB_HOME_BRIGHTNESS, .on = true, .brightness = 0}, false, true, 0),
        "publish a zero-brightness light with a lagging On flag");
    orb_home_view_open(s_home.view, "floor-lamp"); home_advance(800);
    home_check(home_has_text("Off") && !home_has_text("0%"), "zero brightness reads Off after its slider clears");
    const char *ids[] = {"floor-lamp", "fan"};
    for (unsigned i = 0; i < 2; i++) {
        orb_home_load_demo(); authoritative_fixture();
        orb_home_view_open(s_home.view, ids[i]); home_advance(800);
        home_tap(233, 140); home_advance(100);
        orb_home_request_t request;
        home_check(orb_home_take_request(&request) && !strcmp(request.id, ids[i]), "disc queues its power toggle");
        home_check(home_has_text("Off") && !home_has_text("0%") && home_has_text("Updating"),
            "pending Off keeps its semantic caption after clearing the displayed light/fan slider");
        orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        home_check(snapshot && orb_home_snapshot(snapshot), "copy optimistic Off authority");
        orb_home_state_t accepted = orb_home_find(snapshot, ids[i])->state;
        free(snapshot); orb_home_complete(&request, &accepted, NULL); home_advance(800);
        home_check(home_has_text("Off") && !home_has_text("0%"), "accepted Off retains its semantic caption");
    }
    orb_home_load_demo(); authoritative_fixture();
}

static void missing_accessory_detail(void)
{
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    orb_home_view_open(s_home.view, "missing-light"); home_advance(800);
    home_check(home_has_text("Accessory not found") && !orb_home_view_dial(s_home.view, NULL, NULL),
        "a missing accessory shows explicit unavailable detail without invented controls");
    home_check(orb_home_view_back(s_home.view), "missing accessory retains a Back route");
    home_advance(800);
    home_check(rooms_destination(), "missing detail Back returns to the prior Rooms context");
}

int main(int argc, char **argv)
{
    home_init(argc, argv, 1);
    authoritative_fixture();
    const char *qa = getenv("ORB_HOME_QA_CASE");
    if (qa) {
        if (!strcmp(qa, "rooms")) { rooms_list_contract(); room_temperature_subtitle(); room_icon_mapping(); full_room_names(); }
        else if (!strcmp(qa, "names")) full_room_names();
        else if (!strcmp(qa, "polish")) polish_contract();
        else if (!strcmp(qa, "entry")) rooms_entry();
        else if (!strcmp(qa, "target")) authoritative_target_extremes();
        else if (!strcmp(qa, "cover")) cover_rim_security();
        else if (!strcmp(qa, "hold")) unlock_hold_lifecycle();
        else if (!strcmp(qa, "scene")) live_scene_identity();
        else if (!strcmp(qa, "cancel")) cancelled_list_contact();
        else if (!strcmp(qa, "busy")) rejected_pointer_preview();
        else if (!strcmp(qa, "scene-busy")) rejected_scene_feedback();
        else if (!strcmp(qa, "unknown")) unknown_field_write_guards();
        else climate_source_and_staleness();
        home_shutdown(); return 0;
    }
    climate_source_and_staleness();
    authoritative_target_extremes();
    cover_rim_security();
    unlock_hold_lifecycle();
    live_scene_identity();
    cancelled_list_contact();
    rejected_pointer_preview();
    rejected_scene_feedback();
    rooms_list_contract();
    room_temperature_subtitle();
    room_icon_mapping();
    missing_accessory_detail();
    full_room_names();
    kit_action_hierarchy();
    long_scene_and_detail_context();
    polish_contract();
    off_level_captions();
    unrelated_home_update_keeps_room();
    busy_room_continuation();
    embedded_busy_room_navigation();
    room_all_off_contract();
    thermostat_modes_and_sensor_age();
    thermostat_room_live_range();
    settled_screens();
    saved_navigation_context();
    saved_list_anchors();
    summary_consistency();
    rooms_entry();
    navigation_contract();
    climate_reading_order();
    retained_position_contract();
    positioned_parent_contract();
    landing_horizontal_contacts();
    scene_identity();
    toggle_and_rollback();
    unknown_field_write_guards();
    content_contact_guards();
    white_temperature_roundtrip();
    secure_lock();
    rim_damage();
    system_navigation_handoff();
    interrupted_fill();
    unknown_lock_and_wire_sensors();
    connection_gates();
    fahrenheit_controls();
    scene_feedback_and_provenance();
    accessory_damage();
    landing_model_updates();
    long_row_fit_contract();
    printf("Home view: %u checks passed\n", s_home.checks);
    home_shutdown();
    return 0;
}
