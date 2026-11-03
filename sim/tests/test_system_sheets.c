/* Design 2.0 system sheets: real service strings and every accessibility size.
 * Fixtures are private and use the production LVGL/shell rendering path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
#define CHECK(v) do { ++s_checks; if (!(v)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); exit(1); } } while (0)
static uint32_t tick(void) { return s_now; }
static void settle(void)
{
    for (unsigned i = 0; i < 48; ++i) {
        s_now += 16; orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}
static lv_obj_t *label(lv_obj_t *root, const char *value)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), value)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), value);
        if (found) return found;
    }
    return NULL;
}
static bool effective_bounds(lv_obj_t *obj, lv_area_t *bounds)
{
    lv_obj_get_coords(obj, bounds);
    for (lv_obj_t *parent = lv_obj_get_parent(obj); parent; parent = lv_obj_get_parent(parent)) {
        lv_area_t clip; lv_obj_get_coords(parent, &clip);
        bounds->x1 = LV_MAX(bounds->x1, clip.x1); bounds->x2 = LV_MIN(bounds->x2, clip.x2);
        bounds->y1 = LV_MAX(bounds->y1, clip.y1); bounds->y2 = LV_MIN(bounds->y2, clip.y2);
    }
    return bounds->x1 <= bounds->x2 && bounds->y1 <= bounds->y2;
}
static void text_areas(lv_obj_t *obj, lv_area_t *areas, unsigned *count)
{
    if (!lv_obj_is_visible(obj)) return;
    if (lv_obj_check_type(obj, &lv_label_class) && lv_label_get_text(obj)[0]) {
        lv_area_t area;
        if (effective_bounds(obj, &area)) {
            CHECK(*count < 64); areas[(*count)++] = area;
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i)
        text_areas(lv_obj_get_child(obj, i), areas, count);
}
static unsigned primary_count(lv_obj_t *obj)
{
    if (!lv_obj_is_visible(obj)) return 0;
    unsigned count = lv_obj_get_height(obj) == ORB_KIT_PRIMARY_H &&
                     lv_obj_get_child_count(obj) > 0 && orb_glass_get_amount(obj) > 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i)
        count += primary_count(lv_obj_get_child(obj, i));
    return count;
}
static void check_and_shot(const char *directory, const char *scene, lv_obj_t *root)
{
    settle();
    lv_area_t areas[64]; unsigned count = 0;
    text_areas(root, areas, &count);
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned j = i + 1; j < count; ++j) {
            const lv_area_t *a = &areas[i], *b = &areas[j];
            CHECK(a->x2 < b->x1 || b->x2 < a->x1 || a->y2 < b->y1 || b->y2 < a->y1);
        }
    }
    CHECK(primary_count(root) <= 1);
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", directory, scene);
    CHECK(sim_display_save_png(path));
    lv_mem_monitor_t memory; lv_mem_monitor(&memory);
    printf("system sheets %s: retained LVGL payload=%zu bytes\n", scene, memory.total_size);
}
int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init());
    if (strcmp(argv[2], "default")) CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    if (!strcmp(argv[2], "ax3-bold-rt")) {
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t state = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .brightness = 70, .ota = ORB_SHELL_OTA_DOWNLOADING, .progress = 42};
    strcpy(state.firmware_version, "0.5.2"); strcpy(state.version, "0.5.3");
    strcpy(state.clock_time, "10:09"); strcpy(state.clock_date, "Wednesday 28");
    orb_shell_update(s_shell, &state);
    check_and_shot(argv[1], "Updates-downloading", orb_shell_root(s_shell));
    CHECK(label(orb_shell_root(s_shell), "42%"));
    CHECK(primary_count(orb_shell_root(s_shell)) == 0);
    state.ota = ORB_SHELL_OTA_VERIFYING; orb_shell_update(s_shell, &state);
    check_and_shot(argv[1], "Updates-verifying", orb_shell_root(s_shell));
    state.ota = ORB_SHELL_OTA_READY; orb_shell_update(s_shell, &state);
    check_and_shot(argv[1], "Updates-ready", orb_shell_root(s_shell));
    state.ota = ORB_SHELL_OTA_FAILED;
    strcpy(state.error, "Mac disconnected while verifying. Join Wi-Fi and check again.");
    orb_shell_update(s_shell, &state);
    check_and_shot(argv[1], "Updates-error-long", orb_shell_root(s_shell));
    lv_obj_t *action = label(orb_shell_root(s_shell), "Try again"); CHECK(action);
    CHECK(lv_obj_get_height(lv_obj_get_parent(action)) == ORB_KIT_PRIMARY_H);
    orb_shell_twist(s_shell, 8);
    check_and_shot(argv[1], "Updates-error-end", orb_shell_root(s_shell));
    state.ota = ORB_SHELL_OTA_IDLE; orb_shell_update(s_shell, &state);
    orb_shell_button(s_shell, &(orb_button_event_t){.button = ORB_BUTTON_PWR,
        .gesture = ORB_BUTTON_HOLD, .synthetic = true});
    check_and_shot(argv[1], "PowerSheet", lv_display_get_layer_top(s_display));
    const char *controls[] = {"Sleep display", "Restart"};
    for (unsigned i = 0; i < 2; ++i) {
        action = label(lv_display_get_layer_top(s_display), controls[i]); CHECK(action);
        CHECK(lv_obj_get_height(lv_obj_get_parent(action)) == ORB_KIT_SECONDARY_H);
    }
    CHECK(primary_count(lv_display_get_layer_top(s_display)) == 0);
    orb_shell_destroy(s_shell);
    printf("system sheets: %u assertions passed (%s)\n", s_checks, argv[2]);
    return 0;
}
