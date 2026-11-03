#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "sim_display.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_text_private.h"

LV_FONT_DECLARE(orb_inter_28_regular)

static unsigned s_checks;
static uint32_t s_now;
static unsigned s_widow_draws;
static uint32_t tick(void) { return s_now; }

#define CHECK(value) do { s_checks++; if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)

static bool valid_utf8(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned n = *p < 0x80 ? 1 : *p >= 0xC2 && *p < 0xE0 ? 2 :
                     *p >= 0xE0 && *p < 0xF0 ? 3 : *p >= 0xF0 && *p < 0xF5 ? 4 : 0;
        if (!n) return false;
        for (unsigned i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
        }
        p += n;
    }
    return true;
}

static void dot_labels(void)
{
    const char *samples[] = {
        "Wake on wrist raise....",
        "Codex · 71% used this week",
        "On · Saving… preferences for the next session",
        "Living Room · éclair à côté du canapé",
        "ab", "é", "éñ", "🌙🌙", "a long row title",
    };
    const lv_font_t *font = orb_font(ORB_TEXT_BODY);
    for (unsigned sample = 0; sample < sizeof(samples) / sizeof(*samples); sample++) {
        lv_obj_t *label = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_font(label, font, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        for (int width = 1; width < 270; width += 7) {
            lv_obj_set_size(label, width, font->line_height);
            lv_label_set_text(label, samples[sample]);
            lv_obj_update_layout(label);
            const char *text = lv_label_get_text(label);
            CHECK(valid_utf8(text));
            if (strcmp(text, samples[sample])) CHECK(!strstr(text, "..."));
            const char *ellipsis = strstr(text, "…");
            if (ellipsis && !ellipsis[3]) {
                if (ellipsis != text) {
                    uint32_t end = (uint32_t)(ellipsis - text);
                    CHECK(!lv_label_trims_before_ellipsis(lv_text_encoded_prev(text, &end)));
                }
                lv_text_attributes_t attributes = {.max_width = width};
                lv_point_t size;
                lv_text_get_size_attributes(&size, text, font, &attributes);
                CHECK(size.x <= width && size.y <= font->line_height);
            }
            lv_obj_set_width(label, 4000);
            lv_obj_update_layout(label);
            CHECK(!strcmp(lv_label_get_text(label), samples[sample]));
        }
        lv_obj_delete(label);
    }
}

static void path_breaks(void)
{
    const char *command = "cmake --build sim/build -j4";
    const lv_font_t *font = orb_font(ORB_TEXT_BODY);
    for (int width = 1; width <= 430; width += 3) {
        lv_text_attributes_t attributes = {.max_width = width, .text_flags = LV_TEXT_FLAG_PATH_BREAKS};
        uint32_t pos = 0;
        while (command[pos]) {
            uint32_t count = lv_text_get_next_line(command + pos, LV_TEXT_LEN_MAX, font, NULL, &attributes);
            CHECK(count > 0);
            pos += count;
            CHECK(!command[pos] || command[pos] == ' ' || command[pos - 1] == ' ' || command[pos - 1] == '/');
            CHECK(command[pos - 1] != '-');
        }
    }
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_path_breaks(label, true);
    CHECK(lv_label_get_path_breaks(label));
    lv_label_set_path_breaks(label, false);
    CHECK(!lv_label_get_path_breaks(label));
    lv_obj_delete(label);
}

static void fonts(void)
{
    lv_font_glyph_dsc_t glyph;
    CHECK(lv_font_get_glyph_dsc(&orb_inter_28_regular, &glyph, 0x2026, 0) && !glyph.is_placeholder);
    for (int size = 0; size < ORB_CONTENT_SIZE_COUNT; size++) {
        CHECK(orb_fonts_set_content_size(size) == ORB_FONTS_OK);
        for (int bold = 0; bold < 2; bold++) {
            CHECK(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK);
            for (int clock = 0; clock < ORB_CLOCK_COUNT; clock++) {
                CHECK(lv_font_get_glyph_dsc(orb_clock_font(clock), &glyph, 0x2026, 0) && !glyph.is_placeholder);
            }
            for (int style = 0; style < ORB_TEXT_STYLE_COUNT; style++) {
                for (int variant = 0; variant < 4; variant++) {
                    CHECK(lv_font_get_glyph_dsc(orb_font_variant(style, variant), &glyph, 0x2026, 0) && !glyph.is_placeholder);
                    const lv_font_t *icons = orb_icon_font_variant(style, variant);
                    CHECK(lv_font_get_glyph_dsc(icons, &glyph, 0x2026, 0) && !glyph.is_placeholder);
                    for (int cap = 1; cap < 90; cap++) {
                        const lv_font_t *status = orb_status_font_for_cap(style, variant, cap);
                        CHECK(lv_font_get_glyph_dsc(status, &glyph, 0x2026, 0) && !glyph.is_placeholder);
                    }
                }
            }
        }
    }
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
}

static void widow_draw(lv_event_t *event)
{
    lv_draw_label_dsc_t *draw = lv_draw_task_get_label_dsc(lv_event_get_draw_task(event));
    if (!draw) return;
    CHECK(draw->flag & LV_TEXT_FLAG_AVOID_WIDOW);
    CHECK(!strcmp(draw->text, lv_event_get_user_data(event)));
    s_widow_draws++;
}

static void widows(lv_display_t *display)
{
    const char *samples[] = {"Increase label contrast.", "Use opaque controls."};
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    for (int bold = 0; bold < 2; bold++) {
        CHECK(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK);
        for (unsigned sample = 0; sample < sizeof(samples) / sizeof(*samples); sample++) {
            const char *text = samples[sample];
            lv_obj_t *label = orb_kit_page_label(lv_screen_active(), text, ORB_TEXT_BODY,
                false, 233, 252, 330, 3, ORB_KIT_SECONDARY);
            lv_obj_update_layout(label);
            CHECK(lv_label_get_avoid_widow(label));
            CHECK(!strcmp(lv_label_get_text(label), text));
            const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
            int32_t spacing = lv_obj_get_style_text_letter_space(label, 0);
            uint32_t second = (uint32_t)(strchr(text, ' ') + 1 - text);
            uint32_t third = (uint32_t)(strchr(text + second, ' ') + 1 - text);
            lv_text_attributes_t attributes = {
                .max_width = lv_obj_get_content_width(label), .letter_space = spacing,
                .line_space = lv_obj_get_style_text_line_space(label, 0),
                .text_flags = LV_TEXT_FLAG_AVOID_WIDOW,
            };
            int32_t used = 0;
            uint32_t first_line = lv_text_get_next_line(text, LV_TEXT_LEN_MAX, font, &used, &attributes);
            CHECK(first_line == second);
            CHECK(used == lv_text_get_line_width(text, first_line, font, &attributes));
            CHECK(lv_text_get_next_line(text + first_line, LV_TEXT_LEN_MAX, font, NULL, &attributes) == strlen(text + first_line));
            lv_point_t first_pos, second_pos, third_pos, size;
            lv_label_get_letter_pos(label, 0, &first_pos);
            lv_label_get_letter_pos(label, second, &second_pos);
            lv_label_get_letter_pos(label, third, &third_pos);
            CHECK(first_pos.y < second_pos.y && second_pos.y == third_pos.y);
            lv_text_get_size_attributes(&size, text, font, &attributes);
            CHECK(size.y == font->line_height * 2 + attributes.line_space);
            CHECK(size.x <= attributes.max_width);
            lv_obj_set_send_draw_task_events(label, true);
            lv_obj_add_event_cb(label, widow_draw, LV_EVENT_DRAW_TASK_ADDED, (void *)text);
            unsigned before = s_widow_draws;
            lv_refr_now(display);
            CHECK(s_widow_draws > before);
            CHECK(!strcmp(lv_label_get_text(label), text));
            lv_label_set_avoid_widow(label, false);
            CHECK(!lv_label_get_avoid_widow(label));
            lv_obj_delete(label);
        }
    }
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    CHECK(display != NULL);
    orb_ui_init(NULL);
    fonts();
    dot_labels();
    path_breaks();
    widows(display);
    printf("LVGL text: %u checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
