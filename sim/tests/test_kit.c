/* Public kit contract tests: device-pixel geometry and contact retirement. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks, s_clicks, s_detents;
static unsigned s_frames;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static uint8_t s_pixels[SIM_D * SIM_D * 3];
extern size_t sim_memory_allocation_calls(void);
static uint32_t tick(void) { return s_now; }
static void rendered(lv_event_t *event) { (void)event; s_frames++; }

static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    *data = s_input;
}

static void require(bool valid, const char *message)
{
    s_checks++;
    if (valid) return;
    fprintf(stderr, "kit contract: %s\n", message);
    exit(EXIT_FAILURE);
}

static void advance(unsigned milliseconds)
{
    for (unsigned i = 0; i < milliseconds; i += 16) {
        s_now += 16;
        lv_timer_handler();
    }
}

static void clicked(lv_event_t *event)
{
    (void)event;
    s_clicks++;
}

static bool press_geometry_at_rest(lv_obj_t *obj)
{
    return lv_obj_get_style_transform_scale_x(obj, 0) == 256 &&
           lv_obj_get_style_transform_width(obj, 0) == 0 &&
           lv_obj_get_style_transform_height(obj, 0) == 0;
}

static void detent(lv_obj_t *obj, int32_t value, void *user_data)
{
    (void)obj; (void)value; (void)user_data;
    s_detents++;
}

static void sizes_and_placement(lv_obj_t *root)
{
    const orb_kit_disc_kind_t kinds[] = {
        ORB_KIT_DISC_STANDARD, ORB_KIT_DISC_KIND_MINI, ORB_KIT_DISC_KIND_ROW, ORB_KIT_DISC_ART,
    };
    const int sizes[] = {80, 56, 40, 180};
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        lv_obj_t *disc = orb_kit_disc_create(root, kinds[i], ORB_KIT_ACCENT_FILL,
                                            ORB_KIT_CODEX, ORB_ICON_GEAR);
        lv_obj_update_layout(disc);
        require(lv_obj_get_width(disc) == sizes[i] && lv_obj_get_height(disc) == sizes[i],
                "disc diameter drift");
        int width = lv_obj_get_width(disc);
        orb_kit_disc_set_focus(disc, true);
        require(lv_obj_get_width(disc) == width, "focus resized disc");
        require(lv_obj_get_style_transform_scale_x(disc, 0) == 256 &&
                lv_obj_get_style_transform_scale_y(disc, 0) == 256,
                "focus scaled the disc presentation without resizing its layout");
        require(lv_obj_get_style_outline_width(disc, 0) == ORB_KIT_FOCUS_WIDTH,
                "focus ring is not 3px");
        require(lv_obj_get_style_outline_pad(disc, 0) == ORB_KIT_FOCUS_OFFSET,
                "focus ring offset is not 4px");
        require(lv_color_eq(lv_obj_get_style_outline_color(disc, 0), lv_color_white()),
                "focus ring is not white");
        orb_kit_disc_set_focus(disc, false);
        require(lv_obj_get_style_outline_opa(disc, 0) == LV_OPA_TRANSP ||
                lv_obj_get_style_outline_width(disc, 0) == 0, "focus ring did not clear");
        lv_obj_delete(disc);
    }
    lv_obj_t *primary = orb_kit_button_primary_create(root, "Start", ORB_KIT_PLAY);
    orb_kit_place_bottom_arc(primary);
    lv_obj_update_layout(primary);
    require(lv_obj_get_height(primary) == ORB_KIT_PRIMARY_H, "primary height drift");
    require(lv_obj_get_x(primary) + lv_obj_get_width(primary) / 2 == 233,
            "primary is not horizontally centered");
    require(lv_obj_get_y(primary) + lv_obj_get_height(primary) / 2 == 380,
            "primary is not centered at y380");
    lv_obj_t *secondary = orb_kit_button_secondary_create(root, "Done");
    require(orb_kit_is_primary(primary) && !orb_kit_is_primary(secondary),
            "capsule primary roles unavailable");
    lv_obj_update_layout(secondary);
    require(lv_obj_get_height(secondary) == ORB_KIT_SECONDARY_H, "secondary height drift");
    lv_obj_t *a = orb_kit_glyph_button_create(root, ORB_ICON_X, false, ORB_KIT_TIME);
    lv_obj_t *b = orb_kit_glyph_button_create(root, ORB_ICON_PAUSE, true, ORB_KIT_TIME);
    require(!orb_kit_is_primary(a) && orb_kit_is_primary(b), "glyph primary roles unavailable");
    orb_kit_place_arc_pair(a, b);
    lv_obj_update_layout(root);
    require(lv_obj_get_x(a) + lv_obj_get_x(b) + ORB_KIT_DISC == 466,
            "glyph pair is not symmetric");
    require(lv_obj_get_y(a) == lv_obj_get_y(b), "glyph pair has unequal arc elevation");
    require(abs(lv_obj_get_x(a) + 40 - 154) <= 2 && abs(lv_obj_get_y(a) + 40 - 356) <= 2,
            "glyph pair differs from Timer frame");
    lv_obj_send_event(primary, LV_EVENT_PRESSED, NULL);
    advance(96);
    int32_t resting_width = lv_obj_get_width(primary);
    int32_t pressed_inset = lv_obj_get_style_transform_width(primary, 0);
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "live AX3 resize unavailable");
    lv_obj_update_layout(root);
    require(lv_obj_get_x(primary) + lv_obj_get_width(primary) / 2 == 233 &&
            lv_obj_get_y(primary) + lv_obj_get_height(primary) / 2 == 380,
            "live Dynamic Type resize moved bottom primary off its anchor");
    int32_t resized_width = lv_obj_get_width(primary);
    int32_t resized_inset = lv_obj_get_style_transform_width(primary, 0);
    require(resized_width > resting_width && resized_inset < pressed_inset &&
            abs((resized_width + 2 * resized_inset) * 100 - resized_width * 94) <= 200,
            "fully pressed capsule kept its old contraction after Dynamic Type resize");
    orb_kit_press_cancel(primary);
    require(press_geometry_at_rest(primary), "resized capsule did not reset after cancellation");
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "default font restore failed");
    lv_obj_clean(root);
}

static void luminance(void)
{
    require(orb_kit_accent_uses_black(ORB_KIT_HOME), "yellow glyph needs black ink");
    require(orb_kit_accent_uses_black(ORB_TEAL), "teal glyph needs black ink");
    require(orb_kit_accent_uses_black(ORB_KIT_MINT), "mint glyph needs black ink");
    require(orb_kit_accent_uses_black(ORB_KIT_CLAUDE), "orange glyph needs black ink for 3:1 contrast");
    require(orb_kit_accent_uses_black(ORB_KIT_PLAY), "green glyph needs black ink for 3:1 contrast");
    require(!orb_kit_accent_uses_black(ORB_KIT_CODEX), "blue glyph needs white ink");
    require(!orb_kit_accent_uses_black(ORB_KIT_DOT), "indigo glyph needs white ink");
    require(lv_color_eq(orb_kit_accent_ink(lv_color_black()), lv_color_white()),
            "black surface has dark ink");
    require(lv_color_eq(orb_kit_accent_ink(lv_color_white()), lv_color_black()),
            "white surface has light ink");
    const lv_color_t palette[] = {ORB_KIT_CLAUDE, ORB_KIT_CODEX, ORB_KIT_DOT,
        ORB_KIT_HOME, ORB_KIT_TIME, ORB_KIT_PLAY, ORB_KIT_SOUND, ORB_KIT_DATE, ORB_KIT_GREY};
    for (unsigned i = 0; i < sizeof(palette) / sizeof(palette[0]); i++) {
        float channels[] = {palette[i].red / 255.f, palette[i].green / 255.f, palette[i].blue / 255.f};
        for (unsigned j = 0; j < 3; j++)
            channels[j] = channels[j] <= .04045f ? channels[j] / 12.92f : powf((channels[j] + .055f) / 1.055f, 2.4f);
        float background = channels[0] * .2126f + channels[1] * .7152f + channels[2] * .0722f;
        float ink = lv_color_eq(orb_kit_accent_ink(palette[i]), lv_color_black()) ? 0 : 1;
        float contrast = (fmaxf(background, ink) + .05f) / (fminf(background, ink) + .05f);
        require(contrast >= 3.f, "identity disc glyph fails HIG 3:1 contrast");
    }
}

static void press_cancel(lv_obj_t *root)
{
    lv_obj_t *obj = orb_kit_glyph_button_create(root, ORB_ICON_PLAY, true, ORB_KIT_TIME);
    lv_obj_add_event_cb(obj, clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_send_event(obj, LV_EVENT_PRESSED, NULL);
    advance(96);
    int32_t pressed_diameter = 80 + 2 * lv_obj_get_style_transform_width(obj, 0);
    require(lv_obj_get_style_transform_scale_x(obj, 0) == 256 &&
            abs(pressed_diameter * 100 - 80 * 94) <= 200 && pressed_diameter < 80,
            "native glass press does not reach the prepared 94 percent geometry");
    lv_obj_send_event(obj, LV_EVENT_PRESS_LOST, NULL);
    require(orb_kit_press_is_cancelled(obj), "lost contact was not retired");
    require(press_geometry_at_rest(obj),
            "cancel left a pressed transform flash");
    lv_obj_send_event(obj, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
    require(s_clicks == 0, "cancelled contact activated");
    advance(200);
    require(press_geometry_at_rest(obj),
            "retired animation restored pressed state");
    lv_obj_send_event(obj, LV_EVENT_PRESSED, NULL);
    require(!orb_kit_press_is_cancelled(obj), "fresh contact remained cancelled");
    lv_obj_send_event(obj, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
    require(s_clicks == 1, "fresh release failed to activate");
    orb_ui_set_reduce_motion(true);
    lv_obj_send_event(obj, LV_EVENT_PRESSED, NULL);
    orb_kit_press_cancel(obj);
    require(press_geometry_at_rest(obj),
            "Reduce Motion cancel left pressed geometry");
    orb_ui_set_reduce_motion(false);
    lv_obj_delete(obj);
}

static void text_fit(lv_obj_t *root)
{
    lv_obj_t *obj = orb_kit_rim_title_create(root, "TIMER", ORB_KIT_TIME);
    orb_kit_rim_text_info_t info;
    require(orb_kit_rim_text_info(obj, &info), "rim info unavailable");
    require(info.style == ORB_TEXT_FOOTNOTE1 && !info.ellipsised,
            "short title unnecessarily steps down");
    require(info.angle_degrees > 0 && info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES,
            "short title arc outside bounds");
    char text[160]; memset(text, 'M', sizeof(text)); text[sizeof(text) - 1] = 0;
    bool found_step = false;
    /* AX3's two footnote em sizes differ by just one pixel, and individual
     * glyph advances can round to the same width. Find a measured boundary
     * using the actual published fonts rather than assuming an M advance. */
    const char *letters = "MWABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (unsigned letter = 0; letters[letter] && !found_step; ++letter) {
        for (unsigned n = 1; n < sizeof(text); ++n) {
            memset(text, letters[letter], n);
            text[n] = 0;
            float large = orb_kit_rim_text_measure(text, ORB_TEXT_FOOTNOTE1, ORB_KIT_TITLE_RADIUS);
            float small = orb_kit_rim_text_measure(text, ORB_TEXT_FOOTNOTE2, ORB_KIT_TITLE_RADIUS);
            if (large > ORB_KIT_TITLE_MAX_DEGREES && small <= ORB_KIT_TITLE_MAX_DEGREES) {
                orb_kit_rim_text_set(obj, text, NULL);
                require(orb_kit_rim_text_info(obj, &info), "stepped rim info unavailable");
                require(info.style == ORB_TEXT_FOOTNOTE2 && !info.ellipsised,
                        "title overflow did not step down before ellipsis");
                found_step = true; break;
            }
        }
    }
    if (!found_step) fprintf(stderr, "step boundary size=%d bold=%d nominal=%u/%u M=%.3f/%.3f\n",
        orb_fonts_content_size(), orb_fonts_bold_text(), orb_text_style_info(ORB_TEXT_FOOTNOTE1)->size_px,
        orb_text_style_info(ORB_TEXT_FOOTNOTE2)->size_px,
        orb_kit_rim_text_measure("MMMMMMMMMMMMMMMMMMMM", ORB_TEXT_FOOTNOTE1, 200),
        orb_kit_rim_text_measure("MMMMMMMMMMMMMMMMMMMM", ORB_TEXT_FOOTNOTE2, 200));
    require(found_step, "could not exercise font step boundary");
    memset(text, 'M', sizeof(text) - 1); text[sizeof(text) - 1] = 0;
    orb_kit_rim_text_set(obj, text, NULL);
    require(orb_kit_rim_text_info(obj, &info), "ellipsis rim info unavailable");
    require(info.style == ORB_TEXT_FOOTNOTE2 && info.ellipsised,
            "long title did not ellipsise after Footnote2");
    require(info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES + 0.01f, "ellipsised title exceeds its arc budget");
    advance(1); /* Changed rim sources prepare after the setter's caller unwinds. */
    require(lv_obj_get_child_count(obj) >= 2 &&
            lv_obj_raster_ready(lv_obj_get_child(obj, 0)) &&
            lv_obj_raster_ready(lv_obj_get_child(obj, 1)),
            "long rim slices were not prepared");
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_invalidate(root);
        lv_refr_now(lv_display_get_default());
    }
    lv_obj_raster_get_stats(&after);
    require(before.builds == after.builds && before.rejections == after.rejections,
            "long rim draw rebuilt or rejected its source");
    lv_obj_delete(obj);
    lv_obj_t *caption = orb_kit_rim_caption_create(root, "HOME");
    require(orb_kit_rim_caption_set_style(caption, ORB_TEXT_CAPTION2, ORB_TEXT_EMPHASIZED,
                                         lv_color_white(), LV_OPA_COVER) &&
            orb_kit_rim_text_info(caption, &info) && info.style == ORB_TEXT_CAPTION2,
            "focused caption did not adopt Caption2 emphasized style");
    advance(1);
    require(lv_obj_get_child_count(caption) >= 2 &&
            lv_obj_raster_ready(lv_obj_get_child(caption, 0)) &&
            lv_obj_raster_ready(lv_obj_get_child(caption, 1)),
            "restyled caption slices were not prepared");
    lv_obj_delete(caption);
}

static void position_indicators(lv_obj_t *root)
{
    lv_obj_t *dots = orb_kit_page_dots_create(root, 5);
    lv_obj_update_layout(dots);
    int32_t first_left = 0, previous_right = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(dots); i++) {
        lv_obj_t *dot = lv_obj_get_child(dots, i);
        lv_area_t bounds;
        lv_obj_get_coords(dot, &bounds);
        int32_t x = (bounds.x1 + bounds.x2 + 1) / 2 - ORB_SCREEN_R;
        int32_t y = (bounds.y1 + bounds.y2 + 1) / 2 - ORB_SCREEN_R;
        /* Alive's selected page is a 20 px capsule; the other dots keep their
         * original 8 px geometry. This changes no idle-motion assertion. */
        require(lv_obj_get_width(dot) == (i ? 8 : 20) && lv_obj_get_height(dot) == 8 &&
                x * x + y * y < 191 * 191,
                "page dots do not fit inside the rim ring");
        if (i) require(bounds.x1 - previous_right - 1 == 6, "page capsule lost neighbour spacing");
        else first_left = bounds.x1;
        previous_right = bounds.x2;
    }
    require(first_left + previous_right + 1 == 2 * ORB_SCREEN_R, "page capsule group is not centred");
    lv_obj_t *arc = orb_kit_scroll_arc_create(root, 0, 5);
    lv_obj_update_layout(arc);
    require(lv_obj_get_width(arc) == 452 && lv_obj_get_style_arc_width(arc, LV_PART_MAIN) == 4,
            "paged arc geometry differs from list scroll arc");
    float start = lv_arc_get_angle_start(arc);
    orb_kit_scroll_arc_set_position(arc, 4, 5);
    require(lv_arc_get_angle_start(arc) != start && !lv_obj_is_hidden(arc),
            "paged arc does not track position");
    orb_kit_scroll_arc_set_position(arc, 0, 1);
    require(lv_obj_is_hidden(arc), "one page retained an unnecessary position arc");
    lv_obj_delete(arc); lv_obj_delete(dots);
}

static void unchanged_scroll_indicators(lv_obj_t *root)
{
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 466, 466);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    for (unsigned i = 0; i < 8; i++) {
        lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
            .title="Accessibility", .glyph=ORB_ICON_GEAR, .accent=ORB_KIT_GREY,
        });
        lv_obj_set_y(row, 96 + 84 * i);
    }
    require(orb_kit_list_apply(list), "idle indicator list preparation failed");
    lv_obj_t *arc = orb_kit_scroll_arc_attach(list);
    require(arc != NULL, "idle indicator missing");
    require(orb_round_list_scroll_to(list, 160), "idle indicator could not scroll");
    orb_round_list_scroll_finish(list);
    advance(1000); lv_refr_now(lv_display_get_default());
    lv_area_t bounds; lv_obj_get_coords(arc, &bounds);
    require(bounds.x1 == ORB_SCREEN_R - ORB_KIT_SCROLL_RADIUS - 2 &&
            bounds.y1 == ORB_SCREEN_R - ORB_KIT_SCROLL_RADIUS - 2,
            "attached scroll arc is not centred on the display");
    unsigned frames = s_frames;
    size_t allocations = sim_memory_allocation_calls();
    for (unsigned i = 0; i < 10; i++) {
        lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
        lv_obj_send_event(list, LV_EVENT_LAYOUT_CHANGED, NULL);
        lv_refr_now(lv_display_get_default());
    }
    require(s_frames == frames, "unchanged list events redrew the scroll arc");
    require(sim_memory_allocation_calls() == allocations,
            "unchanged list events allocated for the scroll arc");
    lv_obj_delete(list);
    arc = orb_kit_scroll_arc_create(root, 4, 5);
    advance(1000); lv_refr_now(lv_display_get_default());
    frames = s_frames;
    for (unsigned i = 0; i < 10; i++) {
        orb_kit_scroll_arc_set_position(arc, 4, 5);
        lv_refr_now(lv_display_get_default());
    }
    require(s_frames == frames, "unchanged wrapped page position redrew the scroll arc");
    lv_obj_delete(arc);
}

static void values(lv_obj_t *root)
{
    lv_obj_t *ring = orb_kit_ring_create(root, 218, 10, ORB_KIT_CLAUDE);
    lv_obj_update_layout(ring);
    require(lv_obj_get_width(ring) == 446 && lv_obj_get_height(ring) == 446 &&
            lv_obj_get_x(ring) == 10 && lv_obj_get_y(ring) == 10,
            "ring centerline radius drift");
    require(lv_obj_get_style_arc_width(ring, LV_PART_MAIN) == 10 &&
            lv_obj_get_style_arc_width(ring, LV_PART_INDICATOR) == 10,
            "ring stroke width drift");
    lv_obj_t *slider = orb_kit_rim_slider_create(root, 213, 14, ORB_KIT_HOME,
                                               0, 100, 5, detent, NULL);
    orb_kit_rim_slider_set_value(slider, 80, false);
    lv_obj_update_layout(slider);
    lv_obj_t *knob = lv_obj_get_child(slider, 0);
    require(knob && lv_obj_get_width(knob) == 30 && lv_obj_get_height(knob) == 30,
            "rim slider knob is not 30px");
    lv_area_t bounds;
    lv_obj_get_coords(knob, &bounds);
    require(abs(bounds.x1 + 15 - 443) <= 2 && abs(bounds.y1 + 15 - 200) <= 2,
            "slider knob differs from HomeLight frame");
    orb_kit_rim_slider_set_value(slider, -50, false);
    require(orb_kit_rim_slider_get_value(slider) == 0, "slider minimum not clamped");
    orb_kit_rim_slider_set_value(slider, 150, false);
    require(orb_kit_rim_slider_get_value(slider) == 100, "slider maximum not clamped");
    require(s_detents == 0, "programmatic slider update emitted user detents");
    lv_obj_t *toggle = orb_kit_switch_create(root, false, ORB_KIT_PLAY);
    require(!orb_kit_switch_get_checked(toggle), "switch default changed");
    orb_kit_switch_set_checked(toggle, true, true);
    advance(320);
    require(orb_kit_switch_get_checked(toggle), "switch setter did not commit");
    orb_ui_set_reduce_motion(true);
    orb_kit_switch_set_checked(toggle, false, true);
    require(!orb_kit_switch_get_checked(toggle), "Reduce Motion switch setter failed");
    lv_obj_send_event(toggle, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(toggle, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_send_event(toggle, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(toggle, LV_EVENT_CLICKED, NULL);
    require(!orb_kit_switch_get_checked(toggle), "cancelled switch toggled");
    lv_obj_clean(root);
}

static void inset_title_fit(lv_obj_t *root)
{
    const char *places[] = {"SOUND", "BATTERY", "UPDATES", "ACCESSIBILITY",
        "GUEST SUITE BATH", "Alex's Mac mini — Studio display"};
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        lv_obj_t *title = orb_kit_rim_title_inset_create(root, places[i], lv_color_white());
        require(title != NULL, "inset title unavailable");
        orb_kit_rim_text_info_t info;
        require(orb_kit_rim_text_info(title, &info) &&
                info.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES + .01f,
                "inset title exceeds 120 degree budget");
        for (unsigned slice = 0; slice < 2; slice++) {
            lv_obj_t *arc = lv_obj_get_child(lv_obj_get_child(title, slice), 0);
            require(lv_arclabel_get_radius(arc) == ORB_KIT_TITLE_INSET_RADIUS,
                    "inset title baseline drifted");
        }
        lv_refr_now(lv_display_get_default());
        sim_display_rgb(s_pixels);
        unsigned ink = 0;
        for (int y = 0; y < SIM_D; y++) {
            for (int x = 0; x < SIM_D; x++) {
                const uint8_t *pixel = &s_pixels[(y * SIM_D + x) * 3];
                if (pixel[0] < 8 && pixel[1] < 8 && pixel[2] < 8) continue;
                ink++;
                int dx = x - ORB_SCREEN_R, dy = y - ORB_SCREEN_R;
                /* Narrowest full rim in these screens: radius213, width14.
                 * Test real rendered ink, not the intentionally padded cache. */
                require(dx * dx + dy * dy < 206 * 206,
                        "inset title ink touches the full rim ring");
            }
        }
        require(ink > 0, "inset title rendered no ink");
        require(orb_kit_rim_title_set_inset(title, false), "title failed to return to outer baseline");
        require(lv_arclabel_get_radius(lv_obj_get_child(lv_obj_get_child(title, 0), 0)) == ORB_KIT_TITLE_RADIUS,
                "outer title baseline did not restore");
        lv_obj_delete(title);
    }
    lv_obj_t *caption = orb_kit_rim_caption_create(root, "Sound");
    require(!orb_kit_rim_title_set_inset(caption, true), "caption accepted title-only inset");
    lv_obj_delete(caption);
}

static void label_ink_vertical(lv_obj_t *label, int32_t *top, int32_t *bottom)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    const unsigned char *text = (const unsigned char *)lv_label_get_text(label);
    *top = INT32_MAX; *bottom = INT32_MIN;
    /* These real Settings strings are ASCII; bounds follow the actual glyph
     * boxes used by lv_draw_label, including the descenders in “Display”. */
    for (unsigned i = 0; text[i]; i++) {
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, text[i], text[i + 1]) || !glyph.box_h) continue;
        int32_t y = lv_obj_get_y(label) + font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        *top = LV_MIN(*top, y); *bottom = LV_MAX(*bottom, y + glyph.box_h - 1);
    }
    require(*top != INT32_MAX, "row text has no measurable ink");
}

static void real_row_fit(lv_obj_t *root)
{
    const orb_kit_row_config_t fixtures[] = {
        {.title="Accessibility", .subtitle="Reduce Transparency", .glyph=ORB_ICON_GEAR, .accent=ORB_KIT_GREY},
        {.title="Alex's Mac mini", .subtitle="Disconnected from hub", .glyph=ORB_ICON_GEAR, .accent=ORB_KIT_GREY},
        {.title="Display brightness", .value="100%", .glyph=ORB_ICON_SUN, .accent=ORB_KIT_CODEX},
        {.title="Bold Text", .glyph=ORB_ICON_GEAR, .accent=ORB_KIT_GREY, .show_switch=true},
        {.title="Desk charge limit", .subtitle="Keep battery at 80%", .glyph=ORB_ICON_GEAR,
            .accent=ORB_KIT_GREY, .show_switch=true},
        {.title="Spoken replies", .subtitle="Read replies aloud", .glyph=ORB_ICON_SPEAKER_HIGH,
            .accent=ORB_KIT_SOUND, .show_switch=true},
    };
    for (unsigned i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        lv_obj_t *list = lv_obj_create(root);
        lv_obj_remove_style_all(list);
        lv_obj_set_size(list, SIM_D, SIM_D);
        lv_obj_t *row = orb_kit_row_create(list, &fixtures[i]);
        lv_obj_set_y(row, 197);
        lv_obj_update_layout(row);
        lv_obj_t *title = lv_obj_get_child(row, 1);
        /* Default sizes keep one title line; AX3 may wrap to two (HIG §4). */
        int32_t title_lines = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? 2 : 1;
        require(lv_label_get_max_lines(title) <= title_lines,
                "long row title wrapped to a second line");
        int32_t row_h = lv_obj_get_height(row);
        require(row_h >= ORB_KIT_ROW_H && (title_lines == 2 || row_h == ORB_KIT_ROW_H),
                "row is not 72 px at default sizes");
        require(lv_obj_get_x(title) >= 16 + ORB_KIT_DISC_ROW + 14,
                "row title touches leading disc");
        int32_t title_top, title_bottom;
        label_ink_vertical(title, &title_top, &title_bottom);
        require(title_top >= 0 && title_bottom < row_h,
                "row title ink escapes 72px row");
        if (fixtures[i].subtitle) {
            int32_t subtitle_top, subtitle_bottom;
            label_ink_vertical(lv_obj_get_child(row, 2), &subtitle_top, &subtitle_bottom);
            if (title_bottom + 2 >= subtitle_top) fprintf(stderr,
                "row overlap size=%d bold=%d title=%s ink=%d..%d subtitle=%d..%d\n",
                orb_fonts_content_size(), orb_fonts_bold_text(), fixtures[i].title,
                title_top, title_bottom, subtitle_top, subtitle_bottom);
            require(title_bottom + 2 < subtitle_top,
                    "row title and subtitle glyphs overlap");
            require(subtitle_bottom < row_h,
                    "row subtitle escapes 72px row");
        }
        require(orb_kit_list_apply(list), "real string row preparation failed");
        orb_round_list_cache_stats_t before, after;
        require(orb_round_list_cache_stats(list, &before), "real row cache stats unavailable");
        lv_obj_t *arc = orb_kit_scroll_arc_attach(list);
        require(arc && lv_obj_get_width(arc) == 2 * (ORB_KIT_SCROLL_RADIUS + 2) &&
                lv_obj_get_style_arc_width(arc, LV_PART_MAIN) == 4,
                "scroll arc is not at radius224 with 4px stroke");
        for (int offset = 0; offset <= 120; offset += 8) {
            lv_obj_set_y(row, 197 + offset);
            lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
            lv_area_t bounds, disc, text;
            if (!orb_round_list_row_bounds(row, &bounds)) continue;
            require(orb_round_list_child_bounds(row, lv_obj_get_child(row, 0), &disc) &&
                    orb_round_list_child_bounds(row, title, &text), "row content bounds unavailable");
            require(disc.x2 < text.x1, "chord fitted title overlaps its leading disc");
            require(bounds.x2 < ORB_SCREEN_R + ORB_KIT_SCROLL_RADIUS - 2,
                    "widest row touches scroll arc");
        }
        require(orb_round_list_cache_stats(list, &after) &&
                after.source_builds == before.source_builds,
                "row scrolling rebuilt prepared sources");
        size_t allocations = sim_memory_allocation_calls();
        for (int offset = 120; offset >= 0; offset -= 8) {
            lv_obj_set_y(row, 197 + offset);
            lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
        }
        require(sim_memory_allocation_calls() == allocations,
                "prepared row/scroll-arc path allocated during scroll");
        lv_obj_delete(list);
    }
}

static void row_overlay(lv_obj_t *root)
{
    orb_ui_set_reduce_motion(false);
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 466, 466);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(list, 200, 0);
    lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title="Quiet", .glyph=ORB_ICON_MOON, .accent=ORB_KIT_DOT, .show_switch=true, .checked=true,
    });
    lv_obj_set_y(row, 190);
    lv_obj_t *last = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title="About", .glyph=ORB_ICON_GEAR, .accent=ORB_KIT_GREY, .show_switch=true,
    });
    lv_obj_set_y(last, 1400);
    require(orb_kit_list_apply(list), "row overlay list preparation failed");
    lv_obj_t *toggle = orb_kit_row_switch(row);
    require(toggle && lv_obj_get_parent(toggle) == list, "row switch is not a live list overlay");
    lv_area_t row_bounds, toggle_bounds;
    require(orb_round_list_row_bounds(row, &row_bounds), "row presentation bounds unavailable");
    lv_obj_get_coords(toggle, &toggle_bounds);
    require(toggle_bounds.x1 >= row_bounds.x1 && toggle_bounds.x2 <= row_bounds.x2 &&
            toggle_bounds.y1 >= row_bounds.y1 && toggle_bounds.y2 <= row_bounds.y2,
            "live switch escaped row presentation");
    orb_round_list_cache_stats_t before, after;
    require(orb_round_list_cache_stats(list, &before), "row overlay cache stats unavailable");
    orb_kit_switch_set_checked(toggle, false, true);
    advance(96);
    require(orb_round_list_cache_stats(list, &after) && before.source_builds == after.source_builds,
            "switch travel rebuilt row source");
    lv_obj_send_event(toggle, LV_EVENT_PRESSED, NULL);
    require(orb_round_list_scroll_to(list, 120), "managed list did not accept scroll");
    require(orb_kit_press_is_cancelled(toggle), "scroll did not retire live switch contact");
    orb_round_list_scroll_finish(list);
    /* Start outside the lookahead window, then jump directly into focus. An
     * entering switch follows its source after deferred viewport preparation. */
    lv_obj_t *last_toggle = orb_kit_row_switch(last);
    require(last_toggle != NULL, "final row switch unavailable");
    require(orb_round_list_scroll_to(list, 1200), "managed list did not accept end jump");
    orb_round_list_scroll_finish(list);
    advance(32);
    lv_refr_now(lv_obj_get_display(list));
    require(!lv_obj_is_hidden(last_toggle), "entering row kept its offscreen switch hidden");
    size_t allocations = sim_memory_allocation_calls();
    lv_display_send_event(lv_obj_get_display(list), LV_EVENT_REFR_START, NULL);
    require(sim_memory_allocation_calls() == allocations, "overlay refresh allocated at a stable pose");
    lv_display_send_event(lv_obj_get_display(list), LV_EVENT_REFR_READY, NULL);
    require(orb_round_list_row_bounds(last, &row_bounds), "entering row bounds unavailable");
    lv_obj_get_coords(last_toggle, &toggle_bounds);
    require(toggle_bounds.x1 >= row_bounds.x1 && toggle_bounds.x2 <= row_bounds.x2 &&
            toggle_bounds.y1 >= row_bounds.y1 && toggle_bounds.y2 <= row_bounds.y2,
            "entering switch retained its previous scroll pose");
    lv_obj_t *indicator = orb_kit_scroll_arc_attach(list);
    require(indicator != NULL, "scroll indicator unavailable");
    lv_obj_delete(indicator);
    indicator = orb_kit_scroll_arc_attach(list);
    require(indicator != NULL, "scroll indicator did not reattach after deletion");
    lv_obj_delete(row);
    require(!lv_obj_is_valid(toggle), "deleting row leaked live switch overlay");
    lv_obj_delete(list);
    require(!lv_obj_is_valid(indicator), "deleting list leaked scroll indicator");
}

static void pointer_state(int x, int y, bool pressed, unsigned milliseconds)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    advance(milliseconds);
}

static void absolute_slider_contact(lv_obj_t *root)
{
    lv_obj_t *slider = orb_kit_rim_slider_create(root, 213, 12, ORB_KIT_SOUND, 0, 100, 1, detent, NULL);
    orb_kit_rim_slider_set_angles(slider, 145, -110);
    orb_kit_rim_slider_set_value(slider, 75, false);
    lv_obj_update_layout(root);
    s_input = (lv_indev_data_t){.point={117, 412}, .state=LV_INDEV_STATE_PRESSED};
    unsigned callbacks = s_detents;
    lv_indev_read(s_pointer);
    require(abs(orb_kit_rim_slider_get_value(slider) - 20) <= 1,
            "first slider contact did not choose absolute visible value");
    require(s_detents == callbacks + 1, "first absolute slider contact missed its detent");
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    lv_indev_reset(s_pointer, NULL);
    lv_obj_delete(slider);
}

static void real_scroll_cancel(lv_obj_t *root, bool toggle)
{
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 466, 466);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *control = toggle ? orb_kit_switch_create(list, false, ORB_KIT_PLAY) :
        orb_kit_button_primary_create(list, "Play", ORB_KIT_PLAY);
    lv_obj_set_pos(control, 170, 220);
    lv_obj_t *filler = lv_obj_create(list);
    lv_obj_remove_style_all(filler);
    lv_obj_set_pos(filler, 0, 700);
    lv_obj_set_size(filler, 466, 72);
    lv_obj_set_clickable(filler, false);
    lv_obj_add_event_cb(control, clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(root);
    unsigned clicks = s_clicks;
    int x = 170 + lv_obj_get_width(control) / 2;
    int y = 220 + lv_obj_get_height(control) / 2;
    pointer_state(x, y, true, 96);
    for (unsigned i = 1; i <= 10; ++i) pointer_state(x, y - i * 14, true, 16);
    require(lv_obj_get_scroll_y(list) > 0, "pointer trace did not acquire scrolling");
    pointer_state(x, y - 140, false, 64);
    require(s_clicks == clicks, "scrolling contact activated kit control");
    require(orb_kit_press_is_cancelled(control), "scrolling pointer contact was not retired");
    require(press_geometry_at_rest(control),
            "scroll release left a final pressed flash");
    if (toggle) require(!orb_kit_switch_get_checked(control), "scrolling pointer toggled switch");
    lv_indev_reset(s_pointer, NULL);
    lv_obj_delete(list);
}

static void real_contact_retirement(lv_obj_t *root)
{
    lv_obj_t *button = orb_kit_button_primary_create(root, "Play", ORB_KIT_PLAY);
    lv_obj_set_pos(button, 150, 200);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(root);
    unsigned clicks = s_clicks;
    pointer_state(210, 230, true, 96);
    pointer_state(224, 230, true, 64); /* Still inside, past the 10px contact slop. */
    pointer_state(224, 230, false, 64);
    require(s_clicks == clicks && orb_kit_press_is_cancelled(button),
            "moved contact activated a non-scrollable button");
    require(press_geometry_at_rest(button),
            "retired non-scrolling contact left pressed geometry");
    pointer_state(210, 230, true, 64);
    pointer_state(210, 230, false, 64);
    require(s_clicks == clicks + 1, "fresh stationary tap failed after contact retirement");
    pointer_state(210, 230, true, 64);
    pointer_state(420, 230, true, 64);
    pointer_state(210, 230, true, 64);
    pointer_state(210, 230, false, 64);
    require(s_clicks == clicks + 1 && orb_kit_press_is_cancelled(button),
            "leaving and re-entering rearmed a retired contact");
    require(press_geometry_at_rest(button),
            "re-entered retired contact left pressed geometry");
    lv_indev_reset(s_pointer, NULL);
    lv_obj_delete(button);
}

static void moving_target_contact(lv_obj_t *root)
{
    lv_obj_t *button = orb_kit_glyph_button_create(root, ORB_ICON_PLAY, true, ORB_KIT_TIME);
    lv_obj_set_pos(button, 150, 180);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(root);
    unsigned clicks = s_clicks;
    pointer_state(190, 220, true, 32);
    lv_obj_set_x(button, 270);
    lv_obj_update_layout(root);
    pointer_state(190, 220, true, 32);
    pointer_state(190, 220, false, 32);
    require(s_clicks == clicks + 1 && !orb_kit_press_is_cancelled(button),
            "stationary tap lost its press-time hit while target moved");
    lv_indev_reset(s_pointer, NULL);
    lv_obj_delete(button);
}

static void font_observer_lifetime(lv_obj_t *root)
{
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
            "font observer default setup failed");
    require(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "font observer weight setup failed");
    uint32_t baseline = lv_ll_get_len(&orb_fonts_subject()->subs_ll);
    lv_obj_t *button = orb_kit_button_primary_create(root, "Start", ORB_KIT_PLAY);
    lv_obj_t *glyph = orb_kit_glyph_button_create(root, ORB_ICON_PAUSE, true, ORB_KIT_TIME);
    uint32_t live = lv_ll_get_len(&orb_fonts_subject()->subs_ll);
    for (unsigned i = 0; i < 3; ++i) {
        require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK,
                "font observer AX3 transition failed");
        require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
                "font observer default transition failed");
        require(lv_ll_get_len(&orb_fonts_subject()->subs_ll) == live,
                "font resize accumulated label ink observers");
    }
    lv_obj_delete(button);
    lv_obj_delete(glyph);
    require(lv_ll_get_len(&orb_fonts_subject()->subs_ll) == baseline,
            "component deletion leaked font observers");
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    require(sim_display_create() != NULL, "display unavailable");
    lv_display_add_event_cb(lv_display_get_default(), rendered, LV_EVENT_RENDER_READY, NULL);
    s_pointer = lv_indev_create();
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_pointer, pointer_read);
    orb_ui_init(NULL);
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "fonts unavailable");
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_scrollable(root, false);
    sizes_and_placement(root); luminance(); press_cancel(root); text_fit(root); inset_title_fit(root); real_row_fit(root); position_indicators(root); unchanged_scroll_indicators(root); values(root); row_overlay(root);
    real_scroll_cancel(root, false); real_scroll_cancel(root, true);
    real_contact_retirement(root);
    moving_target_contact(root);
    absolute_slider_contact(root);
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "AX3 fonts unavailable");
    text_fit(root); inset_title_fit(root); real_row_fit(root);
    require(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "AX3 bold fonts unavailable");
    orb_ui_set_reduce_transparency(true);
    text_fit(root); inset_title_fit(root); real_row_fit(root);
    font_observer_lifetime(root);
    printf("kit contract: %u checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
