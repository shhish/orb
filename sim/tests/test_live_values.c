/* Live values are retained geometry, not new raster sources per input tick. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "sim_display.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_text_private.h"

static unsigned s_checks, s_detents;
static uint32_t s_now;
static lv_indev_data_t s_input;
static lv_display_t *s_display;
static lv_obj_t *s_lease_label;
static bool s_lease_checked;
extern size_t sim_memory_allocation_calls(void);
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "live values:%d (%d/%d): %s\n", __LINE__, \
        orb_fonts_content_size(), orb_fonts_bold_text(), #value); exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *pointer, lv_indev_data_t *out) { *out = s_input; }
static void advance(unsigned milliseconds)
{
    for (unsigned ms = 0; ms < milliseconds; ms += 4) {
        s_now += 4;
        lv_timer_handler();
    }
}
static lv_obj_t *label_with_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label_with_text(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
static void tabular(lv_obj_t *label)
{
    CHECK(label);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    uint16_t digit = lv_font_get_glyph_width(font, '0', 0);
    for (int ch = '1'; ch <= '9'; ch++) CHECK(lv_font_get_glyph_width(font, ch, 0) == digit);
}
static void fits(lv_obj_t *label)
{
    const char *text = lv_label_get_text(label);
    lv_text_attributes_t attributes = {
        .letter_space = lv_obj_get_style_text_letter_space(label, 0),
        .text_flags = LV_TEXT_FLAG_EXPAND,
    };
    CHECK(lv_text_get_width(text, strlen(text), lv_obj_get_style_text_font(label, 0), &attributes)
          <= lv_obj_get_width(label));
}
static void detent(lv_obj_t *slider, int32_t value, void *data) { ++s_detents; }
static void queued_value(lv_event_t *event)
{
    lv_draw_label_dsc_t *descriptor = lv_draw_task_get_label_dsc(lv_event_get_draw_task(event));
    if (!descriptor || s_lease_checked) return;
    s_lease_checked = true;
    CHECK(descriptor->text_local && !descriptor->text_static);
    CHECK(!strcmp(descriptor->text, "9%"));
    /* A queued worker owns its 9% snapshot even when the UI producer advances
     * the same retained lane to 100% before the task can dispatch. */
    orb_kit_value_set(s_lease_label, "100%", NULL);
    CHECK(!strcmp(lv_label_get_text(s_lease_label), "100%"));
    CHECK(!strcmp(descriptor->text, "9%"));
}
static void draw_lease(lv_obj_t *root)
{
    s_lease_label = lv_label_create(root);
    orb_text_apply(s_lease_label, ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED, 0);
    orb_kit_value_set(s_lease_label, "9%", "100%");
    lv_obj_center(s_lease_label);
    s_lease_checked = false;
    lv_obj_set_send_draw_task_events(s_lease_label, true);
    lv_obj_add_event_cb(s_lease_label, queued_value, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_refr_now(s_display);
    CHECK(s_lease_checked);
    lv_obj_delete(s_lease_label); s_lease_label = NULL;
}
static unsigned primaries(lv_obj_t *root)
{
    unsigned count = orb_kit_is_primary(root);
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) count += primaries(lv_obj_get_child(root, i));
    return count;
}
static unsigned inset_title(lv_obj_t *root)
{
    orb_kit_rim_text_info_t info;
    if (orb_kit_rim_text_info(root, &info)) {
        CHECK(info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES);
        for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
            lv_obj_t *slice = lv_obj_get_child(root, i);
            lv_obj_t *arc = lv_obj_get_child(slice, 0);
            CHECK(arc && lv_obj_check_type(arc, &lv_arclabel_class));
            CHECK(lv_arclabel_get_radius(arc) == ORB_KIT_TITLE_RADIUS); /* bottom-arc levels share the rim baseline */
        }
        return 1;
    }
    unsigned found = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) found += inset_title(lv_obj_get_child(root, i));
    return found;
}

static void buttons(lv_obj_t *root)
{
    lv_obj_t *button = orb_kit_button_secondary_create(root, "9%");
    orb_kit_chip_set_value(button, "9%", "100%");
    lv_obj_update_layout(root);
    advance(500);
    orb_kit_value_info_t initial;
    CHECK(orb_kit_value_info(button, &initial) && initial.tabular);
    lv_obj_t *label = label_with_text(button, "9%"); tabular(label);
    int32_t width = lv_obj_get_width(button), label_width = lv_obj_get_width(label);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    for (unsigned value = 0; value <= 100; ++value) {
        char text[8]; snprintf(text, sizeof(text), "%u%%", value);
        orb_kit_chip_set_value(button, text, "100%");
        advance(16);
        label = label_with_text(button, text); CHECK(label); fits(label);
        CHECK(lv_obj_get_width(button) == width && lv_obj_get_width(label) == label_width);
        orb_kit_value_info_t current;
        CHECK(orb_kit_value_info(button, &current));
        CHECK(current.template_measures == initial.template_measures);
        CHECK(current.raster_prepares == initial.raster_prepares && current.live);
    }
    lv_obj_raster_get_stats(&after);
    CHECK(before.builds == after.builds);
    advance(500);
    orb_kit_value_info_t settled;
    CHECK(orb_kit_value_info(button, &settled));
    CHECK(settled.raster_prepares <= initial.raster_prepares + 1);
    orb_kit_button_set_value(button, "1000%", "1000%");
    CHECK(orb_kit_value_info(button, &settled));
    CHECK(settled.template_measures == initial.template_measures + 1);
    lv_obj_delete(button);
}

static void clocks(lv_obj_t *root)
{
    const char *fixtures[] = {"00:00", "11:11", "59:59", "88:88"};
    lv_obj_t *label = lv_label_create(root);
    orb_text_apply(label, ORB_TEXT_TITLE1, ORB_TEXT_EMPHASIZED, 0);
    orb_kit_value_set(label, fixtures[0], "88:88");
    lv_obj_update_layout(root);
    int32_t width = lv_obj_get_width(label);
    tabular(label);
    for (unsigned i = 0; i < sizeof(fixtures) / sizeof(*fixtures); ++i) {
        orb_kit_value_set(label, fixtures[i], "88:88");
        lv_obj_update_layout(root);
        CHECK(lv_obj_get_width(label) == width);
        fits(label);
    }
    lv_obj_delete(label);
}

static void legacy_ellipsis(lv_obj_t *root)
{
    lv_obj_t *label = lv_label_create(root);
    orb_text_apply(label, ORB_TEXT_FOOTNOTE2, 0, 0);
    lv_obj_set_size(label, 64,
                    lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)));
    lv_label_set_text(label, "Living room thermostat temperature");
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    advance(40);
    CHECK(strstr(lv_label_get_text(label), "\xe2\x80\xa6")); /* One U+2026, never three periods. */

    orb_kit_value_set(label, "100%", "100%");
    advance(40);
    CHECK(!strcmp(lv_label_get_text(label), "100%"));
    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_CLIP);
    tabular(label); fits(label);
    int32_t reserved_width = lv_obj_get_width(label);

    /* A legacy layout helper can put a registered lane back into DOTS and
     * narrow it. Force real ellipsis replacement bytes, then restore its
     * width and write before LVGL's queued text refresh can retire them. */
    lv_obj_set_width(label, LV_MAX(12, reserved_width / 3));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    advance(40);
    CHECK(strstr(lv_label_get_text(label), "\xe2\x80\xa6")); /* One U+2026, never three periods. */
    lv_obj_set_width(label, reserved_width);
    orb_kit_value_set(label, "9%", NULL);
    advance(40);
    CHECK(!strcmp(lv_label_get_text(label), "9%"));
    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_CLIP);
    tabular(label); fits(label);

    orb_kit_value_info_t initial, current;
    CHECK(orb_kit_value_info(label, &initial));
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    for (unsigned percent = 0; percent <= 100; ++percent) {
        char text[8]; snprintf(text, sizeof(text), "%u%%", percent);
        size_t allocations = sim_memory_allocation_calls();
        orb_kit_value_set(label, text, NULL);
        CHECK(sim_memory_allocation_calls() == allocations);
        advance(16);
        CHECK(!strcmp(lv_label_get_text(label), text));
        CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_CLIP);
        CHECK(lv_obj_get_width(label) == reserved_width);
        CHECK(orb_kit_value_info(label, &current));
        CHECK(current.template_measures == initial.template_measures);
        tabular(label); fits(label);
    }
    lv_obj_raster_get_stats(&after);
    CHECK(before.builds == after.builds);
    lv_obj_delete(label);
}

static void selector_chips(lv_obj_t *root)
{
    const char *icons[] = {ORB_ICON_SUN, ORB_ICON_SPEAKER_LOW};
    const int centres[] = {143, 323};
    lv_obj_t *chips[2];
    lv_area_t chip_bounds[2];
    int32_t ink_gap[2];
    for (unsigned i = 0; i < 2; ++i) {
        char text[32], reservation[32];
        snprintf(text, sizeof(text), "%s 9%%", icons[i]);
        snprintf(reservation, sizeof(reservation), "%s 100%%", icons[i]);
        chips[i] = orb_kit_button_secondary_create(root, text);
        orb_kit_disc_set_text_style(chips[i], ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED);
        orb_kit_chip_set_value(chips[i], text, reservation);
        lv_obj_align(chips[i], LV_ALIGN_TOP_MID, centres[i] - 233, 300);
        lv_obj_update_layout(root);
        lv_obj_t *icon = label_with_text(chips[i], icons[i]); CHECK(icon);
        lv_obj_t *number = label_with_text(chips[i], "9%"); CHECK(number);
        tabular(number);
        lv_area_t icon_before, number_before;
        lv_obj_get_coords(icon, &icon_before); lv_obj_get_coords(number, &number_before);
        lv_obj_get_coords(chips[i], &chip_bounds[i]);
        uint32_t offset = 0;
        lv_font_glyph_dsc_t glyph;
        CHECK(lv_font_get_glyph_dsc(lv_obj_get_style_text_font(icon, 0), &glyph,
            lv_text_encoded_next(icons[i], &offset), 0));
        int32_t icon_right = icon_before.x1 + glyph.ofs_x + glyph.box_w;
        int32_t icon_bearing = glyph.ofs_x;
        CHECK(icon_before.x1 + icon_bearing >= chip_bounds[i].x1 + 16);
        CHECK(number_before.x1 - icon_right == 8);
        CHECK(number_before.x2 <= chip_bounds[i].x2 - 16);
        CHECK(icon_before.y1 >= chip_bounds[i].y1 && icon_before.y2 <= chip_bounds[i].y2);
        CHECK(number_before.y1 >= chip_bounds[i].y1 && number_before.y2 <= chip_bounds[i].y2);
        orb_kit_value_info_t icon_metrics, number_metrics, current;
        CHECK(orb_kit_value_info(icon, &icon_metrics) && orb_kit_value_info(number, &number_metrics));
        const unsigned values[] = {10, 100, 9};
        for (unsigned index = 0; index < sizeof(values) / sizeof(*values); ++index) {
            snprintf(text, sizeof(text), "%s %u%%", icons[i], values[index]);
            size_t allocations = sim_memory_allocation_calls();
            orb_kit_chip_set_value(chips[i], text, reservation);
            CHECK(sim_memory_allocation_calls() == allocations);
            CHECK(orb_kit_value_info(icon, &current) && current.template_measures == icon_metrics.template_measures);
            CHECK(orb_kit_value_info(number, &current) && current.template_measures == number_metrics.template_measures);
            lv_obj_update_layout(root);
            lv_area_t icon_after, number_after, button_after;
            lv_obj_get_coords(icon, &icon_after); lv_obj_get_coords(number, &number_after);
            lv_obj_get_coords(chips[i], &button_after);
            CHECK(!memcmp(&icon_before, &icon_after, sizeof(icon_before)));
            CHECK(number_before.x2 == number_after.x2 && number_before.x1 == number_after.x1);
            CHECK(!memcmp(&chip_bounds[i], &button_after, sizeof(button_after)));
            CHECK(number_after.x2 <= button_after.x2 - 16 &&
                icon_after.x1 + icon_bearing >= button_after.x1 + 16);
            CHECK(!strcmp(orb_kit_button_get_text(chips[i]), text));
            fits(number);
            if (values[index] == 100) {
                lv_text_attributes_t attributes = {
                    .letter_space = lv_obj_get_style_text_letter_space(number, 0),
                    .text_flags = LV_TEXT_FLAG_EXPAND,
                };
                const lv_font_t *font = lv_obj_get_style_text_font(number, 0);
                int32_t advance = lv_text_get_width("100%", 4, font, &attributes);
                CHECK(lv_font_get_glyph_dsc(font, &glyph, '1', '0'));
                ink_gap[i] = number_after.x2 + 1 - advance + glyph.ofs_x - icon_right;
            }
        }
    }
    CHECK(ink_gap[0] == ink_gap[1] && ink_gap[0] >= 8);
    CHECK(chip_bounds[1].x1 - chip_bounds[0].x2 - 1 >= 20);
    lv_obj_delete(chips[0]); lv_obj_delete(chips[1]);
}

static void rows(lv_obj_t *root)
{
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 466, 466);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Battery", .value = "0%", .glyph = ORB_ICON_BATTERY_FULL,
        .accent = ORB_KIT_PLAY,
    });
    lv_obj_set_y(row, 197);
    orb_kit_row_set_live_value(row, "0%", "100%");
    CHECK(orb_kit_list_apply(list));
    advance(500);
    lv_obj_t *value = label_with_text(list, "0%"); CHECK(value);
    tabular(value);
    int32_t width = lv_obj_get_width(value);
    orb_round_list_cache_stats_t before, after;
    CHECK(orb_round_list_cache_stats(list, &before));
    lv_obj_raster_stats_t raster_before, raster_after;
    lv_obj_raster_get_stats(&raster_before);
    for (unsigned percent = 0; percent <= 100; ++percent) {
        char text[8]; snprintf(text, sizeof(text), "%u%%", percent);
        size_t allocations = sim_memory_allocation_calls();
        orb_kit_row_set_live_value(row, text, "100%");
        CHECK(sim_memory_allocation_calls() == allocations);
        advance(16);
        CHECK(lv_obj_get_width(value) == width && !strcmp(lv_label_get_text(value), text));
    }
    CHECK(orb_round_list_cache_stats(list, &after));
    lv_obj_raster_get_stats(&raster_after);
    CHECK(before.source_builds == after.source_builds && raster_before.builds == raster_after.builds);
    /* Existing semantic callers may keep using the row setter after a row
     * enters live mode. It must update visible ink, not the cached anchor. */
    orb_kit_row_set_value(row, "9%");
    advance(32);
    CHECK(!strcmp(lv_label_get_text(value), "9%") && lv_obj_get_width(value) == width);
    CHECK(orb_round_list_cache_stats(list, &after));
    lv_obj_raster_get_stats(&raster_after);
    CHECK(before.source_builds == after.source_builds && raster_before.builds == raster_after.builds);
    /* A deliberate typography change may remeasure once; it must retain the
     * current value rather than exposing the reservation's placeholder ink. */
    CHECK(orb_kit_row_set_text_role(row, ORB_KIT_ROW_TEXT_VALUE,
                                  ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR));
    advance(32);
    CHECK(!strcmp(lv_label_get_text(value), "9%"));
    tabular(value); fits(value);
    lv_obj_delete(list);
}

static void level(lv_obj_t *root)
{
    lv_obj_t *slider = orb_kit_level_create(root, ORB_KIT_SOUND, 0, 100, 1, detent, NULL);
    CHECK(slider);
    orb_kit_level_set_icon(slider, ORB_ICON_SPEAKER_HIGH);
    orb_kit_level_set_title(slider, "Display brightness");
    orb_kit_level_set_value(slider, 0, false);
    lv_obj_update_layout(root);
    advance(500);
    CHECK(lv_arc_get_mode(slider) == LV_ARC_MODE_REVERSE);
    CHECK(lv_obj_get_width(lv_obj_get_child(slider, 0)) == ORB_KIT_KNOB);
    CHECK(primaries(slider) <= 1);
    CHECK(inset_title(root) == 1);
    lv_obj_t *icon = label_with_text(slider, ORB_ICON_SPEAKER_HIGH);
    lv_obj_t *value = label_with_text(slider, "0%");
    CHECK(icon && value);
    lv_area_t icon_box, value_box;
    lv_obj_get_coords(icon, &icon_box); lv_obj_get_coords(value, &value_box);
    CHECK(icon_box.y2 + 12 <= value_box.y1);
    orb_kit_value_info_t initial;
    CHECK(orb_kit_value_info(slider, &initial));
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    unsigned detents_before = s_detents;
    int previous = 0;
    for (int sample = 0; sample <= 200; ++sample) {
        int value = sample <= 100 ? sample : 200 - sample;
        float radians = (145.f - 110.f * value / 100.f) * (float)M_PI / 180.f;
        s_input.point = (lv_point_t){(int)lroundf(233 + 213 * cosf(radians)),
                                    (int)lroundf(233 + 213 * sinf(radians))};
        s_input.state = LV_INDEV_STATE_PRESSED;
        advance(32);
        int actual = orb_kit_rim_slider_get_value(slider);
        CHECK(abs(actual - value) <= 1);
        CHECK(sample <= 100 ? actual >= previous : actual <= previous);
        previous = actual;
        orb_kit_value_info_t current;
        CHECK(orb_kit_value_info(slider, &current));
        CHECK(current.width == initial.width && current.template_measures == initial.template_measures);
        CHECK(current.raster_prepares == initial.raster_prepares);
        char text[8]; snprintf(text, sizeof(text), "%d%%", orb_kit_rim_slider_get_value(slider));
        lv_obj_t *label = label_with_text(slider, text);
        if (!label) label = label_with_text(root, text); /* semantic children can be live siblings */
        tabular(label); fits(label);
    }
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds && s_detents > detents_before + 50);
    s_input.state = LV_INDEV_STATE_RELEASED;
    advance(40);
    CHECK(orb_kit_rim_slider_get_value(slider) == 0);
    lv_obj_delete(slider);
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL);
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, s_display);
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_scrollable(root, false);
    for (int profile = 0; profile < 3; ++profile) {
        CHECK(orb_fonts_set_content_size(profile ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        orb_fonts_set_bold_text(profile == 2);
        orb_ui_set_reduce_transparency(profile == 2);
        buttons(root); clocks(root); legacy_ellipsis(root); selector_chips(root);
        rows(root); level(root); draw_lease(root);
    }
    lv_indev_delete(pointer); lv_deinit();
    printf("live values: %u checks, Default/AX3/AX3 Bold RT, zero raster builds per drag\n", s_checks);
    return 0;
}
