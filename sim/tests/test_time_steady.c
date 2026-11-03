/* Contacts and transport keep their meaning across the real Time view. */
#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_time.h"
#include "orb_time_view.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_now = 1000;
static unsigned s_checks, s_failures;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_time_view_t *s_view;
static const time_t s_wall = 1793182140;

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_input; }
static void expect(bool value, const char *why)
{
    ++s_checks;
    if (value) return;
    ++s_failures;
    fprintf(stderr, "Time steady: %s\n", why);
}
static void advance(unsigned ms)
{
    uint32_t end = s_now + ms;
    while (s_now < end) {
        s_now += LV_MIN(8, end - s_now);
        orb_time_poll((int64_t)s_now * 1000, s_wall);
        orb_time_view_poll(s_view);
        lv_timer_handler();
    }
    lv_obj_update_layout(lv_screen_active());
}
static void point(int x, int y, bool down)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    advance(32);
}
static void lift(void) { point(s_input.point.x, s_input.point.y, false); advance(700); }
static void view(orb_time_page_t page)
{
    if (s_view) orb_time_view_destroy(s_view);
    s_view = NULL;
    orb_time_init(); orb_time_poll((int64_t)s_now * 1000, s_wall);
    if (page == ORB_TIME_PAGE_STOPWATCH) {
        orb_time_stopwatch_start((int64_t)s_now * 1000);
        for (unsigned i = 0; i < 12; ++i) {
            s_now += 1000;
            orb_time_stopwatch_lap((int64_t)s_now * 1000);
        }
        orb_time_stopwatch_pause((int64_t)s_now * 1000);
    } else if (page == ORB_TIME_PAGE_REMINDER) {
        for (unsigned i = 0; i < 8; ++i) {
            char title[32]; snprintf(title, sizeof(title), "Reminder %u", i + 1);
            expect(orb_time_reminder_set(title, s_wall + 3600 + i * 60, NULL), "create bounded reminder fixture");
        }
    }
    s_view = orb_time_view_create(lv_screen_active(), NULL);
    expect(s_view != NULL, "create real production Time view");
    orb_time_view_select(s_view, page); advance(700);
}
static lv_obj_t *named(lv_obj_t *obj, const char *name)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), name)) {
        lv_obj_t *control = lv_obj_get_parent(obj);
        while (control && !lv_obj_is_clickable(control)) control = lv_obj_get_parent(control);
        if (control && lv_obj_is_visible(control)) return control;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *found = named(lv_obj_get_child(obj, (int32_t)i), name);
        if (found) return found;
    }
    return NULL;
}
static lv_point_t centre(const char *name)
{
    lv_obj_t *obj = named(orb_time_view_root(s_view), name);
    expect(obj != NULL, name);
    lv_area_t box = {0};
    if (obj) lv_obj_get_coords(obj, &box);
    return (lv_point_t){(box.x1 + box.x2 + 1) / 2, (box.y1 + box.y2 + 1) / 2};
}
static void tap_at(lv_point_t p) { point(p.x, p.y, true); lift(); }
static void transport_clearance(const char *primary, const char *secondary)
{
    lv_obj_t *p = named(orb_time_view_root(s_view), primary);
    lv_obj_t *s = named(orb_time_view_root(s_view), secondary);
    lv_obj_t *laps = named(orb_time_view_root(s_view), "Laps");
    expect(p && s && laps, "transport and Laps targets retain semantic names");
    if (!p || !s || !laps) return;
    lv_area_t a, b, c;
    lv_obj_get_coords(p, &a); lv_obj_get_coords(s, &b); lv_obj_get_coords(laps, &c);
    expect(LV_MIN(a.y1, b.y1) - c.y2 - 1 >= 20, "Laps capsule keeps twenty pixels above both transport discs");
    expect(LV_MAX(a.x1, b.x1) - LV_MIN(a.x2, b.x2) - 1 >= 20,
        "transport discs retain twenty pixels between targets");
}
static void capture(const char *directory, const char *name)
{
    char path[512]; snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    lv_refr_now(s_display);
    expect(sim_display_save_png(path), "save transport evidence");
}
static void paging(void)
{
    for (unsigned page = ORB_TIME_PAGE_STOPWATCH; page <= ORB_TIME_PAGE_REMINDER; page += 2) {
        for (unsigned limit = 0; limit < 3; ++limit) {
            view((orb_time_page_t)page);
            lv_obj_t *list = orb_time_view_scroll(s_view);
            expect(list != NULL, "list fixture has a scroll owner");
            int maximum = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
            int offset = limit == 0 ? 0 : limit == 1 ? maximum / 2 : maximum;
            lv_obj_scroll_to_y(list, offset, LV_ANIM_OFF); advance(300);
            int y = limit == 0 ? 210 : 300;
            int dy = limit == 0 ? 36 : -36;
            point(233, y, true);
            point(233, y + dy, true);
            if (limit == 1) expect(lv_obj_get_scroll_y(list) > offset,
                "vertical list contact scrolls before sideways drift");
            /* After the vertical owner is established, drift dominates the
             * final displacement. It must not reclassify this contact. */
            point(290, y + dy, true);
            point(365, y + dy, true);
            point(390, y + dy, true); lift();
            expect(orb_time_view_page(s_view) == (orb_time_page_t)page,
                page == ORB_TIME_PAGE_STOPWATCH ? "vertical laps contact cannot become paging, including limits" :
                "vertical reminders contact cannot become paging, including limits");
        }
    }
    view(ORB_TIME_PAGE_STOPWATCH);
    point(330, 170, true); point(280, 170, true); point(230, 170, true); point(160, 170, true); lift();
    expect(orb_time_view_page(s_view) == ORB_TIME_PAGE_ALARM, "deliberate horizontal contact still pages");
    view(ORB_TIME_PAGE_REMINDER);
    point(160, 220, true); point(210, 220, true); point(260, 220, true); point(330, 220, true); lift();
    expect(orb_time_view_page(s_view) == ORB_TIME_PAGE_ALARM, "horizontal reminder-row contact still pages");
}
static void transport(const char *directory)
{
    view(ORB_TIME_PAGE_TIMER);
    orb_time_view_select(s_view, ORB_TIME_PAGE_STOPWATCH); advance(700);
    lv_point_t start = centre("Start"); capture(directory, "stopwatch-idle");
    tap_at(start);
    lv_point_t pause = centre("Pause"), lap = centre("Lap");
    expect(start.x == pause.x && start.y == pause.y, "primary Start/Pause never moves");
    expect(pause.x == 233, "stopwatch primary stays on the centre axis");
    expect(abs(pause.x - lap.x) >= 100, "Lap has at least twenty pixels clear of the primary disc");
    advance(1200); tap_at(lap);
    transport_clearance("Pause", "Lap");
    capture(directory, "stopwatch-running");
    tap_at(pause);
    lv_point_t resume = centre("Resume"), reset = centre("Reset");
    expect(start.x == resume.x && start.y == resume.y, "paused primary retains its transport position");
    expect(abs(reset.x - lap.x) >= 100 || abs(reset.y - lap.y) >= 100,
        "Reset does not replace the running Lap target");
    expect(reset.x < resume.x && lap.x > pause.x, "Reset stays left and Lap has its own right slot");
    transport_clearance("Resume", "Reset");
    orb_time_snapshot_t before, after;
    orb_time_snapshot((int64_t)s_now * 1000, &before);
    tap_at(lap);
    orb_time_snapshot((int64_t)s_now * 1000, &after);
    expect(after.stopwatch_elapsed_us == before.stopwatch_elapsed_us && after.lap_count == before.lap_count,
        "one extra tap at Lap after Pause retains time and laps");
    capture(directory, "stopwatch-paused");
    if (after.stopwatch_elapsed_us) {
        tap_at(reset);
        orb_time_snapshot((int64_t)s_now * 1000, &after);
        expect(!after.stopwatch_elapsed_us && !after.lap_count, "deliberate Reset clears stopped stopwatch");
        lv_point_t restarted = centre("Start");
        expect(restarted.x == start.x && restarted.y == start.y, "Reset returns to the same primary position");
    }
}
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create();
    orb_ui_init(NULL); expect(orb_model_init(), "isolated model initializes");
    if (argc > 3 && !strcmp(argv[3], "ax3"))
        expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "AX3 loads");
    if (argc > 3 && !strcmp(argv[3], "rt")) orb_ui_set_reduce_transparency(true);
    s_pointer = lv_indev_create(); lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display); lv_indev_set_read_cb(s_pointer, read_pointer);
    if (!strcmp(argv[1], "paging")) paging();
    else if (!strcmp(argv[1], "transport")) transport(argv[2]);
    else return 2;
    printf("Time steady %s: %u checks, %u failures\n", argv[1], s_checks, s_failures);
    orb_time_view_destroy(s_view); lv_indev_delete(s_pointer); lv_deinit();
    return s_failures ? 1 : 0;
}
