/* Battery validity, first-run instructions and real round geometry. No board
 * services or external input leave this isolated simulator process. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_sound.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "src/misc/lv_text_private.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks, s_desk_actions;
static unsigned s_regression_failures;
static unsigned s_idle_flushes;
static bool s_measuring_idle;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static const char *s_scene = "initialization";

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "System %s at %u ms: %s:%d: %s\n", s_scene, s_now, __FILE__, __LINE__, #value); \
    exit(1); \
} } while (0)
#define REGRESSION(value) do { ++s_checks; if (!(value)) { \
    ++s_regression_failures; fprintf(stderr, "System QA regression at %u ms: %s:%d: %s\n", \
        s_now, __FILE__, __LINE__, #value); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void display_event(lv_event_t *event)
{
    if (s_measuring_idle && lv_event_get_code(event) == LV_EVENT_FLUSH_START) ++s_idle_flushes;
}
static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    *data = s_input;
}
static void action(orb_shell_action_t action, void *ctx)
{
    (void)ctx;
    if (action == ORB_SHELL_ACTION_DESK_LIMIT) ++s_desk_actions;
}
static void advance(unsigned frames)
{
    for (unsigned i = 0; i < frames; ++i) {
        s_now += 16;
        orb_shell_poll(s_shell);
        lv_timer_handler();
        lv_refr_now(s_display);
    }
}
static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    const char *rim_text = orb_kit_rim_text_get(root);
    if (rim_text && !strcasecmp(rim_text, text)) return root;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
static void text_ink_bounds(lv_obj_t *label, lv_area_t *area)
{
    lv_obj_get_coords(label, area);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    const char *text = lv_label_get_text(label);
    int32_t top = font->line_height, bottom = 0;
    uint32_t offset = 0;
    while (text[offset]) {
        uint32_t cp = lv_text_encoded_next(text, &offset);
        uint32_t peek = offset, next = text[peek] ? lv_text_encoded_next(text, &peek) : 0;
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, next) || !glyph.box_h) continue;
        int32_t y = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        top = LV_MIN(top, y); bottom = LV_MAX(bottom, y + glyph.box_h - 1);
    }
    area->y2 = area->y1 + bottom; area->y1 += top;
}
/* Check actual prepared row geometry, including the narrow trailing switch
 * lane under AX3. Measuring each label alone misses title/subtitle collisions. */
static void row_overlap(lv_obj_t *row)
{
    if (lv_obj_get_height(row) < ORB_KIT_ROW_H || lv_obj_get_width(row) != 418) return;
    for (unsigned i = 0; i < lv_obj_get_child_count(row); i++) {
        lv_obj_t *a = lv_obj_get_child(row, i);
        if (!lv_obj_check_type(a, &lv_label_class)) continue;
        lv_area_t aa; text_ink_bounds(a, &aa);
        for (unsigned j = i + 1; j < lv_obj_get_child_count(row); j++) {
            lv_obj_t *b = lv_obj_get_child(row, j);
            if (!lv_obj_check_type(b, &lv_label_class)) continue;
            lv_area_t bb; text_ink_bounds(b, &bb);
            if (aa.x1 <= bb.x2 && bb.x1 <= aa.x2 && aa.y1 <= bb.y2 && bb.y1 <= aa.y2)
                fprintf(stderr, "Row overlap: %s / %s\n", lv_label_get_text(a), lv_label_get_text(b));
            CHECK(aa.x1 > bb.x2 || bb.x1 > aa.x2 || aa.y1 > bb.y2 || bb.y1 > aa.y2);
        }
    }
}
static unsigned fitted(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return 0;
    row_overlap(object);
    unsigned checked = 0;
    if (lv_obj_check_type(object, &lv_label_class) && *lv_label_get_text(object)) {
        lv_area_t area; lv_obj_get_coords(object, &area);
        for (lv_obj_t *parent = lv_obj_get_parent(object); parent; parent = lv_obj_get_parent(parent)) {
            lv_area_t shown;
            orb_round_list_cache_stats_t cache;
            if (orb_round_list_cache_stats(lv_obj_get_parent(parent), &cache)) {
                /* Cached sources stay semantically present outside their
                 * visible chord; inspect only the mapped, clipped label. */
                if (!orb_round_list_child_bounds(parent, object, &shown) ||
                    !lv_obj_area_is_visible(parent, &shown)) return 0;
                area = shown;
                break;
            }
            if (orb_round_list_child_bounds(parent, object, &shown)) { area = shown; break; }
            if (lv_obj_get_style_opa(parent, 0) == 0) return 0;
            if (!lv_obj_is_overflow_visible(parent)) {
                lv_area_t clip; lv_obj_get_coords(parent, &clip);
                area.x1 = LV_MAX(area.x1, clip.x1); area.x2 = LV_MIN(area.x2, clip.x2);
                area.y1 = LV_MAX(area.y1, clip.y1); area.y2 = LV_MIN(area.y2, clip.y2);
                if (area.x1 > area.x2 || area.y1 > area.y2) return 0;
            }
        }
        if (area.y1 >= 0 && area.y2 < 466) {
            ++checked;
            int chord = orb_chord_width(area.y1, area.y2, ORB_RIM_MARGIN_TEXT), left = (466 - chord) / 2;
            if (area.x1 < left || area.x2 >= left + chord)
                fprintf(stderr, "chord: %s (%d,%d)-(%d,%d) width%d\n", lv_label_get_text(object), area.x1,area.y1,area.x2,area.y2,chord);
            CHECK(area.x1 >= left && area.x2 < left + chord);
            lv_point_t size; lv_text_get_size(&size, lv_label_get_text(object), lv_obj_get_style_text_font(object,0),
                lv_obj_get_style_text_letter_space(object,0), lv_obj_get_style_text_line_space(object,0),
                lv_obj_get_width(object), LV_TEXT_FLAG_NONE);
            if (lv_label_get_long_mode(object) != LV_LABEL_LONG_MODE_DOTS &&
                (size.x > lv_obj_get_width(object) || size.y > lv_obj_get_height(object)))
                fprintf(stderr,"fit: %s text%d,%d box%d,%d\n",lv_label_get_text(object),size.x,size.y,(int)lv_obj_get_width(object),(int)lv_obj_get_height(object));
            CHECK(lv_label_get_long_mode(object) == LV_LABEL_LONG_MODE_DOTS ||
                  (size.x <= lv_obj_get_width(object) && size.y <= lv_obj_get_height(object)));
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) checked += fitted(lv_obj_get_child(object,i));
    return checked;
}
static void save(const char *directory, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    CHECK(sim_display_save_png(path));
}
static orb_shell_snapshot_t snapshot(void)
{
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .usb = true, .charging = true,
        .desk_charge_limit = true,
        .battery_detail = {.present = true, .usb = true, .charging = true, .battery_pct = 80,
            .health_valid = true, .health_pct = 96, .temperature_valid = true, .temperature_c = 25}};
    strcpy(status.clock_time, "10:09");
    return status;
}
static void fresh(orb_shell_surface_t surface, const orb_shell_snapshot_t *status)
{
    if (s_shell) orb_shell_destroy(s_shell);
    s_shell = orb_shell_create(lv_screen_active(), action, NULL); CHECK(s_shell);
    orb_shell_update(s_shell, status);
    orb_shell_navigate(s_shell, surface);
    advance(80);
    CHECK(orb_shell_surface(s_shell) == surface);
}
static void retained_size(const char *heading)
{
    lv_obj_t *title = find(orb_shell_root(s_shell), heading); CHECK(title);
    lv_obj_t *page = lv_obj_get_parent(title);
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    lv_obj_clean(page);
    lv_mem_monitor(&after);
    CHECK(before.total_size > after.total_size);
    printf("%s: retained LVGL view tree %zu bytes; C view state 0 bytes (desktop payload, no owner budgets)\n",
           s_scene, before.total_size - after.total_size);
    orb_shell_destroy(s_shell); s_shell = NULL;
}
static void tap(int x, int y, bool synthetic)
{
    if (synthetic) orb_input_mark_synthetic(s_pointer);
    else lv_indev_set_user_data(s_pointer, NULL);
    s_input = (lv_indev_data_t){.point = {x, y}, .state = LV_INDEV_STATE_PRESSED};
    advance(3);
    s_input.state = LV_INDEV_STATE_RELEASED;
    advance(3);
}
static void drag_back(int x, int y)
{
    lv_indev_set_user_data(s_pointer, NULL);
    s_input = (lv_indev_data_t){.point = {x, y}, .state = LV_INDEV_STATE_PRESSED};
    advance(3);
    for (unsigned i = 1; i <= 6; i++) {
        s_input.point.y = y - (int)i * 15;
        advance(2);
    }
    for (unsigned i = 1; i <= 6; i++) {
        s_input.point.y = y - 90 + (int)i * 15;
        advance(2);
    }
    s_input.state = LV_INDEV_STATE_RELEASED;
    advance(30);
}
static void still_at_rest(void)
{
    advance(100);
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    s_idle_flushes = 0; s_measuring_idle = true;
    advance(320);
    s_measuring_idle = false;
    lv_mem_monitor(&after);
    CHECK(s_idle_flushes == 0);
    CHECK(before.total_size == after.total_size);
    printf("%s: no redraws or retained-payload growth over 5.12 s idle\n", s_scene);
}
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    const char *directory = argc > 2 ? argv[2] : "/tmp";
    bool ax = !strncmp(mode, "ax3", 3);
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    lv_display_add_event_cb(s_display, display_event, LV_EVENT_FLUSH_START, NULL);
    orb_ui_init(NULL); orb_glass_cache_prewarm(); CHECK(orb_model_init());
    orb_model_reset(); orb_model_set_connection(true, true);
    if (ax) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        if (!strcmp(mode, "ax3-bold-rt")) {
            CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
            orb_ui_set_reduce_transparency(true);
        }
    }
    s_pointer = lv_indev_create(); CHECK(s_pointer);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display); lv_indev_set_read_cb(s_pointer, pointer_read);
    s_input.state = LV_INDEV_STATE_RELEASED;
    orb_shell_snapshot_t status = snapshot();

    s_scene = "Battery";
    fresh(ORB_SHELL_BATTERY, &status);
    CHECK(find(orb_shell_root(s_shell), "80%") && find(orb_shell_root(s_shell), "96%"));
    CHECK(find(orb_shell_root(s_shell), "25°C"));
    CHECK(!find(orb_shell_root(s_shell), "Good · 96%") && !find(orb_shell_root(s_shell), "About 9 h left at your usual use"));
    save(directory, "Battery");
    CHECK(fitted(orb_shell_root(s_shell)) >= 5);
    still_at_rest();
    tap(233, 331, true);
    CHECK(s_desk_actions == 0);
    lv_obj_t *desk_label = find(orb_shell_root(s_shell), "Desk limit"); CHECK(desk_label);
    lv_obj_t *desk_switch = orb_kit_row_switch(lv_obj_get_parent(desk_label)); CHECK(desk_switch);
    /* A rejected contact must not leave a switch looking successfully saved. */
    REGRESSION(orb_kit_switch_get_checked(desk_switch) == status.desk_charge_limit);
    advance(100);
    tap(233, 331, false);
    CHECK(s_desk_actions == 1);
    drag_back(233, 331);
    CHECK(s_desk_actions == 1);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_BATTERY);
    unsigned volume = orb_sound_volume();
    CHECK(orb_shell_twist(s_shell, 1)); advance(30);
    CHECK(orb_sound_volume() == volume && s_desk_actions == 1);
    if (ax) {
        lv_obj_t *name = find(orb_shell_root(s_shell), "Temperature"); CHECK(name);
        lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(name));
        lv_obj_scroll_to_y(list, 160, LV_ANIM_OFF); advance(20);
        CHECK(fitted(orb_shell_root(s_shell)) >= 3);
        save(directory, "Battery-scrolled");
    }
    lv_obj_t *battery_list = lv_obj_get_parent(lv_obj_get_parent(find(orb_shell_root(s_shell), "Temperature")));
    lv_obj_scroll_to_y(battery_list, 0, LV_ANIM_OFF); advance(160);
    status.battery_setting_error = true;
    orb_shell_update(s_shell, &status); advance(20);
    CHECK(find(orb_shell_root(s_shell), ORB_ICON_WARNING " Not saved"));
    CHECK(fitted(orb_shell_root(s_shell)) >= 3);
    save(directory, "Battery-error");
    status.battery_setting_error = false;
    retained_size("Battery");

    s_scene = "Battery-units";
    fresh(ORB_SHELL_BATTERY, &status);
    orb_prefs_t preferences; orb_model_prefs_defaults(&preferences);
    preferences.temperature = ORB_TEMPERATURE_F;
    CHECK(orb_model_set_prefs(&preferences)); advance(80);
    CHECK(find(orb_shell_root(s_shell), "77°F"));
    status.battery_detail.temperature_c = -10;
    orb_shell_update(s_shell, &status); advance(20);
    CHECK(find(orb_shell_root(s_shell), "14°F"));
    status.battery_detail.temperature_c = NAN;
    orb_shell_update(s_shell, &status); advance(20);
    CHECK(!find(orb_shell_root(s_shell), "14°F") && find(orb_shell_root(s_shell), "Unavailable"));
    preferences.temperature = ORB_TEMPERATURE_C;
    CHECK(orb_model_set_prefs(&preferences));
    status.battery_detail.temperature_c = 25;

    s_scene = "Battery-unknown";
    status.battery_detail.health_valid = false;
    status.battery_detail.temperature_valid = false;
    status.battery_detail.current_valid = false;
    status.battery_detail.health_pct = 96; /* Invalid stale numbers stay hidden. */
    status.battery_detail.temperature_c = 25;
    fresh(ORB_SHELL_BATTERY, &status);
    CHECK(!find(orb_shell_root(s_shell), "96%") && !find(orb_shell_root(s_shell), "25°C"));
    CHECK(find(orb_shell_root(s_shell), "Unavailable"));
    CHECK(fitted(orb_shell_root(s_shell)) >= 5);
    save(directory, "Battery-unknown");
    status.battery_detail.present = false; status.battery_detail.battery_pct = -1;
    orb_shell_update(s_shell, &status); advance(80);
    CHECK(!find(orb_shell_root(s_shell), "80%"));
    CHECK(find(orb_shell_root(s_shell), "—"));

    /* A battery removed between telemetry samples invalidates all of its
     * values, even when an earlier health/temperature sample was valid. */
    status.battery_detail.health_valid = status.battery_detail.temperature_valid = true;
    status.battery_detail.desk_limit_active = true;
    orb_shell_update(s_shell, &status); advance(20);
    REGRESSION(!find(orb_shell_root(s_shell), "96%"));
    REGRESSION(!find(orb_shell_root(s_shell), "25°C"));
    REGRESSION(!find(orb_shell_root(s_shell), "Desk limit reached"));
    save(directory, "Battery-removed");
    fresh(ORB_SHELL_BATTERY, &status);
    REGRESSION(!find(orb_shell_root(s_shell), "96%"));
    REGRESSION(!find(orb_shell_root(s_shell), "25°C"));
    REGRESSION(!find(orb_shell_root(s_shell), "Desk limit reached"));

    status = snapshot();
    status.net = ORB_SHELL_NET_FAILED; status.link = ORB_SHELL_LINK_OFFLINE;
    status.usb = status.charging = false;
    status.battery_pct = status.battery_detail.battery_pct = 5;
    status.battery_detail.usb = status.battery_detail.charging = false;
    fresh(ORB_SHELL_BATTERY, &status);
    CHECK(find(orb_shell_root(s_shell), "5%"));
    CHECK(find(orb_shell_root(s_shell), "On battery"));
    CHECK(find(orb_shell_root(s_shell), "25°C"));
    save(directory, "Battery-offline-low");

    s_scene = "Onboarding";
    fresh(ORB_SHELL_ONBOARDING, &status);
    save(directory, "Onboarding-low");
    status = snapshot();
    fresh(ORB_SHELL_ONBOARDING, &status);
    CHECK(find(orb_shell_root(s_shell), "Welcome to Orb") && find(orb_shell_root(s_shell), "Got it"));
    CHECK(find(orb_shell_root(s_shell), "Notifications") && find(orb_shell_root(s_shell), "Smart Stack"));
    CHECK(fitted(orb_shell_root(s_shell)) >= 5);
    save(directory, "Onboarding");
    still_at_rest();
    drag_back(233, 250);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_ONBOARDING);
    CHECK(orb_shell_twist(s_shell, 1)); advance(30);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_ONBOARDING);
    CHECK(s_desk_actions == 1 && orb_model_pending_answers() == 0);
    retained_size("Welcome to Orb");
    fresh(ORB_SHELL_ONBOARDING, &status);
    tap(233, 380, false); advance(80);
    /* The ordinary host fixture has no durable store: physical acceptance
     * stays visible as a retryable failure instead of manufacturing success. */
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_ONBOARDING);
    CHECK(find(orb_shell_root(s_shell), "Not saved") && find(orb_shell_root(s_shell), "Try again"));
    tap(233, 380, true); advance(80);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    orb_shell_destroy(s_shell);
    printf("system views %s: %u checks passed\n", mode, s_checks);
    return s_regression_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
