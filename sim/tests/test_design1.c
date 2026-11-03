/* Design-frame regressions against the real retained shell, not a mock view.
 * Fixtures are process-local and every input remains within this fake display. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_fixture.h"
#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "sim_display.h"

static unsigned s_checks;
static uint32_t s_now;
static const char *s_scene;
static lv_display_t *s_display;
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "Design 1.0 %s at %u ms: %s:%d: %s\n", s_scene, s_now, __FILE__, __LINE__, #value); exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev); *data = (lv_indev_data_t){.state = LV_INDEV_STATE_RELEASED};
}
static void settle(void)
{
    for (unsigned i = 0; i < 70; ++i) {
        s_now += 16;
        emu_fixture_step(s_now, 0, true, 0, 0, 1);
        orb_shell_power_advance(emu_fixture_shell(), s_now);
        lv_timer_handler(); lv_refr_now(s_display);
    }
}
static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (orb_kit_rim_text_get(root) &&
        !strcasecmp(orb_kit_rim_text_get(root), text)) return root;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *find_part(lv_obj_t *root, const char *text)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && strstr(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find_part(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *button_at(lv_obj_t *root, int x, int y, int width, int height)
{
    if (!lv_obj_is_visible(root)) return NULL;
    lv_area_t a; lv_obj_get_coords(root, &a);
    if (lv_obj_is_clickable(root) && !lv_obj_is_scrollable(root) && abs(a.x1 - x) <= 2 && abs(a.y1 - y) <= 2 &&
        abs(lv_obj_get_width(root) - width) <= 2 && abs(lv_obj_get_height(root) - height) <= 2) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = button_at(lv_obj_get_child(root, i), x, y, width, height);
        if (found) return found;
    }
    return NULL;
}
static unsigned labels(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return 0;
    unsigned result = lv_obj_check_type(root, &lv_label_class);
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) result += labels(lv_obj_get_child(root, i));
    return result;
}
static void stepped_rows(void)
{
    lv_obj_t *title = find(lv_screen_active(), "Sound"); CHECK(title);
    lv_obj_t *row = lv_obj_get_parent(title), *list = lv_obj_get_parent(row);
    lv_obj_t *disc = lv_obj_get_child(row, 0);
    const lv_font_t *font = lv_obj_get_style_text_font(title, 0);
    const int nominal_width = lv_obj_get_width(disc), nominal_height = lv_obj_get_height(disc);
    unsigned native = 0, rim = 0;
    orb_ui_scroll_diagnostics_begin();
    for (int y = 0; y <= 180; ++y) {
        lv_obj_scroll_to_y(list, y, LV_ANIM_OFF);
        orb_round_list_update(list);
        CHECK(lv_obj_get_style_text_font(title, 0) == font);
        lv_area_t presented;
        if (!orb_round_list_child_bounds(row, disc, &presented)) continue;
        const int width = lv_area_get_width(&presented), height = lv_area_get_height(&presented);
        bool full = width == nominal_width && height == nominal_height;
        bool small = width == nominal_width * 211 / 256 && height == nominal_height * 211 / 256;
        CHECK(full || small); /* Two cached sizes, never per-pixel perspective. */
        native += full; rim += small;
    }
    orb_ui_scroll_diagnostics_t diagnostics;
    orb_ui_scroll_diagnostics_snapshot(&diagnostics); orb_ui_scroll_diagnostics_end();
    CHECK(native > 0 && rim > 0);
    CHECK(diagnostics.style_writes == 0 && diagnostics.font_changes == 0);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}

static void scene(const char *name)
{
    s_scene = name;
    CHECK(emu_fixture_design_scene(name));
    settle();
    CHECK(labels(lv_screen_active()) > 0 || !strcmp(name, "AlwaysOn"));
}
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    const bool ax = !strcmp(mode, "ax3-bold-rt");
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create();
    s_scene = "initialization";
    CHECK(s_display && emu_fixture_init(s_display));
    lv_indev_t *pointer = lv_indev_create(); CHECK(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER); lv_indev_set_display(pointer, s_display);
    lv_indev_set_read_cb(pointer, read_pointer);
    if (ax) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(true);
    }
    orb_ui_set_reduce_motion(!strcmp(mode, "reduce-motion"));
    scene("Face");
    CHECK(find(lv_screen_active(), "10:09"));
    CHECK(find(lv_screen_active(), "10")); /* Compact Home count inside its80px disc. */
    CHECK(find(lv_screen_active(), ORB_ICON_BATTERY_HIGH));
    /* FaceOrb 2.0 uses three standard complications and one mini utility. */
    CHECK(button_at(lv_screen_active(), 205, 380, 56, 56));
    CHECK(!find(lv_screen_active(), "Apps"));
    scene("FaceWaiting");
    CHECK(!find(lv_screen_active(), "2 waiting") && !find(lv_screen_active(), ORB_ICON_HAND " 2 waiting"));
    CHECK(find(lv_screen_active(), "Tuesday"));
    scene("Apps");
    /* Design 2.0 keeps all seven 80px discs at the exported centres in
     * every type size. Focus adds only an outline, never a size change. */
    static const int positions[7][2] = {
        {193,93},{280,143},{106,143},{193,193},{280,243},{106,243},{193,293},
    };
    for (unsigned i = 0; i < 7; ++i)
        CHECK(button_at(lv_screen_active(), positions[i][0], positions[i][1], 80, 80));
    CHECK(find(lv_screen_active(), "Agents") || find(lv_screen_active(), "Home") ||
          find(lv_screen_active(), "Time"));
    scene("Offline");
    CHECK(find(lv_screen_active(), "10:09") && find(lv_screen_active(), "—"));
    CHECK(!find(lv_screen_active(), "2 waiting"));
    scene("Charging");
    CHECK(find(lv_screen_active(), "47%") ||
          (find(lv_screen_active(), "47") && find(lv_screen_active(), "%")));
    /* The PMU supplies charge/USB, no fabricated time-to-full estimate. */
    CHECK(!find(lv_screen_active(), "Full in about 1 h 10 min"));
    scene("Settings"); CHECK(find(lv_screen_active(), "Settings"));
    lv_obj_t *sounds = find(lv_screen_active(), "Sound"); CHECK(sounds);
    lv_obj_t *sounds_row = lv_obj_get_parent(sounds);
    /* Dynamic values are live list siblings outside the prepared row. */
    lv_obj_t *sound_value = find(lv_obj_get_parent(sounds_row), "On"); CHECK(sound_value);
    lv_area_t row_bounds, value_bounds;
    CHECK(orb_round_list_row_bounds(sounds_row, &row_bounds));
    lv_obj_get_coords(sound_value, &value_bounds);
    CHECK(abs(value_bounds.y1 + value_bounds.y2 - row_bounds.y1 - row_bounds.y2) <= 1);
    CHECK(value_bounds.x2 <= row_bounds.x2);
    CHECK(lv_obj_get_width(lv_obj_get_child(sounds_row, 0)) == ORB_KIT_DISC_ROW);
    CHECK(lv_obj_get_style_text_align(sound_value, 0) == LV_TEXT_ALIGN_RIGHT);
    stepped_rows();
    scene("Notifications");
    lv_obj_t *notice = find_part(lv_screen_active(), "Build"); CHECK(notice);
    lv_obj_t *notice_source = find(lv_obj_get_parent(notice), "Claude"); CHECK(notice_source);
    lv_area_t notice_bounds, source_bounds;
    lv_obj_get_coords(notice, &notice_bounds); lv_obj_get_coords(notice_source, &source_bounds);
    /* Kit rows align actual glyph ink; Inter line boxes include blank
     * ascent/leading and can intersect without intersecting visible text. */
    CHECK(notice_bounds.y1 < source_bounds.y1);
    /* 72 px is the kit's minimum; AX3 rows grow to wrap their titles. */
    CHECK(lv_obj_get_height(lv_obj_get_parent(notice)) == ORB_KIT_ROW_H ||
          (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 && lv_obj_get_height(lv_obj_get_parent(notice)) > ORB_KIT_ROW_H));
    scene("NotificationDetail");
    /* Long-look actions follow the complete message in its scroll owner. */
    orb_shell_twist(emu_fixture_shell(), 8); settle();
    CHECK(find(lv_screen_active(), "Open session"));
    CHECK(find(lv_screen_active(), "Dismiss"));
    scene("Stack"); CHECK(find(lv_screen_active(), "Living Room"));
    CHECK(!find(lv_screen_active(), "Stack"));
    scene("ControlCenter");
    {
        const char *toggles[] = {ORB_ICON_BELL_SLASH, ORB_ICON_MOON, ORB_ICON_FLASHLIGHT,
                                ORB_ICON_LOCK, ORB_ICON_MIC, ORB_ICON_GEAR};
        for (unsigned i = 0; i < sizeof(toggles) / sizeof(toggles[0]); i++) {
            lv_obj_t *glyph = find(lv_screen_active(), toggles[i]);
            if (!glyph && i == 0) glyph = find(lv_screen_active(), ORB_ICON_BELL);
            CHECK(glyph);
            CHECK(lv_obj_get_width(lv_obj_get_parent(glyph)) == 80);
            CHECK(lv_obj_get_height(lv_obj_get_parent(glyph)) == 80);
        }
        CHECK(!find(lv_screen_active(), ORB_ICON_MINUS));
        for (unsigned i = 0; i < 50; i++) CHECK(orb_shell_twist(emu_fixture_shell(), -1));
        settle();
        CHECK(orb_shell_brightness(emu_fixture_shell()) == 20);
        lv_obj_t *light = find(lv_screen_active(), ORB_ICON_FLASHLIGHT); CHECK(light);
        lv_obj_send_event(lv_obj_get_parent(light), LV_EVENT_CLICKED, NULL); settle();
        CHECK(orb_shell_brightness(emu_fixture_shell()) == 100);
        light = find(lv_screen_active(), ORB_ICON_FLASHLIGHT); CHECK(light);
        lv_obj_send_event(lv_obj_get_parent(light), LV_EVENT_CLICKED, NULL); settle();
        CHECK(orb_shell_brightness(emu_fixture_shell()) == 20);
    }
    scene("Compass"); CHECK(find(lv_screen_active(), "Settings") && find(lv_screen_active(), "Home"));
    scene("PowerSheet");
    CHECK(find(lv_display_get_layer_top(s_display), "Sleep display") &&
          find(lv_display_get_layer_top(s_display), "Restart"));
    const char *power_labels[] = {"Sleep display", "Restart"};
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *caption = find(lv_display_get_layer_top(s_display), power_labels[i]);
        lv_area_t caption_bounds, control_bounds;
        lv_obj_get_coords(caption, &caption_bounds);
        lv_obj_get_coords(lv_obj_get_parent(caption), &control_bounds);
        CHECK(caption_bounds.x1 >= control_bounds.x1 && caption_bounds.x2 <= control_bounds.x2);
        CHECK(caption_bounds.y1 >= control_bounds.y1 && caption_bounds.y2 <= control_bounds.y2);
    }
    scene("Setup"); CHECK(find(lv_screen_active(), "482 915"));
    CHECK(find(lv_screen_active(), "Pair with your Mac"));
    scene("Updating"); CHECK(find(lv_screen_active(), "64%"));
    /* AX3 keeps the full reassurance in the bounded reader below the hero. */
    orb_shell_twist(emu_fixture_shell(), 8); settle();
    CHECK(find(lv_screen_active(), "Downloading 0.4.1")); /* The stage line carries the version (polish #22). */
    scene("AlwaysOn");
    orb_power_snapshot_t power = orb_shell_power_snapshot(emu_fixture_shell());
    CHECK(power.state == ORB_POWER_ALWAYS_ON && power.redact);
    lv_obj_t *top = lv_display_get_layer_top(s_display);
    CHECK(find(top, "10:09")); CHECK(find(top, ORB_ICON_LOCK));
    CHECK(!find(top, "Open session") && !find(top, "80%") && !find(top, "Living Room"));
    if (!strcmp(mode, "increase-contrast")) {
        orb_shell_power_set_contrast(emu_fixture_shell(), true); settle();
        CHECK(find(top, ORB_ICON_LOCK));
    }
    orb_model_snapshot_t before, after;
    CHECK(orb_model_snapshot(&before));
    const orb_button_event_t down = {.button = ORB_BUTTON_PWR, .gesture = ORB_BUTTON_DOWN};
    const orb_button_event_t short_press = {.button = ORB_BUTTON_PWR, .gesture = ORB_BUTTON_SHORT};
    orb_shell_button(emu_fixture_shell(), &down);
    orb_shell_button(emu_fixture_shell(), &short_press);
    settle();
    CHECK(orb_model_snapshot(&after));
    CHECK(after.card_count == before.card_count && orb_model_pending_answers() == 0);
    CHECK(orb_shell_surface(emu_fixture_shell()) == ORB_SHELL_FACE);
    scene("AlwaysOnEmpty");
    top = lv_display_get_layer_top(s_display);
    CHECK(find(top, "10:09") && !find(top, "2 waiting") && !find(top, "0 waiting"));
    scene("AlwaysOnShifted");
    power = orb_shell_power_snapshot(emu_fixture_shell());
    CHECK(power.state == ORB_POWER_ALWAYS_ON && power.shift_x == 1 && power.shift_y == -1);
    scene("Sleep");
    power = orb_shell_power_snapshot(emu_fixture_shell());
    CHECK(power.state == ORB_POWER_SLEEP && !power.panel_on && power.redact);
    static uint8_t dark[SIM_D * SIM_D * 3];
    sim_display_rgb(dark);
    uint8_t maximum = 0;
    for (unsigned i = 0; i < sizeof(dark); ++i) maximum |= dark[i];
    CHECK(maximum == 0);
    scene("Wake");
    power = orb_shell_power_snapshot(emu_fixture_shell());
    CHECK(power.state == ORB_POWER_ACTIVE && !power.redact);
    CHECK(orb_shell_surface(emu_fixture_shell()) == ORB_SHELL_FACE);
    emu_fixture_shutdown(); lv_indev_delete(pointer);
    printf("Design 1.0 %s: %u geometry/privacy checks passed\n", mode, s_checks);
    return 0;
}
