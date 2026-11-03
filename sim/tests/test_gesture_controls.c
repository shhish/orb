/* Real LVGL input cadence, isolated controls and observable action delivery.
 * No forced indev reads: a finger can lift between two scheduled samples. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_sound.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "sim_display.h"
#include "src/misc/lv_text_private.h"

static uint32_t s_now;
static unsigned s_checks, s_brightness_actions, s_frames;
static int s_applied;
static int s_tracking_direction;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "controls at %u ms, %s:%d: %s\n", s_now, __FILE__, __LINE__, #value); \
    exit(1); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data) { *data = s_input; }
static void action(orb_shell_action_t action, void *ctx)
{
    if (action == ORB_SHELL_ACTION_BRIGHTNESS) {
        ++s_brightness_actions;
        int latest = orb_shell_brightness(s_shell);
        if (s_tracking_direction > 0) CHECK(latest >= s_applied);
        if (s_tracking_direction < 0) CHECK(latest <= s_applied);
        s_applied = latest;
    }
}
static void frame(lv_event_t *event)
{
    ++s_frames;
    /* The host framebuffer is synchronously drained. Match main.c's safe
     * presentation receipt when a test enables the real power controller. */
    orb_power_snapshot_t policy = orb_shell_power_snapshot(s_shell);
    orb_shell_power_display_ready(s_shell, policy.revision, policy.panel_on && policy.brightness_pct > 0);
}
static void advance(unsigned ms)
{
    uint32_t end = s_now + ms;
    while (s_now < end) {
        s_now += LV_MIN(4, end - s_now);
        orb_shell_poll(s_shell);
        lv_timer_handler();
    }
}
static unsigned input_period(void) { return LV_DEF_REFR_PERIOD; }
static void point(float radius, float angle, bool down)
{
    float rad = angle * (float)M_PI / 180;
    s_input.point = (lv_point_t){(int)lroundf(233 + radius * cosf(rad)),
                               (int)lroundf(233 + radius * sinf(rad))};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void hold(float radius, float angle, bool down)
{
    point(radius, angle, down);
    advance(input_period() + 20);
}
static void glide(float radius, float from, float to, unsigned duration)
{
    unsigned period = input_period();
    for (unsigned ms = period; ms < duration; ms += period) {
        float t = (float)ms / duration;
        float eased = 1 - (1 - t) * (1 - t); /* deceleration before lift */
        point(radius + 2 * sinf(t * (float)M_PI), from + (to - from) * eased, true);
        advance(period);
    }
    hold(radius, to, true);
}
static lv_obj_t *arc(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_arc_class)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = arc(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *text(lv_obj_t *root, const char *caption)
{
    if (lv_obj_is_hidden(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), caption)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = text(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}
static void dump_values(lv_obj_t *root)
{
    if (lv_obj_check_type(root, &lv_label_class) && strchr(lv_label_get_text(root), '%')) {
        lv_area_t box; lv_obj_get_coords(root, &box);
        fprintf(stderr, "percentage '%s' hidden=%d visible=%d box=%d,%d,%d,%d\n",
                lv_label_get_text(root), lv_obj_is_hidden(root), lv_obj_is_visible(root),
                (int)box.x1, (int)box.y1, (int)box.x2, (int)box.y2);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) dump_values(lv_obj_get_child(root, i));
}
static void click_text(const char *caption)
{
    lv_obj_t *label = text(orb_shell_root(s_shell), caption); CHECK(label);
    lv_obj_t *control = lv_obj_get_parent(label);
    while (control && !lv_obj_is_clickable(control)) control = lv_obj_get_parent(control);
    CHECK(control);
    lv_obj_scroll_to_view(control, LV_ANIM_OFF); advance(64);
    lv_area_t target;
    if (!orb_round_list_child_bounds(control, label, &target)) lv_obj_get_coords(label, &target);
    s_input.point = (lv_point_t){(target.x1 + target.x2) / 2, (target.y1 + target.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED; advance(input_period() + 20);
    s_input.state = LV_INDEV_STATE_RELEASED; advance(1200);
}
static void check_level_text_fit(lv_obj_t *slider, const char *caption)
{
    lv_obj_t *value = text(slider, caption);
    if (!value) value = text(orb_shell_root(s_shell), caption);
    CHECK(value);
    const lv_font_t *font = lv_obj_get_style_text_font(value, 0);
    lv_text_attributes_t attributes = {
        .letter_space = lv_obj_get_style_text_letter_space(value, 0),
        .text_flags = LV_TEXT_FLAG_EXPAND,
    };
    int32_t ink = lv_text_get_width(caption, strlen(caption), font, &attributes);
    orb_kit_value_info_t info; CHECK(orb_kit_value_info(slider, &info));
    if (ink > lv_obj_get_width(value))
        fprintf(stderr, "level '%s': ink=%d width=%d font_height=%u template_measures=%u\n",
                caption, (int)ink, (int)lv_obj_get_width(value), font->line_height,
                info.template_measures);
    CHECK(ink <= lv_obj_get_width(value));
    CHECK(font->line_height == orb_font_variant(ORB_TEXT_LARGE_TITLE,
                                              ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR)->line_height);
}
static unsigned primary_count(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return 0;
    unsigned count = orb_kit_is_primary(root);
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i)
        count += primary_count(lv_obj_get_child(root, i));
    return count;
}
static void save(const char *directory, const char *name)
{
    CHECK(primary_count(orb_shell_root(s_shell)) <= 1);
    CHECK(primary_count(lv_display_get_layer_top(s_display)) <= 1);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    CHECK(sim_display_save_png(path));
}

static void framebuffer_consistent(const char *directory, const char *name)
{
    /* The rendered panel is the oracle: fixed metrics alone cannot detect
     * incomplete damage after 9% becomes 100%. Freeze time for both images. */
    static uint8_t partial[SIM_D * SIM_D * 3], full[sizeof(partial)];
    sim_display_rgb(partial);
    char state[80]; snprintf(state, sizeof(state), "%s-partial", name); save(directory, state);
    lv_obj_invalidate(orb_shell_root(s_shell));
    lv_refr_now(s_display);
    sim_display_rgb(full);
    snprintf(state, sizeof(state), "%s-full", name); save(directory, state);
    unsigned changed = 0;
    int x1 = SIM_D, y1 = SIM_D, x2 = -1, y2 = -1;
    for (int y = 0; y < SIM_D; ++y) for (int x = 0; x < SIM_D; ++x) {
        size_t offset = (size_t)(y * SIM_D + x) * 3;
        if (!memcmp(partial + offset, full + offset, 3)) continue;
        ++changed; x1 = LV_MIN(x1, x); y1 = LV_MIN(y1, y);
        x2 = LV_MAX(x2, x); y2 = LV_MAX(y2, y);
    }
    if (changed) fprintf(stderr, "%s: partial/full mismatch %u pixels at %d,%d..%d,%d\n",
                         name, changed, x1, y1, x2, y2);
    CHECK(changed == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    bool ax = argc > 2;
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL);
    orb_apps_init(ORB_APP_NEEDS_IMU);
    CHECK(orb_model_init() && orb_home_init());
    orb_model_load_demo(); orb_home_load_demo(); orb_model_set_connection(true, true);
    if (ax) {
        orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3);
        orb_fonts_set_bold_text(true);
        orb_ui_set_reduce_transparency(true);
    }
    s_pointer = lv_indev_create();
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    s_shell = orb_shell_create(lv_screen_active(), action, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .usb = true};
    strcpy(status.clock_time, "10:09"); strcpy(status.ssid, "Demo Home");
    orb_shell_update(s_shell, &status);
    orb_shell_navigate(s_shell, ORB_SHELL_CONTROL_CENTER); advance(1500);
    save(argv[1], "control-center");
    lv_obj_t *slider = arc(orb_shell_root(s_shell)); CHECK(slider);
    CHECK(lv_arc_get_mode(slider) == LV_ARC_MODE_REVERSE); /* fills from the left end */
    CHECK(lv_arc_get_value(slider) == 70);
    /* The shared kit uses a real 30px child knob, not LVGL's ad-hoc knob part. */
    lv_obj_t *knob = lv_obj_get_child(slider, 0); CHECK(knob);
    CHECK(lv_obj_get_width(knob) == ORB_KIT_KNOB);
    CHECK(lv_obj_get_style_bg_opa(knob, 0) == LV_OPA_COVER);

    /* Several writes before one frame must produce one latest-value action. */
    lv_display_add_event_cb(s_display, frame, LV_EVENT_REFR_READY, NULL);
    unsigned before = s_brightness_actions;
    for (unsigned i = 0; i < 5; i++) CHECK(orb_shell_twist(s_shell, 1));
    CHECK(orb_shell_brightness(s_shell) == 75);
    CHECK(s_brightness_actions == before);
    advance(40);
    CHECK(s_brightness_actions == before + 1 && s_applied == 75);

    before = s_brightness_actions; unsigned frames = s_frames;
    /* Left is dim, right is bright (the slider reads as horizontal). */
    hold(213, 123, true); /* absolute 20%, rather than relative 75% */
    CHECK(abs(orb_shell_brightness(s_shell) - 20) <= 1);
    CHECK(s_brightness_actions > before && s_applied == orb_shell_brightness(s_shell));
    s_tracking_direction = 1;
    glide(213, 123, 57, 450); /* towards the right -> 80% */
    s_tracking_direction = 0;
    CHECK(abs(orb_shell_brightness(s_shell) - 80) <= 1);
    /* Real telemetry arrives while the finger owns the slider. Battery and
     * an older brightness receipt must not replace the active control or
     * rebuild its raster sources, nor pull its preview backwards. */
    orb_kit_value_info_t value_before, value_after;
    CHECK(orb_kit_value_info(slider, &value_before));
    lv_obj_raster_stats_t raster_before, raster_after;
    lv_obj_raster_get_stats(&raster_before);
    status.battery_pct = 9; status.brightness = 9;
    orb_shell_update(s_shell, &status); advance(64);
    CHECK(arc(orb_shell_root(s_shell)) == slider);
    CHECK(abs(orb_shell_brightness(s_shell) - 80) <= 1);
    CHECK(orb_kit_value_info(slider, &value_after));
    CHECK(value_after.width == value_before.width);
    CHECK(value_after.tabular && value_after.template_measures == value_before.template_measures);
    lv_obj_raster_get_stats(&raster_after);
    CHECK(raster_before.builds == raster_after.builds);
    CHECK(!arc(lv_display_get_layer_top(s_display)));
    save(argv[1], "brightness-clockwise");
    advance(150); /* stationary pause before release */
    hold(213, 57, false);
    CHECK(abs(orb_shell_brightness(s_shell) - 80) <= 1);
    CHECK(s_brightness_actions - before <= s_frames - frames);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);

    hold(203, 57, true); /* near stroke, inside its minimum 46px target */
    s_tracking_direction = -1;
    glide(213, 57, 112, 300); /* towards the left -> 30% */
    s_tracking_direction = 0;
    hold(213, 112, false);
    CHECK(abs(orb_shell_brightness(s_shell) - 30) <= 1);
    hold(213, 145, true); CHECK(orb_shell_brightness(s_shell) == 0);
    hold(213, 145, false);
    hold(213, 35, true); CHECK(orb_shell_brightness(s_shell) == 100);
    hold(213, 35, false);
    save(argv[1], "brightness-endpoint");

    for (unsigned i = 0; i < 50; i++) CHECK(orb_shell_twist(s_shell, -1));
    advance(40);
    CHECK(orb_shell_brightness(s_shell) == 50);
    hold(216, 0, true); /* rim outside visible arc remains relative */
    CHECK(orb_shell_brightness(s_shell) == 50);
    glide(216, 0, 36, 300); hold(216, 36, false); advance(500);
    CHECK(orb_shell_brightness(s_shell) > 50);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);

    /* The other selector edits volume through the identical visible arc.
     * All digit-count boundaries keep the lane fixed and never summon a HUD. */
    int retained_brightness = orb_shell_brightness(s_shell);
    before = s_brightness_actions;
    s_input.point = (lv_point_t){323, 326}; s_input.state = LV_INDEV_STATE_PRESSED;
    advance(input_period() + 20);
    s_input.state = LV_INDEV_STATE_RELEASED; advance(1200);
    slider = arc(orb_shell_root(s_shell)); CHECK(slider);
    CHECK(orb_kit_value_info(slider, &value_before));
    lv_obj_raster_get_stats(&raster_before);
    const int volume_states[] = {0, 9, 10, 99, 100};
    for (unsigned i = 0; i < sizeof(volume_states) / sizeof(*volume_states); ++i) {
        int value = volume_states[i];
        float angle = 145.f - 110.f * value / 100.f;
        hold(213, angle, true); hold(213, angle, false);
        CHECK(orb_sound_volume() == (unsigned)value);
        CHECK(orb_shell_brightness(s_shell) == retained_brightness);
        CHECK(!arc(lv_display_get_layer_top(s_display)));
        CHECK(orb_kit_value_info(slider, &value_after));
        CHECK(value_after.width == value_before.width && value_after.tabular);
        CHECK(value_after.template_measures == value_before.template_measures);
        char state[40]; snprintf(state, sizeof(state), "control-volume-%d", value);
        save(argv[1], state);
    }
    lv_obj_raster_get_stats(&raster_after);
    CHECK(raster_before.builds == raster_after.builds && s_brightness_actions == before);
    float nine_percent = 145.f - 110.f * .09f;
    hold(213, nine_percent, true); hold(213, nine_percent, false); advance(320);
    CHECK(orb_sound_volume() == 9);
    hold(213, 35, true); hold(213, 35, false); advance(320);
    CHECK(orb_sound_volume() == 100);
    framebuffer_consistent(argv[1], "control-volume-9-to-100");

    /* Design 2.0: a volume notification is display-only. Its visible ring
     * cannot claim either the direct arc or the generic rim owner. */
    /* A visible control already explains the change. Exercise an unseen
     * hub/media change from the Face, where a HUD is actually warranted. */
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(1200);
    orb_sound_set_volume(60); orb_shell_show_volume(s_shell); advance(320);
    save(argv[1], "volume");
    lv_obj_t *hud_arc = arc(lv_display_get_layer_top(s_display)); CHECK(hud_arc);
    CHECK(!lv_obj_is_clickable(hud_arc));
    hold(213, 145, true);
    glide(213, 145, 35, 600); hold(213, 35, false);
    CHECK(orb_sound_volume() == 60);
    advance(1600); CHECK(!arc(lv_display_get_layer_top(s_display)));

    /* Sound's own Volume destination reuses the same 145°→35° level control
     * as Control Center. The former 270° page is intentionally retired. */
    orb_shell_navigate(s_shell, ORB_SHELL_SOUND); advance(1500);
    click_text("Volume");
    slider = arc(orb_shell_root(s_shell)); CHECK(slider);
    CHECK(lv_obj_is_clickable(slider));
    CHECK(lv_obj_get_width(lv_obj_get_child(slider, 0)) == ORB_KIT_KNOB);
    hold(213, 123, true); CHECK(abs((int)orb_sound_volume() - 20) <= 1);
    glide(213, 123, 57, 600); advance(80);
    CHECK(abs((int)orb_sound_volume() - 80) <= 1);
    CHECK(abs(orb_kit_rim_slider_get_value(slider) - 80) <= 1);
    CHECK(!arc(lv_display_get_layer_top(s_display)));
    save(argv[1], "volume-clockwise");
    advance(1700); CHECK(arc(orb_shell_root(s_shell)) == slider);
    glide(213, 57, 90, 300); hold(213, 90, false); advance(60);
    CHECK(abs((int)orb_sound_volume() - 50) <= 1);
    hold(213, 35, true); CHECK(orb_sound_volume() == 100);
    hold(213, 35, false);
    CHECK(!arc(lv_display_get_layer_top(s_display)));
    save(argv[1], "volume-endpoint");
    orb_shell_back(s_shell); advance(1200);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SOUND);
    CHECK(!arc(lv_display_get_layer_top(s_display))); /* no delayed local echo */

    /* Real firmware enables the power controller before opening Settings.
     * Start that lifecycle in a fresh fixture rather than turning it on
     * inside a synthetic row CLICKED after the long earlier gesture trace. */
    orb_shell_destroy(s_shell);
    s_shell = orb_shell_create(lv_screen_active(), action, NULL); CHECK(s_shell);
    orb_shell_update(s_shell, &status);
    orb_shell_power_enable(s_shell, true);
    /* Startup is deliberately redacted until a manual wake. */
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted);
    advance(800);
    orb_shell_navigate(s_shell, ORB_SHELL_DISPLAY); advance(1200);
    lv_obj_t *display_row_label = text(orb_shell_root(s_shell), "Brightness"); CHECK(display_row_label);
    lv_obj_t *display_row = lv_obj_get_parent(display_row_label);
    lv_obj_raster_get_stats(&raster_before);
    status.brightness = 61; status.battery_pct = 81;
    orb_shell_update(s_shell, &status); advance(64);
    CHECK(text(orb_shell_root(s_shell), "Brightness") == display_row_label);
    CHECK(lv_obj_get_parent(display_row_label) == display_row);
    if (!text(orb_shell_root(s_shell), "61%")) {
        fprintf(stderr, "workspace brightness: %d\n", orb_shell_brightness(s_shell));
        dump_values(orb_shell_root(s_shell));
        lv_area_t row_box;
        fprintf(stderr, "row bounds valid=%d", orb_round_list_row_bounds(display_row, &row_box));
        fprintf(stderr, " box=%d,%d,%d,%d\n", row_box.x1, row_box.y1, row_box.x2, row_box.y2);
        for (unsigned i = 0; i < lv_obj_get_child_count(display_row); ++i) {
            lv_obj_t *child = lv_obj_get_child(display_row, i);
            lv_area_t projected = {0};
            bool mapped = orb_round_list_child_bounds(display_row, child, &projected);
            fprintf(stderr, "row child%u native=%dx%d projected=%d %d,%d,%d,%d text=%s\n", i,
                    (int)lv_obj_get_width(child), (int)lv_obj_get_height(child), mapped,
                    projected.x1, projected.y1, projected.x2, projected.y2,
                    lv_obj_check_type(child, &lv_label_class) ? lv_label_get_text(child) : "<disc>");
        }
    }
    CHECK(text(orb_shell_root(s_shell), "61%"));
    lv_obj_raster_get_stats(&raster_after);
    CHECK(raster_before.builds == raster_after.builds);
    click_text("Brightness");
    slider = arc(orb_shell_root(s_shell)); CHECK(slider && lv_obj_is_clickable(slider));
    CHECK(lv_arc_get_mode(slider) == LV_ARC_MODE_REVERSE);
    hold(213, 123, true); CHECK(abs(orb_shell_brightness(s_shell) - 20) <= 1);
    glide(213, 123, 57, 450); hold(213, 57, false);
    CHECK(abs(orb_shell_brightness(s_shell) - 80) <= 1);
    save(argv[1], "display-brightness");
    orb_shell_back(s_shell); advance(1200);
    click_text("Always On brightness");
    slider = arc(orb_shell_root(s_shell)); CHECK(slider && lv_obj_is_clickable(slider));
    lv_obj_invalidate(orb_shell_root(s_shell)); lv_refr_now(s_display);
    orb_power_snapshot_t presented = orb_shell_power_snapshot(s_shell);
    orb_shell_power_display_ready(s_shell, presented.revision, true);
    save(argv[1], "always-on-brightness-before");
    /* Stepped AOD uses a shorter domain than 0–100, but its reservation must
     * use the same final hero font as the displayed percentages. */
    CHECK(orb_kit_value_info(slider, &value_before));
    lv_obj_raster_get_stats(&raster_before);
    for (unsigned level = 1; level <= 15; ++level) {
        float angle = 145.f - 110.f * (level - 1) / 14.f;
        hold(213, angle, true); hold(213, angle, false);
        CHECK(orb_shell_power_preferences(s_shell).always_on_brightness_pct == level);
        char caption[16]; snprintf(caption, sizeof(caption), "%u%%", level);
        check_level_text_fit(slider, caption);
        CHECK(orb_kit_value_info(slider, &value_after));
        CHECK(value_after.width == value_before.width);
        CHECK(value_after.template_measures == value_before.template_measures);
    }
    lv_obj_raster_get_stats(&raster_after);
    CHECK(raster_before.builds == raster_after.builds);
    float nine_aod = 145.f - 110.f * 8.f / 14.f;
    hold(213, nine_aod, true); hold(213, nine_aod, false); advance(320);
    CHECK(orb_shell_power_preferences(s_shell).always_on_brightness_pct == 9);
    hold(213, 35, true); hold(213, 35, false); advance(320);
    CHECK(orb_shell_power_preferences(s_shell).always_on_brightness_pct == 15);
    check_level_text_fit(slider, "15%");
    framebuffer_consistent(argv[1], "always-on-brightness-9-to-15");
    save(argv[1], "always-on-brightness");
    CHECK(!arc(lv_display_get_layer_top(s_display)));

    orb_shell_destroy(s_shell); lv_indev_delete(s_pointer); lv_deinit();
    printf("gesture controls: %u checks passed, LVGL scheduled input, %s\n", s_checks, ax ? "AX3 Bold RT" : "Default");
    return 0;
}
