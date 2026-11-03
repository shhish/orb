/* Shared AX rows retain their semantic hierarchy and grow before truncating. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "sim_display.h"
#include "../../firmware/components/orb_ui/orb_ui_private.h"
#include "../../firmware/components/orb_ui/orb_round_source.h"
#include "src/misc/lv_text_private.h"

static unsigned s_checks;
static uint32_t s_now;
extern size_t sim_memory_allocation_calls(void);
static uint32_t tick(void) { return s_now; }
#define CHECK(valid) do { ++s_checks; if (!(valid)) { \
    fprintf(stderr, "polish rows:%d size=%d bold=%d: %s\n", __LINE__, \
        orb_fonts_content_size(), orb_fonts_bold_text(), #valid); exit(1); \
} } while (0)

static lv_obj_t *list_create(void)
{
    lv_obj_t *list = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 466, 466);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}

static void row_growth(void)
{
    lv_obj_t *list = list_create();
    lv_obj_t *first = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Reduce transparency", .subtitle = "Keep every control clear and readable",
        .accent = ORB_KIT_TIME, .glyph = ORB_ICON_GEAR, .show_switch = true,
    });
    lv_obj_t *second = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Battery", .accent = ORB_KIT_TIME, .glyph = ORB_ICON_GEAR,
    });
    CHECK(first && second);
    lv_obj_update_layout(list);
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    lv_obj_t *title = lv_obj_get_child(first, 1), *subtitle = lv_obj_get_child(first, 2);
    CHECK(lv_obj_get_height(first) >= 72);
    CHECK(lv_label_get_max_lines(title) == (ax ? 2 : 1));
    CHECK(lv_label_get_max_lines(subtitle) == (ax ? 2 : 1));
    CHECK(lv_obj_get_style_text_font(title, 0) ==
        orb_font_variant(ax ? ORB_TEXT_HEADLINE : ORB_TEXT_CAPTION2,
                          ax ? ORB_TEXT_REGULAR : ORB_TEXT_EMPHASIZED));
    CHECK(lv_obj_get_y(second) >= lv_obj_get_y(first) + lv_obj_get_height(first) + 12);
    CHECK(lv_obj_get_height(second) == 72);
    if (ax) {
        CHECK(lv_obj_get_height(first) > 72);
        /* The switch cannot consume the enlarged title's shoulder rail. */
        lv_obj_t *anchor = lv_obj_get_child(first, 3);
        CHECK(lv_obj_get_y(anchor) >= lv_obj_get_y(subtitle) + lv_obj_get_height(subtitle) -
            orb_font(ORB_TEXT_CAPTION2)->base_line);
        CHECK(lv_obj_get_width(title) > 250);
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        lv_obj_update_layout(list);
        CHECK(lv_obj_get_height(first) == 72);
        CHECK(lv_obj_get_y(second) == 84);
    }
    lv_obj_delete(list);
}

static void shoulder_prefix(void)
{
    lv_obj_t *label = lv_label_create(lv_screen_active());
    orb_text_apply(label, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(label, "Reduce transparency");
    lv_obj_set_size(label, 320, orb_font(ORB_TEXT_HEADLINE)->line_height);
    lv_draw_buf_t dots = {.header = {.w = lv_font_get_glyph_width(orb_font(ORB_TEXT_HEADLINE), 0x2026, 0)}};
    round_chord_child_t child = {.obj = label, .width = 320,
        .height = orb_font(ORB_TEXT_HEADLINE)->line_height, .dots = &dots};
    orb_round_source_cuts(&child);
    CHECK(child.cut && child.lines == 1);
    int minimum = lv_text_get_width("Redu", 4, orb_font(ORB_TEXT_HEADLINE),
        &(lv_text_attributes_t){.letter_space = lv_obj_get_style_text_letter_space(label, 0)});
    bool hidden = false, preview = false;
    for (unsigned i = 0; i < CHORD_TEXT_BUCKETS; i++) {
        uint16_t cut = child.cut[i];
        if (cut == ORB_ROUND_CUT_HIDDEN) hidden = true;
        else if (cut != UINT16_MAX) { CHECK(cut >= minimum); preview = true; }
    }
    CHECK(hidden && preview);
    orb_ui_free(child.cut);
    lv_obj_delete(label);
}

static void reader_chord(void)
{
    lv_obj_t *reader = list_create();
    lv_obj_set_pos(reader, 0, 128);
    lv_obj_set_height(reader, 300);
    lv_obj_t *heading = lv_label_create(reader);
    orb_text_apply(heading, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(heading, "Safe reading");
    lv_obj_set_pos(heading, 100, 20);
    lv_obj_t *line = lv_label_create(reader);
    orb_text_apply(line, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(line, "WWWWWWWWWWWW");
    lv_obj_set_pos(line, 60, 260);
    lv_obj_set_size(line, 346, orb_font(ORB_TEXT_HEADLINE)->line_height);
    lv_label_set_long_mode(line, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_update_layout(reader);
    int32_t height = orb_kit_reader_whole_lines(reader, 300);
    CHECK(height > 30 && height < 300);
    CHECK(!strcmp(lv_label_get_text(line), "WWWWWWWWWWWW"));
    lv_obj_delete(reader);
}

static void opt_in(void)
{
    lv_obj_t *list = list_create();
    lv_obj_t *first = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Take the medicine before going to sleep",
        .accent = ORB_KIT_TIME, .glyph = ORB_ICON_GEAR,
    });
    lv_obj_t *second = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Battery", .accent = ORB_KIT_TIME, .glyph = ORB_ICON_GEAR,
    });
    orb_kit_row_wrap_title(first, ORB_TEXT_REGULAR);
    lv_obj_update_layout(list);
    int32_t height = lv_obj_get_height(first), next_y = lv_obj_get_y(second);
    CHECK(height > 72 && next_y >= height + 12);
    orb_kit_row_wrap_title(first, ORB_TEXT_REGULAR);
    lv_obj_update_layout(list);
    CHECK(lv_obj_get_height(first) == height && lv_obj_get_y(second) == next_y);
    lv_obj_delete(list);
}

static void fitted_live_value(void)
{
    lv_obj_t *label = lv_label_create(lv_screen_active());
    orb_text_apply(label, ORB_TEXT_CAPTION2, ORB_TEXT_TABULAR, 0);
    orb_kit_value_set(label, "Codex · 71% used", "Codex · 100% used");
    orb_kit_value_fit(label, 150);
    CHECK(strstr(lv_label_get_text(label), "…"));
    CHECK(!strstr(lv_label_get_text(label), "..."));
    size_t allocations = sim_memory_allocation_calls();
    for (int width = 50; width <= 240; width++) orb_kit_value_fit(label, width);
    CHECK(sim_memory_allocation_calls() == allocations);
    orb_kit_value_fit(label, 1000);
    CHECK(!strcmp(lv_label_get_text(label), "Codex · 71% used"));
    allocations = sim_memory_allocation_calls();
    orb_kit_value_set(label, "Codex · 99% used", NULL);
    CHECK(sim_memory_allocation_calls() == allocations);
    orb_kit_value_fit(label, 70);
    CHECK(strstr(lv_label_get_text(label), "…"));
    orb_kit_value_fit(label, 1000);
    CHECK(!strcmp(lv_label_get_text(label), "Codex · 99% used"));
    lv_obj_delete(label);
}

static void live_row_growth(void)
{
    lv_obj_t *list = list_create();
    lv_obj_t *first = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Living Room", .value = "—", .accent = ORB_KIT_HOME, .glyph = ORB_ICON_GEAR,
    });
    lv_obj_t *second = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Battery", .accent = ORB_KIT_HOME, .glyph = ORB_ICON_GEAR,
    });
    lv_obj_update_layout(list);
    int32_t original_height = lv_obj_get_height(first), original_next = lv_obj_get_y(second);
    orb_kit_row_set_live_value(first, "Last known", "Last known");
    lv_obj_update_layout(list);
    CHECK(lv_obj_get_height(first) >= original_height);
    CHECK(lv_obj_get_y(second) >= lv_obj_get_y(first) + lv_obj_get_height(first) + 12);
    orb_kit_row_set_live_value(first, "—", "—");
    lv_obj_update_layout(list);
    CHECK(lv_obj_get_height(first) == original_height);
    CHECK(lv_obj_get_y(second) == original_next);
    /* A live title's longer reservation uses that same shared flow. */
    orb_kit_row_set_live_text(first, ORB_KIT_ROW_TEXT_TITLE,
        "Living room thermostat", "Living room thermostat");
    lv_obj_update_layout(list);
    CHECK(lv_obj_get_y(second) >= lv_obj_get_y(first) + lv_obj_get_height(first) + 12);
    lv_obj_delete(list);
}

static void fitted_tracking(void)
{
    const char *strings[] = {"AVATAR To Wa", "Été · déjà vu Ångström"};
    const int32_t tracking[] = {-2, 0, 3};
    for (unsigned style = 0; style < ORB_TEXT_STYLE_COUNT; style++) {
        for (unsigned t = 0; t < sizeof(tracking) / sizeof(*tracking); t++) {
            lv_obj_t *label = lv_label_create(lv_screen_active());
            orb_text_apply(label, style, ORB_TEXT_REGULAR, 0);
            for (unsigned text = 0; text < sizeof(strings) / sizeof(*strings); text++) {
                /* Set tracking after tabular selection, which reapplies the
                 * role's standard metrics on initial live registration. */
                orb_kit_value_set(label, strings[text], strings[text]);
                lv_obj_set_style_text_letter_space(label, tracking[t], 0);
                orb_kit_value_fit(label, 1000);
                orb_kit_value_set(label, strings[text], NULL);
                size_t allocations = sim_memory_allocation_calls();
                for (int32_t width = 1; width <= 500; width++) {
                    orb_kit_value_fit(label, width);
                    const char *visible = lv_label_get_text(label);
                    CHECK(lv_obj_get_style_text_letter_space(label, 0) == tracking[t]);
                    int32_t measured = lv_text_get_width(visible, strlen(visible),
                        lv_obj_get_style_text_font(label, 0),
                        &(lv_text_attributes_t){.letter_space = tracking[t]});
                    CHECK(measured <= width);
                }
                CHECK(sim_memory_allocation_calls() == allocations);
            }
            lv_obj_delete(label);
        }
    }
}

static void compact_live_row(void)
{
    lv_obj_t *list = list_create();
    lv_obj_set_scrollable(list, false);
    lv_obj_set_pos(list, 0, 348);
    lv_obj_set_height(list, 80);
    lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "", .accent = ORB_KIT_GREY, .glyph = ORB_ICON_COPY,
    });
    CHECK(orb_kit_list_apply(list));
    orb_kit_row_set_live_text(row, ORB_KIT_ROW_TEXT_TITLE,
        "Codex · 71% used", "Stopwatch · 88:88:88");
    lv_obj_update_layout(list);
    CHECK(lv_obj_get_height(row) == ORB_KIT_ROW_H);
    CHECK(lv_obj_get_height(list) == 80);
    CHECK(lv_label_get_max_lines(lv_obj_get_child(row, 1)) == 1);
    CHECK(lv_obj_get_child_count(list) == 2);
    lv_obj_t *clip = lv_obj_get_child(list, 1), *live = lv_obj_get_child(clip, 0);
    CHECK(live && lv_obj_check_type(live, &lv_label_class));
    CHECK(lv_label_get_text(live)[0]);
    CHECK(!strstr(lv_label_get_text(live), "..."));
    CHECK(!lv_obj_is_hidden(clip));
    lv_obj_delete(list);
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    CHECK(sim_display_create());
    orb_ui_init(NULL);
    for (unsigned size = 0; size < ORB_CONTENT_SIZE_COUNT; size++) {
        for (unsigned bold = 0; bold < 2; bold++) {
            CHECK(orb_fonts_set_content_size(size) == ORB_FONTS_OK);
            CHECK(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK);
            fitted_live_value();
            live_row_growth();
            fitted_tracking();
            compact_live_row();
            row_growth();
            shoulder_prefix();
            reader_chord();
            opt_in();
        }
    }
    printf("polish rows: %u checks passed\n", s_checks);
    return 0;
}
