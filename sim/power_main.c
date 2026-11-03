/* Deterministic power presentation and screenshot contract: real controller,
 * production fonts/LVGL, synthetic public data, no board or shell mutation. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "orb_fonts.h"
#include "orb_ui.h"
#include "power_view.h"
#include "sim_display.h"
#include "src/libs/lodepng/lodepng.h"

static uint32_t s_now;
static unsigned s_checks, s_images;
static lv_display_t *s_display;
static power_view_t s_view;
static orb_power_controller_t s_power;
static const char *s_output;
static bool s_contrast;
static power_face_data_t s_data = {9, 41, 2};
static uint8_t s_pixels[466 * 466 * 3];
static uint8_t s_previous_pixels[466 * 466 * 3];

static uint32_t tick(void) { return s_now; }

static void check(bool ok, const char *why)
{
    s_checks++;
    if (ok) return;
    fprintf(stderr, "power preview: %s\n", why);
    exit(1);
}

static void safe(lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int points[4][2] = {{area.x1, area.y1}, {area.x2, area.y1},
                        {area.x1, area.y2}, {area.x2, area.y2}};
    for (unsigned i = 0; i < 4; i++) {
        int x = points[i][0] - 233, y = points[i][1] - 233;
        check(x * x + y * y <= 217 * 217, "semantic label exceeds the round text safe area");
    }
}

static bool render(uint64_t now)
{
    check(orb_power_tick(&s_power, now), "advance monotonic power time");
    s_now = (uint32_t)now;
    orb_power_snapshot_t snap = orb_power_get_snapshot(&s_power);
    check(snap.redact, "all preview states remain redacted");
    bool changed = power_view_update(&s_view, &snap, s_data,
                                     orb_ui_reduce_motion(), now);
    lv_refr_now(s_display);
    safe(s_view.clock);
    safe(s_view.mark);
    safe(s_view.hand);
    return changed;
}

static void shot(const char *name)
{
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s.png", s_output, name);
    check(length > 0 && (size_t)length < sizeof(path), "PNG path fits");
    check(sim_display_save_png(path), "write PNG");
    unsigned char *decoded = NULL;
    unsigned width = 0, height = 0;
    FILE *file = fopen(path, "rb");
    check(file != NULL && fseek(file, 0, SEEK_END) == 0, "open the saved PNG artifact");
    long length_on_disk = ftell(file);
    check(length_on_disk > 0 && fseek(file, 0, SEEK_SET) == 0, "measure saved PNG bytes");
    unsigned char *encoded = lv_malloc((size_t)length_on_disk);
    check(encoded != NULL && fread(encoded, 1, (size_t)length_on_disk, file) == (size_t)length_on_disk,
          "read the actual saved PNG bytes");
    check(fclose(file) == 0, "close the saved PNG artifact");
    check(lodepng_decode32(&decoded, &width, &height, encoded, (size_t)length_on_disk) == 0,
          "decode the saved PNG artifact");
    lv_free(encoded);
    sim_display_rgb(s_pixels);
    /* Orb's LVGL fork returns an owned draw buffer, not upstream's raw pointer. */
    lv_draw_buf_t *buffer = (lv_draw_buf_t *)decoded;
    bool exact = width == 466 && height == 466;
    for (unsigned pixel = 0; exact && pixel < 466 * 466; pixel++)
        exact = !memcmp(buffer->data + pixel * 4, s_pixels + pixel * 3, 3);
    check(exact, "saved PNG contains exactly the actual complete panel pixels");
    lv_draw_buf_destroy(buffer);
    s_images++;
}

static void aod(void)
{
    check(orb_power_set_battery(&s_power, 80, true, false, 0), "USB keeps all burn-in corners observable");
    (void)render(29999);
    check(render(30000), "enter Always On preview");
    check(render(30250), "settle Always On entry");
    orb_power_snapshot_t snap = orb_power_get_snapshot(&s_power);
    check(snap.state == ORB_POWER_ALWAYS_ON && snap.frame_interval_ms == 1000,
          "Always On grants 1 Hz public-data opportunities");
    check(snap.brightness_pct == 9, "Always On uses its named nine percent setting");
    check(!strcmp(lv_label_get_text(s_view.mark), "2 waiting"), "aggregate count uses generic copy");
    check(lv_obj_get_x(s_view.mark) - (lv_obj_get_x(s_view.hand) + lv_obj_get_width(s_view.hand)) >= 16,
          "attention symbol clears text by sixteen pixels");
    shot("aod");
    sim_display_rgb(s_pixels);
    unsigned lit = 0;
    for (unsigned i = 0; i < 466 * 466; i++)
        lit += s_pixels[3 * i] || s_pixels[3 * i + 1] || s_pixels[3 * i + 2];
    check(lit < 466 * 466 / 12, "Always On lights less than one twelfth of the canvas");
    (void)sim_display_take_changed();
    check(!render(30300), "unchanged Always On never invalidates periodically");
    check(!sim_display_take_changed(), "unchanged public data produces zero panel writes");
    s_data.waiting = 3;
    check(!render(30500), "attention burst coalesces inside the 1 Hz allowance");
    check(render(31250), "aggregate attention update at next 1 Hz allowance");
    s_data.waiting = 0;
    check(render(32250), "zero attention updates the face");
    check(power_view_frame_interval_ms(&s_view) == 16, "attention presence reflow requests intermediate frames");
    shot("attention-clear-000");
    float old_clock_y = s_view.pose.clock_y;
    check(render(32330), "reflow draws an intermediate frame");
    if (!orb_ui_reduce_motion()) check(s_view.pose.clock_y > old_clock_y, "clock recentres continuously when attention clears");
    shot("attention-clear-080");
    check(render(32500), "attention-clear reflow reaches endpoint");
    check(power_view_frame_interval_ms(&s_view) == 0, "settled attention reflow releases frame cadence");
    check(lv_obj_is_hidden(s_view.mark) && lv_obj_is_hidden(s_view.hand), "zero omits the entire attention mark");
    check(fabsf(s_view.pose.clock_y - (233 + snap.shift_y)) < .01f, "clear clock ink centres on equator plus burn-in offset");
    shot("attention-clear-250");
    shot("aod-clear");
    s_data.waiting = 100;
    check(render(33500), "maximum aggregate fixture");
    check(!strcmp(lv_label_get_text(s_view.mark), "99+ waiting"), "large counts have a bounded generic label");
    shot("attention-arrive-000");
    (void)render(33580);
    shot("attention-arrive-080");
    (void)render(33750);
    shot("attention-arrive-250");
    shot("aod-count-cap");
    s_data.waiting = 2;
    (void)render(34750);
    int previous_x[3] = {lv_obj_get_x(s_view.clock), lv_obj_get_x(s_view.hand), lv_obj_get_x(s_view.mark)};
    int previous_y[3] = {lv_obj_get_y(s_view.clock), lv_obj_get_y(s_view.hand), lv_obj_get_y(s_view.mark)};
    static const int shifts[4][2] = {{-1,-1},{1,-1},{1,1},{-1,1}};
    for (unsigned i = 1; i < 4; i++) {
        check(render(60000u * i), "minute shift updates only on its boundary");
        snap = orb_power_get_snapshot(&s_power);
        check(snap.shift_x == shifts[i][0] && snap.shift_y == shifts[i][1], "minute corner matches controller");
        lv_obj_t *elements[] = {s_view.clock, s_view.hand, s_view.mark};
        for (unsigned j = 0; j < 3; j++) {
            check(lv_obj_get_x(elements[j]) - previous_x[j] == shifts[i][0] - shifts[i - 1][0] &&
                  lv_obj_get_y(elements[j]) - previous_y[j] == shifts[i][1] - shifts[i - 1][1],
                  "burn-in shift moves every Always On element by exactly the same offset");
            previous_x[j] = lv_obj_get_x(elements[j]);
            previous_y[j] = lv_obj_get_y(elements[j]);
        }
        char name[32];
        snprintf(name, sizeof(name), "aod-shift-%u", i);
        shot(name);
    }
}

static void moment(const char *name, bool wake, uint64_t start)
{
    static const unsigned offsets[] = {0, 1, 40, 80, 120, 149, 150, 200, 249, 250, 280, 320, 399, 400, 500};
    bool from_dark = !orb_power_get_snapshot(&s_power).panel_on;
    sim_display_rgb(s_previous_pixels);
    if (wake) {
        orb_power_input_result_t result = orb_power_wake(&s_power, ORB_POWER_WAKE_PICKUP, start);
        check(result.accepted, "synthetic pickup admits redacted wake");
    }
    if (!wake) check(orb_power_request_sleep(&s_power, start), "explicit sleep transition fixture");
    float previous_clock_opa = wake ? 0 : 1;
    float previous_scale = .9f;
    float initial_clock_y = 0;
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        unsigned offset = offsets[i];
        (void)render(start + offset);
        sim_display_rgb(s_pixels);
        orb_power_snapshot_t snap = orb_power_get_snapshot(&s_power);
        unsigned duration = orb_ui_reduce_motion() ? 150 : wake ? 400 : 250;
        if (!i) {
            check(!memcmp(s_pixels, s_previous_pixels, sizeof(s_pixels)),
                  "transition start preserves every pixel from the preceding frame");
            initial_clock_y = s_view.pose.clock_y;
        }
        if (snap.moment != ORB_POWER_MOMENT_NONE) {
            check(snap.moment_duration_ms == duration, "direction-specific bounded transition duration");
            check(snap.frame_interval_ms == 16, "transition admits intermediate fade frames");
            if (offset == 40)
                check(orb_power_frame_due(&s_power, start, snap.revision),
                      "real controller scheduling admits an intermediate frame");
            check(lv_obj_get_style_opa(s_view.mark, 0) || wake || s_view.crossfade || s_view.progress > .99f,
                  "retained attention fades continuously instead of vanishing");
            if (wake) {
                check(s_view.pose.clock_opa >= previous_clock_opa, "wake clock opacity is monotonic");
                check(s_view.pose.scale >= previous_scale, "arriving attention scale settles monotonically");
                previous_scale = s_view.pose.scale;
            } else check(s_view.pose.clock_opa <= previous_clock_opa, "sleep clock opacity is monotonic");
            previous_clock_opa = s_view.pose.clock_opa;
            if (orb_ui_reduce_motion()) {
                check(s_view.pose.scale == 1, "Reduce Motion disables scale");
                check(s_view.pose.clock_y == initial_clock_y, "Reduce Motion has no parallax or positional motion");
            }
        }
        if (offset == duration - 1) memcpy(s_previous_pixels, s_pixels, sizeof(s_pixels));
        if (offset == duration) {
            if (wake && from_dark) {
                check(fabsf(initial_clock_y - s_view.pose.clock_y - (orb_ui_reduce_motion() ? 0 : 2)) < .001f,
                      "dark wake lifts time exactly two pixels, or zero under Reduce Motion");
                check(s_view.pose.scale == 1, "attention finishes at its semantic size");
            }
            unsigned changed = 0;
            for (unsigned px = 0; px < sizeof(s_pixels); px++) changed += s_pixels[px] != s_previous_pixels[px];
            check(changed < 1200, "endpoint has no one-frame content pop");
        }
        char filename[64];
        snprintf(filename, sizeof(filename), "%s-%03u", name, offset);
        shot(filename);
    }
    orb_power_snapshot_t snap = orb_power_get_snapshot(&s_power);
    check(snap.moment == ORB_POWER_MOMENT_NONE, "transition ends without a running animation");
    check(lv_obj_is_hidden(s_view.source_image),
          "dissolve source is absent at rest");
    check(!render(start + 600), "settled endpoint causes no further redraw");
    if (!wake) {
        sim_display_rgb(s_pixels);
        unsigned ink = 0;
        for (unsigned i = 0; i < sizeof(s_pixels); i++) ink += s_pixels[i] != 0;
        check(ink == 0, "sleep endpoint is true black across the whole panel");
    } else check(!lv_obj_is_hidden(s_view.mark), "attention remains after wake endpoint");
}

static void rest_sequence(void)
{
    orb_power_init(&s_power, 0);
    check(orb_power_set_battery(&s_power, 80, true, false, 0), "USB preserves the original Always On window");
    orb_power_preferences_t prefs = orb_power_default_preferences();
    prefs.reduce_motion = orb_ui_reduce_motion();
    check(orb_power_set_preferences(&s_power, &prefs, 0), "select motion preference for rest sequence");
    s_view.initialized = false;
    (void)render(19999);
    (void)render(20250);
    (void)render(29999);
    sim_display_rgb(s_previous_pixels);
    static const unsigned offsets[] = {0, 1, 40, 80, 120, 149, 150, 200, 249, 250, 400};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        (void)render(30000 + offsets[i]);
        if (!i) {
            sim_display_rgb(s_pixels);
            check(!memcmp(s_pixels, s_previous_pixels, sizeof(s_pixels)), "Always On entry has no positional jump");
        }
        char name[40];
        snprintf(name, sizeof(name), "rest-%03u", offsets[i]);
        shot(name);
    }
}

static void reflow_at_minute_boundary(void)
{
    orb_power_init(&s_power, 0);
    orb_power_preferences_t prefs = orb_power_default_preferences();
    prefs.reduce_motion = orb_ui_reduce_motion();
    check(orb_power_set_preferences(&s_power, &prefs, 0), "minute-reflow motion policy");
    s_view.initialized = false;
    s_data.waiting = 2;
    (void)render(59000);
    float previous_x = s_view.pose.clock_x;
    s_data.waiting = 0;
    check(render(60000), "attention presence changes exactly at a burn-in boundary");
    check(s_view.pose.clock_x == previous_x + 2, "minute shift stays discrete at reflow admission");
    if (orb_ui_reduce_motion())
        check(lv_obj_get_x(s_view.source_image) == 2, "reduced-motion source shifts with all destination elements");
    check(render(60080), "minute reflow intermediate");
    check(s_view.pose.clock_x == previous_x + 2, "minute shift does not drift during reflow");
    check(render(60250), "minute reflow settles");
    check(!power_view_frame_interval_ms(&s_view), "minute reflow returns to settled cadence");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fputs("usage: orb_power_sim OUTPUT_DIRECTORY [ax3] [bold] [rt] [rm] [ic]\n", stderr);
        return 2;
    }
    s_output = argv[1];
    check(mkdir(s_output, 0755) == 0 || errno == EEXIST, "create preview output directory");
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    lv_lock();
    orb_ui_init(NULL);
    check(orb_fonts_preload_default() == ORB_FONTS_OK, "load production Inter and Framework7 assets");
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "ax3")) check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "AX3 type");
        else if (!strcmp(argv[i], "bold")) check(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "Bold Text");
        else if (!strcmp(argv[i], "rt")) orb_ui_set_reduce_transparency(true);
        else if (!strcmp(argv[i], "rm")) orb_ui_set_reduce_motion(true);
        else if (!strcmp(argv[i], "ic")) s_contrast = true;
        else { fprintf(stderr, "unknown variant: %s\n", argv[i]); return 2; }
    }
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    power_view_create(&s_view, lv_screen_active(), s_contrast);
    orb_power_init(&s_power, 0);
    orb_power_preferences_t prefs = orb_power_default_preferences();
    prefs.reduce_motion = orb_ui_reduce_motion();
    check(orb_power_set_preferences(&s_power, &prefs, 0), "select actual power motion policy");
    aod();
    (void)render(299999);
    moment("sleep", false, 300000);
    moment("wake", true, 301000);
    rest_sequence();
    moment("wake-aod", true, 31000);
    /* Interruption uses the visible partial fade; it never waits for sleep. */
    (void)render(61000);
    check(orb_power_request_sleep(&s_power, 331000), "explicit interrupted sleep fixture");
    (void)render(331000);
    (void)render(331080);
    moment("wake-interrupted", true, 331080);
    reflow_at_minute_boundary();
    lv_unlock();
    printf("power preview: %u checks passed, %u PNGs\n", s_checks, s_images);
    return 0;
}
