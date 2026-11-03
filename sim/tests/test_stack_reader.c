/* A reader retains a physical contact, including pulls at either limit.
 * Exercise the real shell and LVGL input timer with local synthetic cards. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static const char *s_case = "setup";
static orb_shell_t *s_shell;
static lv_indev_data_t s_input;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "Stack reader %s at %u ms, line %d: %s\n", s_case, s_now, __LINE__, #value); \
    exit(1); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_input; }
static void advance(unsigned milliseconds)
{
    uint32_t end = s_now + milliseconds;
    while (s_now < end) {
        s_now += LV_MIN(4, end - s_now);
        orb_shell_poll(s_shell); lv_timer_handler();
    }
}
static void point(int x, int y, bool down, unsigned milliseconds)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    advance(milliseconds);
}
static void drag(int x, int y, int dx, int dy)
{
    point(x, y, true, 32);
    for (unsigned step = 1; step <= 24; step++)
        point(x + dx * (int)step / 24, y + dy * (int)step / 24, true, 16);
    point(x + dx, y + dy, true, 96);
    point(x + dx, y + dy, false, 64); advance(1200);
}
static lv_obj_t *text(lv_obj_t *root, const char *value)
{
    if (lv_obj_is_hidden(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), value)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = text(lv_obj_get_child(root, i), value);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *action(lv_obj_t *root, unsigned id)
{
    if (lv_obj_is_clickable(root) && (uintptr_t)lv_obj_get_user_data(root) == id) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = action(lv_obj_get_child(root, i), id);
        if (found) return found;
    }
    return NULL;
}
static void retained(lv_obj_t *reader)
{
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    lv_obj_t *title = text(orb_shell_root(s_shell), "Read this card");
    CHECK(title && lv_obj_get_parent(title) == reader);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); s_case = argv[1];
    bool short_body = !strcmp(s_case, "static"), preview_mode = !strcmp(s_case, "preview");
    lv_init(); lv_tick_set_cb(tick); CHECK(sim_display_create()); orb_ui_init(NULL);
    orb_time_init(); orb_apps_init(ORB_APP_NEEDS_IMU); CHECK(orb_model_init() && orb_home_init());
    orb_model_set_connection(true, true);
    orb_card_t first = {.kind = ORB_CARD_MESSAGE, .priority = ORB_PRIORITY_HIGH};
    strcpy(first.id, "read"); strcpy(first.title, "Read this card");
    strcpy(first.body, short_body ? "Short body" :
        "First paragraph stays with this card while the reader scrolls.\n"
        "Second paragraph gives the reader more than a full screen of text.\n"
        "Third paragraph still belongs to the same card.\n"
        "Last paragraph is the bottom of this long update.");
    orb_card_t second = {.kind = ORB_CARD_MESSAGE, .priority = ORB_PRIORITY_NORMAL};
    strcpy(second.id, "next"); strcpy(second.title, "The next card"); strcpy(second.body, "Next body");
    if (!strcmp(s_case, "top")) { first.priority = ORB_PRIORITY_NORMAL; second.priority = ORB_PRIORITY_HIGH; }
    if (preview_mode) {
        CHECK(orb_time_timer_set(60) && orb_time_timer_start(0));
    } else CHECK(orb_model_upsert_card(&first) && orb_model_upsert_card(&second));
    lv_indev_t *input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_pointer);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .brightness = 70, .usb = true};
    strcpy(status.clock_time, "10:09"); strcpy(status.clock_date, "Tuesday, October 29");
    orb_shell_update(s_shell, &status);
    orb_shell_navigate(s_shell, ORB_SHELL_STACK); advance(1500);
    if (preview_mode) {
        CHECK(text(orb_shell_root(s_shell), "Timer running"));
        lv_obj_t *preview = action(orb_shell_root(s_shell), 103); CHECK(preview);
        lv_area_t bounds; lv_obj_get_coords(preview, &bounds);
        point((bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2, true, 32);
        point(s_input.point.x, s_input.point.y, false, 64); advance(1200);
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
        CHECK(!text(orb_shell_root(s_shell), "Timer running"));
        goto done;
    }
    if (!strcmp(s_case, "top")) {
        CHECK(text(orb_shell_root(s_shell), second.title));
        drag(233, 290, 0, -110);
    }
    lv_obj_t *title = text(orb_shell_root(s_shell), first.title); CHECK(title);
    lv_obj_t *reader = lv_obj_get_parent(title); lv_obj_update_layout(reader);
    int maximum = lv_obj_get_scroll_bottom(reader) + lv_obj_get_scroll_top(reader);
    CHECK(short_body ? maximum <= 0 : maximum > 150);
    if (!strcmp(s_case, "top")) {
        CHECK(lv_obj_get_scroll_y(reader) == 0);
        drag(233, 200, 0, 110); retained(reader);
        CHECK(lv_obj_get_scroll_y(reader) == 0);
    } else if (!strcmp(s_case, "bottom")) {
        lv_obj_scroll_to_y(reader, maximum, LV_ANIM_OFF); advance(96);
        drag(233, 290, 0, -110); retained(reader);
        CHECK(lv_obj_get_scroll_bottom(reader) == 0);
    } else if (!strcmp(s_case, "scroll")) {
        drag(233, 290, 0, -110); retained(reader);
        CHECK(lv_obj_get_scroll_y(reader) > 0);
        /* Leaving the viewport never promotes a retained reader contact. */
        drag(233, 290, 0, -180); retained(reader);
    } else if (!strcmp(s_case, "rim")) {
        point(449, 233, true, 32);
        for (unsigned step = 1; step <= 30; step++) {
            float angle = (float)step * 1.4f * (float)M_PI / 180;
            point((int)lroundf(233 + 216 * cosf(angle)), (int)lroundf(233 + 216 * sinf(angle)), true, 16);
        }
        point(s_input.point.x, s_input.point.y, false, 64); advance(1200);
        CHECK(text(orb_shell_root(s_shell), second.title));
    } else {
        CHECK(short_body); drag(233, 290, 0, -110);
        CHECK(text(orb_shell_root(s_shell), second.title));
    }
done:
    orb_shell_destroy(s_shell); lv_indev_delete(input);
    printf("Stack reader %s: %u checks passed\n", s_case, s_checks);
    return 0;
}
