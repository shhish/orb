/* Design 2.0 Usage: measured line boxes, fixed concentric comparison tracks
 * and allocation-free retained centre paging. All data stays process-local. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks, s_ui_allocations;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static uint8_t s_pixels[SIM_D * SIM_D * 3];
#define CHECK(value) do { s_checks++; if (!(value)) { \
    fprintf(stderr, "usage %s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void *ui_alloc(size_t bytes) { s_ui_allocations++; return malloc(bytes); }
static void ui_free(void *value) { free(value); }
static void pointer_read(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_input; }
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 16) {
        s_now += 16; orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}
static lv_obj_t *find(lv_obj_t *object, const char *text)
{
    if (!lv_obj_is_visible(object)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) && strstr(lv_label_get_text(object), text)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find(lv_obj_get_child(object, i), text);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *label(const char *text) { return find(orb_shell_root(s_shell), text); }
static void check_line(lv_obj_t *text)
{
    CHECK(text && lv_obj_check_type(text, &lv_label_class));
    lv_area_t area; lv_obj_get_coords(text, &area);
    int available = orb_chord_width(area.y1, area.y2, 58);
    CHECK(lv_area_get_width(&area) <= available);
    CHECK(area.x1 >= 233 - available / 2);
    CHECK(area.x2 <= 233 + (available + 1) / 2 - 1);
    lv_point_t measured;
    lv_text_get_size(&measured, lv_label_get_text(text), lv_obj_get_style_text_font(text, 0),
        lv_obj_get_style_text_letter_space(text, 0), lv_obj_get_style_text_line_space(text, 0),
        LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (measured.x > lv_area_get_width(&area)) fprintf(stderr, "usage overflow: %s ink=%ld lane=%d y=%d..%d\n", lv_label_get_text(text), (long)measured.x, lv_area_get_width(&area), area.y1, area.y2);
    CHECK(measured.x <= lv_area_get_width(&area));
    CHECK(measured.y <= lv_area_get_height(&area));
    CHECK(!strstr(lv_label_get_text(text), "…"));
    /* Geometry alone cannot establish that retained labels reached the panel.
     * No track enters these safe line boxes, so lit pixels must be text. */
    unsigned lit = 0;
    for (int y = area.y1; y <= area.y2; y++) {
        for (int x = area.x1; x <= area.x2; x++) {
            const uint8_t *pixel = &s_pixels[(y * SIM_D + x) * 3];
            lit += pixel[0] > 96 || pixel[1] > 96 || pixel[2] > 96;
        }
    }
    CHECK(lit > 0);
}
static void check_page(const char *provider, const char *value, bool stale)
{
    if (!label(value)) fprintf(stderr, "usage expected %s %s at %u ms\n", provider, value, s_now);
    CHECK(label(provider)); CHECK(label(value)); CHECK(label(stale ? "Last known" : "1 working"));
    CHECK(!label(!strcmp(provider, "Claude") ? "Codex" : "Claude"));
    lv_obj_t *page = lv_obj_get_parent(label(provider));
    sim_display_rgb(s_pixels);
    int previous_bottom = -1;
    for (unsigned i = 0; i < lv_obj_get_child_count(page); i++) {
        lv_obj_t *child = lv_obj_get_child(page, i);
        if (!lv_obj_is_visible(child) || !lv_obj_check_type(child, &lv_label_class)) continue;
        check_line(child);
        lv_area_t area; lv_obj_get_coords(child, &area);
        CHECK(area.y1 > previous_bottom);
        previous_bottom = area.y2;
    }
    check_line(label(stale ? "Last known" : "1 working"));
    lv_area_t footer; lv_obj_get_coords(label(stale ? "Last known" : "1 working"), &footer);
    CHECK(footer.y1 > previous_bottom);
    if (stale) CHECK(label("Last known"));
}
static void check_week(const char *expected_value, const char *expected_reset)
{
    lv_obj_t *first = label("Week "); CHECK(first);
    lv_obj_t *page = lv_obj_get_parent(first);
    bool weekly = false;
    unsigned line = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(page); i++) {
        lv_obj_t *child = lv_obj_get_child(page, i);
        weekly |= child == first;
        if (!weekly || !lv_obj_is_visible(child) || !lv_obj_check_type(child, &lv_label_class)) continue;
        CHECK(line < 2);
        CHECK(!strcmp(lv_label_get_text(child), line ? expected_reset : expected_value));
        line++;
    }
    /* Percentage/stale and reset time now occupy two reserved semantic lines.
     * A 9-to-100 change must not rewrap prose or move the hero group. */
    CHECK(line == 2);
}
/* Polish review #4: both rings belong to the visible provider. The outer ring
 * is the focal value, the inner ring that provider's separate week (hidden
 * when it has none), and both wear the provider's colour. */
static void check_rings(lv_obj_t *const tracks[2], lv_color_t colour, int outer, int inner)
{
    for (unsigned i = 0; i < 2; i++) {
        CHECK(lv_color_eq(lv_obj_get_style_arc_color(tracks[i], LV_PART_INDICATOR), colour));
        CHECK(lv_color_eq(lv_obj_get_style_arc_color(tracks[i], LV_PART_MAIN), colour));
    }
    CHECK(!lv_obj_is_hidden(tracks[0]));
    CHECK(lv_obj_get_style_arc_opa(tracks[0], LV_PART_INDICATOR) == (outer < 0 ? LV_OPA_TRANSP : LV_OPA_COVER));
    CHECK(lv_arc_get_value(tracks[0]) == (outer < 0 ? 0 : outer * 10));
    CHECK(lv_obj_is_hidden(tracks[1]) == (inner < 0));
    if (inner >= 0) CHECK(lv_arc_get_value(tracks[1]) == inner * 10);
}
static void rings(lv_obj_t *object, lv_obj_t *found[2], unsigned *count)
{
    if (!lv_obj_is_visible(object)) return;
    if (lv_obj_check_type(object, &lv_arc_class)) {
        CHECK(*count < 2); found[(*count)++] = object;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) rings(lv_obj_get_child(object, i), found, count);
}
static void vertical(int delta)
{
    s_input.point = (lv_point_t){233, delta < 0 ? 320 : 150}; s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer); advance(32);
    for (unsigned i = 1; i <= 8; i++) {
        s_input.point.y += delta; lv_indev_read(s_pointer); advance(16);
    }
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(200);
}
static void rim(float from, float to)
{
    /* Same physical radius/arc as the list scroll regression, sampled at the
     * display cadence. This exercises pointer capture, detents and release. */
    unsigned allocated = s_ui_allocations;
    for (unsigned at = 0; at <= 704; at += 16) {
        float angle = (from + (to - from) * at / 704) * (float)M_PI / 180;
        s_input.point = (lv_point_t){(int)lroundf(233 + 215 * cosf(angle)),
                                    (int)lroundf(233 + 215 * sinf(angle))};
        s_input.state = LV_INDEV_STATE_PRESSED;
        lv_indev_read(s_pointer); advance(16);
        CHECK(s_ui_allocations == allocated);
    }
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer);
    CHECK(s_ui_allocations == allocated);
    /* After release/settling, nav_tick may recapture the Back destination's
     * Face button (orb_glass_apply). That idle cache work is not rim paging. */
    advance(1400);
}
static orb_usage_snapshot_t usage(bool stale)
{
    struct tm reset = {.tm_year = 126, .tm_mon = 9, .tm_mday = 28, .tm_hour = 23, .tm_min = 59};
    uint64_t resets = (uint64_t)mktime(&reset);
    orb_usage_snapshot_t data = {.count = 4};
    for (unsigned i = 0; i < 4; i++) {
        data.windows[i] = (orb_usage_window_t){.provider = i / 2, .used_percent = i < 2 ? 100 : 85,
            .resets_at = resets, .observed_at = resets - 600, .stale = stale, .source = ORB_USAGE_USAGE};
        snprintf(data.windows[i].label, sizeof(data.windows[i].label), "%s", i % 2 ? "week" : "5 hours");
    }
    return data;
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); CHECK(setenv("TZ", "UTC", 1) == 0); tzset();
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(&(orb_ui_config_t){.alloc = ui_alloc, .free = ui_free});
    CHECK(orb_model_init()); CHECK(orb_home_init()); orb_model_set_connection(true, true);
    orb_time_set_clock_format(false);
    bool ax = strncmp(argv[1], "ax3", 3) == 0, bold = !strcmp(argv[1], "ax3-bold-rt");
    CHECK(orb_fonts_set_content_size(ax ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(bold);
    s_pointer = lv_indev_create(); CHECK(s_pointer);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER); lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, pointer_read); lv_indev_set_mode(s_pointer, LV_INDEV_MODE_EVENT);
    orb_session_t busy = {.id = "usage-test", .title = "Synthetic", .agent = ORB_AGENT_CLAUDE, .state = ORB_SESSION_BUSY};
    CHECK(orb_model_upsert_session(&busy));
    orb_usage_snapshot_t data = usage(true); CHECK(orb_model_set_usage(&data));
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .brightness = 70, .battery_pct = 80, .clock_time = "10:09"};
    orb_shell_update(s_shell, &status); orb_shell_navigate(s_shell, ORB_SHELL_USAGE); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_USAGE);
    check_page("Claude", "100%", true); CHECK(label("resets 11:59 PM")); CHECK(label("Week 100%"));
    check_week("Week 100%", "resets Wed 11:59 PM");
    lv_obj_t *tracks[2] = {0}; unsigned count = 0;
    rings(orb_shell_root(s_shell), tracks, &count); CHECK(count == 2);
    const int diameters[] = {446, 394}, widths[] = {10, 6};
    for (unsigned i = 0; i < 2; i++) {
        CHECK(lv_obj_get_width(tracks[i]) == diameters[i]); CHECK(lv_obj_get_height(tracks[i]) == diameters[i]);
        lv_area_t area; lv_obj_get_coords(tracks[i], &area);
        CHECK(area.x1 == (SIM_D - diameters[i]) / 2 && area.y1 == area.x1);
        CHECK(lv_obj_get_style_arc_width(tracks[i], LV_PART_MAIN) == widths[i]);
        CHECK(lv_obj_get_style_arc_width(tracks[i], LV_PART_INDICATOR) == widths[i]);
        /* The shared gauge uses an 18% accent track and tenth-percent storage. */
        CHECK(lv_obj_get_style_arc_opa(tracks[i], LV_PART_MAIN) == 46);
        CHECK(lv_arc_get_min_value(tracks[i]) == 0 && lv_arc_get_max_value(tracks[i]) == 1000);
        CHECK(lv_arc_get_rotation(tracks[i]) == 270); CHECK(lv_arc_get_bg_angle_start(tracks[i]) == 0);
        CHECK(lv_arc_get_bg_angle_end(tracks[i]) == 360);
    }
    unsigned allocated = s_ui_allocations;
    CHECK(orb_shell_twist(s_shell, 1)); CHECK(s_ui_allocations == allocated); advance(100);
    check_page("Codex", "85%", true);
    check_week("Week 85%", "resets Wed 11:59 PM");
    check_rings(tracks, ORB_KIT_CODEX, 85, 85);
    lv_obj_t *codex = label("Codex"), *codex_page = lv_obj_get_parent(codex);
    for (unsigned i = 0; i < 16; i++) {
        CHECK(orb_shell_twist(s_shell, i % 2 ? 1 : -1)); CHECK(s_ui_allocations == allocated);
    }
    CHECK(label("Codex") == codex); CHECK(lv_obj_get_parent(codex) == codex_page);
    vertical(22); check_page("Claude", "100%", true);
    rim(40, 140); check_page("Codex", "85%", true);
    rim(140, 40); check_page("Claude", "100%", true); check_rings(tracks, ORB_KIT_CLAUDE, 100, 100);
    vertical(-22); check_page("Codex", "85%", true);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_USAGE);
    lv_obj_t *after[2] = {0}; count = 0; rings(orb_shell_root(s_shell), after, &count);
    CHECK(count == 2 && after[0] == tracks[0] && after[1] == tracks[1]);
    /* Distinct values per provider and window: every page/ring pairing must
     * read its own provider's number (usage.codex once drew Claude's). */
    orb_usage_snapshot_t mixed = data;
    const float mixed_values[] = {62, 41, 38, 71};
    for (unsigned i = 0; i < 4; i++) { mixed.windows[i].used_percent = mixed_values[i]; mixed.windows[i].stale = false; }
    /* Endpoint/chord assertions follow the finite Alive telemetry tween. */
    CHECK(orb_model_set_usage(&mixed)); advance(550);
    check_page("Codex", "38%", false); check_rings(tracks, ORB_KIT_CODEX, 38, 71);
    CHECK(orb_shell_twist(s_shell, -1)); advance(100);
    check_page("Claude", "62%", false); check_rings(tracks, ORB_KIT_CLAUDE, 62, 41);
    /* Weekly-only for both providers: the week is the focal outer ring and no
     * unlabelled inner track remains. */
    mixed.count = 2; mixed.windows[0] = mixed.windows[1]; mixed.windows[1] = mixed.windows[3];
    CHECK(orb_model_set_usage(&mixed)); advance(550);
    check_page("Claude", "41%", false); CHECK(label("Week")); check_rings(tracks, ORB_KIT_CLAUDE, 41, -1);
    CHECK(orb_shell_twist(s_shell, 1)); advance(100);
    check_page("Codex", "71%", false); CHECK(label("Week")); check_rings(tracks, ORB_KIT_CODEX, 71, -1);
    /* Claude unknown while Codex is known: Claude's page shows only its empty track. */
    mixed = data; for (unsigned i = 0; i < 4; i++) mixed.windows[i].used_percent = mixed_values[i];
    mixed.windows[0] = mixed.windows[2]; mixed.windows[1] = mixed.windows[3]; mixed.count = 2;
    mixed.unknown[0] = true; CHECK(orb_model_set_usage(&mixed)); advance(550);
    check_rings(tracks, ORB_KIT_CODEX, 38, 71);
    CHECK(orb_shell_twist(s_shell, -1)); advance(100);
    check_page("Claude", "Not available", false); check_rings(tracks, ORB_KIT_CLAUDE, -1, -1);
    CHECK(orb_shell_twist(s_shell, 1)); advance(100);
    CHECK(orb_model_set_usage(&data)); advance(550);
    data.count = 2; data.unknown[1] = true; CHECK(orb_model_set_usage(&data)); advance(550);
    check_page("Codex", "Not available", false); CHECK(!label("0%"));
    check_rings(tracks, ORB_KIT_CODEX, -1, -1);
    data.unknown[1] = false; data.count = 4; data.windows[2].used_percent = 0; data.windows[2].stale = false;
    data.windows[3].stale = false; CHECK(orb_model_set_usage(&data)); advance(550);
    check_page("Codex", "0%", false); CHECK(!label("Not available")); CHECK(!label("stale"));
    check_week("Week 85%", "resets Wed 11:59 PM");
    check_rings(tracks, ORB_KIT_CODEX, 0, 85);
    /* A spent plan with its week still shown: credits replace the week's reset day. */
    data.windows[2].used_percent = 100; data.windows[2].on_credits = true;
    CHECK(orb_model_set_usage(&data)); advance(550);
    check_page("Codex", "100%", false); check_week("Week 85%", "Using credits"); CHECK(!label("resets Wed 11:59 PM"));
    data.windows[2].on_credits = false;
    /* Codex has reported only its weekly cap: the week becomes the value. */
    data.count = 3; data.windows[2] = data.windows[3]; data.windows[2].used_percent = 99;
    CHECK(orb_model_set_usage(&data)); advance(550);
    check_page("Codex", "99%", false); CHECK(!label("Not available"));
    CHECK(label("resets Wed 11:59 PM")); CHECK(label("Week"));
    check_rings(tracks, ORB_KIT_CODEX, 99, -1);
    /* Weekly-only: "Using credits" takes the empty reset slot. */
    data.windows[2].used_percent = 100;
    data.windows[2].on_credits = true;
    CHECK(orb_model_set_usage(&data)); advance(550);
    check_page("Codex", "100%", false);
    CHECK(label("Using credits"));
    CHECK(label("Week"));
    data.windows[2].on_credits = false;
    CHECK(orb_model_set_usage(&data)); advance(550);
    check_page("Codex", "100%", false);
    CHECK(!label("Using credits"));
    /* Any valid quota window is useful, including changed provider durations.
     * Never relabel it as five hours or lose it behind Not available. */
    strcpy(data.windows[2].label, "3 hours"); data.windows[2].used_percent = 42;
    CHECK(orb_model_set_usage(&data)); advance(550);
    check_page("Codex", "42%", false); CHECK(label("3 hr")); CHECK(!label("5 hr"));
    CHECK(!label("Week"));
    strcpy(data.windows[2].label, "90 minutes");
    CHECK(orb_model_set_usage(&data)); advance(550);
    check_page("Codex", "42%", false); CHECK(label("90 min"));
    data.count = 4; data.windows[3] = data.windows[2];
    strcpy(data.windows[3].label, "week"); data.windows[3].used_percent = 85;
    CHECK(orb_model_set_usage(&data)); advance(550);
    check_page("Codex", "42%", false); CHECK(label("90 min · resets"));
    check_week("Week 85%", "resets Wed 11:59 PM");
    strcpy(data.windows[2].label, "3 hours");
    CHECK(orb_model_set_usage(&data)); advance(550);
    check_page("Codex", "42%", false); CHECK(label("3 hr · resets"));
    check_week("Week 85%", "resets Wed 11:59 PM"); check_rings(tracks, ORB_KIT_CODEX, 42, 85);
    /* A model-specific Claude weekly window has a real, named quota too. */
    data.count = 1; data.windows[0] = data.windows[2];
    data.windows[0].provider = ORB_AGENT_CLAUDE; strcpy(data.windows[0].label, "Sonnet");
    CHECK(orb_model_set_usage(&data)); CHECK(orb_shell_twist(s_shell, -1)); advance(550);
    check_page("Claude", "42%", false); CHECK(label("Sonnet"));
    CHECK(!label("Not available"));
    orb_time_set_clock_format(true); data.windows[0].used_percent = 43;
    CHECK(orb_model_set_usage(&data)); advance(550);
    CHECK(label("resets Wed 23:59")); CHECK(!label("11:59 PM"));
    orb_prefs_t prefs; orb_model_prefs_defaults(&prefs); prefs.sources[ORB_AGENT_CLAUDE] = false;
    CHECK(orb_model_set_prefs(&prefs)); advance(100);
    CHECK(!label("0 working")); CHECK(!label("1 working"));
    orb_shell_destroy(s_shell); lv_deinit();
    printf("usage %s: %u checks, retained paging: 0 Orb UI allocations\n", argv[1], s_checks);
    return 0;
}
