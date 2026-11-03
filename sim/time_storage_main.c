/* Production Time persistence-failure previews. The real service is compiled
 * with ORB_TIME_TEST_STORAGE; the simulator's unmounted storage returns a real
 * save_error. No record, user configuration or mounted filesystem is touched. */
#include "orb_fonts.h"
#include "orb_fs.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_time.h"
#include "orb_time_view.h"
#include "sim_display.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint32_t s_now = 1000;
static const time_t s_wall = 1793182140;
static lv_display_t *s_display;
static orb_time_view_t *s_view;
static unsigned s_checks, s_failures, s_reads, s_writes;
static const char *s_out, *s_scene = "setup";

/* These satisfy the storage-enabled production service's unreachable I/O
 * branches. Existing sim_apps.c advertises !orb_fs_ready(), so the real
 * service must stop before asking either stub to access a record. */
esp_err_t orb_fs_read(const char *id, const char *key, void *data, size_t capacity, size_t *size)
{
    (void)id; (void)key; (void)data; (void)capacity;
    if (size) *size = 0;
    ++s_reads;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size)
{
    (void)id; (void)key; (void)data; (void)size;
    ++s_writes;
    return ESP_ERR_INVALID_STATE;
}

static uint32_t tick(void) { return s_now; }

static void expect(bool valid, const char *why)
{
    ++s_checks;
    if (valid) return;
    ++s_failures;
    fprintf(stderr, "Time storage [%s]: %s\n", s_scene, why);
}

static orb_time_snapshot_t snapshot(void)
{
    orb_time_snapshot_t state;
    orb_time_snapshot((int64_t)s_now * 1000, &state);
    return state;
}

static void settle(void)
{
    for (unsigned i = 0; i < 48; ++i) {
        s_now += 16;
        orb_time_poll((int64_t)s_now * 1000, s_wall);
        orb_time_persist_service();
        if (s_view) orb_time_view_poll(s_view);
        lv_timer_handler();
    }
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(s_display);
}

static void reset(orb_time_page_t page)
{
    if (s_view) orb_time_view_destroy(s_view);
    s_view = NULL;
    orb_time_init();
    orb_time_poll((int64_t)s_now * 1000, s_wall);
    expect(snapshot().save_error, "unmounted storage produces a real initial-load error");
    s_view = orb_time_view_create(lv_screen_active(), NULL);
    expect(s_view != NULL, "create the production Time UI");
    orb_time_view_select(s_view, page);
    settle();
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    if (!lv_obj_is_visible(obj)) return NULL;
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(obj, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

static bool inside(lv_area_t ink, const lv_area_t *control)
{
    return ink.x1 >= control->x1 && ink.y1 >= control->y1 &&
        ink.x2 <= control->x2 && ink.y2 <= control->y2;
}

static void inspect(lv_obj_t *obj, unsigned *primary, unsigned *glass)
{
    if (!lv_obj_is_visible(obj) || !lv_obj_get_style_opa(obj, 0)) return;
    if (orb_glass_get_amount(obj) > 0) {
        ++*glass;
        int height = lv_obj_get_height(obj), width = lv_obj_get_width(obj);
        expect(height == ORB_KIT_SECONDARY_H || height == ORB_KIT_PRIMARY_H ||
            (height == width && (height == ORB_KIT_DISC || height == ORB_KIT_DISC_MINI)),
            "error action retains a canonical kit capsule or disc size");
        if (lv_obj_get_style_border_width(obj, 0) == 1 && lv_obj_get_style_border_opa(obj, 0) == 140) ++*primary;
        lv_area_t bounds; lv_obj_get_coords(obj, &bounds);
        int inset = ORB_SCREEN_R - ORB_RIM_MARGIN_GLASS;
        int xs[] = {bounds.x1 - 233, bounds.x2 - 233};
        int ys[] = {bounds.y1 - 233, bounds.y2 - 233};
        /* Capsules' bounding corners lie outside their rounded painted edge;
         * the system chord contract intentionally reserves the full box. */
        bool fits = true;
        for (unsigned x = 0; x < 2; ++x) for (unsigned y = 0; y < 2; ++y)
            fits &= xs[x] * xs[x] + ys[y] * ys[y] <= inset * inset;
        expect(fits, "complete error-action box fits the circular glass safe area");
        for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i) {
            lv_obj_t *label = lv_obj_get_child(obj, (int32_t)i);
            if (!lv_obj_check_type(label, &lv_label_class) || !lv_obj_get_style_opa(label, 0)) continue;
            lv_area_t ink; lv_obj_get_coords(label, &ink);
            lv_point_t extent;
            lv_text_get_size(&extent, lv_label_get_text(label), lv_obj_get_style_text_font(label, 0),
                lv_obj_get_style_text_letter_space(label, 0), lv_obj_get_style_text_line_space(label, 0),
                LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            expect(extent.x <= lv_obj_get_width(label) && extent.y <= lv_obj_get_height(label),
                "entire error capsule text fits on one line at the selected type size");
            expect(inside(ink, &bounds), "capsule label remains inside its actual control");
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i)
        inspect(lv_obj_get_child(obj, (int32_t)i), primary, glass);
}

static void capture(const char *name)
{
    s_scene = name;
    settle();
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", s_out, name);
    expect(sim_display_save_png(path), "capture the real failed-storage UI");
    orb_time_snapshot_t state = snapshot();
    expect(state.save_error && !state.saving, "capture shows a settled failed write, never a successful save");
    /* Polish review 14/15: one failure line under the rim title names the
     * failure, and Retry is a neutral secondary capsule. */
    lv_obj_t *retry = find_label(orb_time_view_root(s_view), "Retry");
    expect(retry != NULL, "failed storage has an explicit visible retry capsule");
    if (retry) expect(lv_obj_get_height(lv_obj_get_parent(retry)) == ORB_KIT_SECONDARY_H,
        "Retry is a 52px neutral secondary capsule");
    expect(find_label(orb_time_view_root(s_view), ORB_ICON_WARNING " Couldn’t save") != NULL,
        "failed storage states what failed in words beside a warning glyph");
    unsigned primary = 0, glass = 0;
    inspect(orb_time_view_root(s_view), &primary, &glass);
    expect(primary <= 1, "failed-storage surface has at most one tinted primary");
    expect(glass <= 3, "failed-storage surface stays within the glass-element budget");
    expect(!s_reads && !s_writes, "unmounted-storage failure performs no filesystem I/O");
}

static void retry_requires_tap(void)
{
    s_scene = "explicit-error-retry";
    lv_obj_t *label = find_label(orb_time_view_root(s_view), "Retry");
    if (!label) { expect(false, "retry action is present"); return; }
    lv_obj_t *control = lv_obj_get_parent(label);
    orb_time_snapshot_t before = snapshot();
    lv_obj_send_event(control, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(control, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_send_event(control, LV_EVENT_CLICKED, NULL);
    expect(snapshot().generation == before.generation && !snapshot().saving,
        "a cancelled scroll contact never retries a failed save");
    lv_obj_send_event(control, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(control, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(control, LV_EVENT_CLICKED, NULL);
    expect(snapshot().saving && snapshot().generation > before.generation,
        "an explicit fresh tap queues the production persistence retry");
    orb_time_persist_service();
    expect(snapshot().save_error && !snapshot().saving,
        "unavailable storage keeps the retry error truthful");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: orb_time_storage_sim OUTDIR [ax3 bold rt rm]\n"); return 2; }
    s_out = argv[1];
    if (mkdir(s_out, 0755) && errno != EEXIST) { perror(s_out); return 2; }
    setenv("TZ", "UTC0", 1); tzset();
    lv_init(); lv_tick_set_cb(tick); orb_ui_init(NULL);
    s_display = sim_display_create();
    expect(s_display != NULL && orb_model_init(), "initialize the isolated simulator model/display");
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "ax3")) expect(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "load AX3 fonts");
        else if (!strcmp(argv[i], "bold")) expect(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "load Bold Text fonts");
        else if (!strcmp(argv[i], "rt")) orb_ui_set_reduce_transparency(true);
        else if (!strcmp(argv[i], "rm")) orb_ui_set_reduce_motion(true);
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    reset(ORB_TIME_PAGE_ALARM);
    capture("alarm-save-error");
    lv_obj_t *options = find_label(orb_time_view_root(s_view), "Options");
    lv_obj_t *retry = find_label(orb_time_view_root(s_view), "Retry");
    lv_area_t a = {0}, b = {0};
    if (options && retry) {
        lv_obj_get_coords(lv_obj_get_parent(options), &a); lv_obj_get_coords(lv_obj_get_parent(retry), &b);
    }
    expect(options && retry && (a.x2 + 20 <= b.x1 || b.x2 + 20 <= a.x1),
        "Alarm keeps Options reachable beside Retry with 20 px between the capsules");
    retry_requires_tap();
    reset(ORB_TIME_PAGE_REMINDER);
    capture("reminders-empty-save-error");
    expect(orb_time_reminder_set("Water the basil", s_wall + 3600, NULL) &&
        orb_time_reminder_set("Take a break", s_wall + 7200, NULL), "create real reminders before a failed persistence flush");
    settle();
    capture("reminders-save-error");
    retry_requires_tap();
    orb_time_view_destroy(s_view);
    printf("Time storage errors: %u checks, %u failures; captures=%s\n", s_checks, s_failures, s_out);
    lv_deinit();
    return s_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
