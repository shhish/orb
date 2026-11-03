/* Regressions for the shared text lanes, measured with the shipped Inter faces. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "sim_display.h"
#include "src/misc/lv_text_private.h"
#include "../../firmware/components/orb_ui/orb_kit_value.h"
#include "../../firmware/components/orb_ui/orb_ui_private.h"

static unsigned s_checks;
static uint32_t s_now;
static uint32_t tick(void) { return s_now; }

static void require(bool valid, const char *message)
{
    s_checks++;
    if (!valid) {
        fprintf(stderr, "polish text: %s (size=%d bold=%d)\n", message,
                orb_fonts_content_size(), orb_fonts_bold_text());
        exit(EXIT_FAILURE);
    }
}

static int32_t width(const char *text, const lv_font_t *font)
{
    return lv_text_get_width(text, strlen(text), font, &(lv_text_attributes_t){0});
}

static bool utf8_complete(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned continuation = *p < 0x80 ? 0 : *p >= 0xc2 && *p < 0xe0 ? 1 :
            *p < 0xf0 && *p >= 0xe0 ? 2 : *p >= 0xf0 && *p < 0xf5 ? 3 : 4;
        if (continuation == 4) return false;
        p++;
        while (continuation--) if ((*p++ & 0xc0) != 0x80) return false;
    }
    return true;
}

static void truncation(void)
{
    const lv_font_t *font = orb_font(ORB_TEXT_BODY);
    const char *texts[] = {"Saving the living room", "Codex · 71% used", "naïve façade — café",
                           "Living room   ·   ", "Wake on raise…", "Pending.... operation"};
    for (unsigned t = 0; t < sizeof(texts) / sizeof(*texts); t++) {
        int32_t full = width(texts[t], font);
        for (int32_t room = width("…", font); room <= full + 5; room++) {
            char result[128];
            size_t bytes = orb_kit_text_truncate(texts[t], font, 0, room, result, sizeof(result));
            require(bytes == strlen(result), "truncation byte count");
            require(utf8_complete(result), "truncation split a UTF-8 code point");
            require(width(result, font) <= room, "ellipsis exceeds its lane");
            if (room >= full) require(!strcmp(result, texts[t]), "fitting text changed");
            else {
                require(bytes >= 3 && !strcmp(result + bytes - 3, "…"), "missing U+2026");
                if (bytes > 3) require(result[bytes - 4] != ' ' && result[bytes - 4] != '.',
                                       "space or period before ellipsis");
            }
        }
    }
    /* A caller's undersized buffer must never turn UTF-8 safety into an overwrite. */
    for (size_t capacity = 1; capacity < 9; capacity++) {
        unsigned char guarded[16]; memset(guarded, 0xa5, sizeof(guarded));
        orb_kit_text_truncate("naïve façade", font, 0, 40, (char *)guarded, capacity);
        require(memchr(guarded, 0, capacity) != NULL, "small buffer lacks terminator");
        require(utf8_complete((const char *)guarded), "small buffer split UTF-8");
        for (size_t i = capacity; i < sizeof(guarded); i++) require(guarded[i] == 0xa5, "small buffer overrun");
    }
}

static void row_growth(void)
{
    lv_obj_t *list = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(list); lv_obj_set_size(list, 466, 466);
    lv_obj_t *short_row = orb_kit_row_create(list, &(orb_kit_row_config_t){.title = "Sound"});
    require(lv_obj_get_height(short_row) == ORB_KIT_ROW_H, "fitting row moved");
    lv_obj_t *long_row = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Reduce transparency for controls", .subtitle = "Use solid surfaces for every control",
    });
    lv_obj_t *next = orb_kit_row_create(list, &(orb_kit_row_config_t){.title = "Next"});
    require(orb_kit_list_apply(list), "list preparation");
    lv_obj_update_layout(list);
    /* The disc is content; semantic title and subtitle are the next children. */
    lv_obj_t *title = lv_obj_get_child(long_row, 1), *subtitle = lv_obj_get_child(long_row, 2);
    if (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3) {
        require(lv_label_get_max_lines(title) == 2 && lv_label_get_max_lines(subtitle) == 2,
                "AX title/subtitle must use both lines before truncation");
        require(lv_obj_get_height(long_row) > ORB_KIT_ROW_H, "AX row did not grow");
        require(lv_obj_get_style_text_font(title, 0) == orb_font(ORB_TEXT_HEADLINE),
                "AX title lost its semantic weight");
        require(lv_obj_get_y(next) >= lv_obj_get_y(long_row) + lv_obj_get_height(long_row) + 12,
                "grown row overlaps its successor");
    } else {
        require(lv_obj_get_height(long_row) == ORB_KIT_ROW_H, "default row height changed");
        require(lv_obj_get_y(next) == 168, "default row cadence changed");
    }
    lv_obj_delete(list);
}

/* A question's complete choice must survive when its following context
 * straddles the reader edge. Text-only snapping used to hide the choice. */
static void reader_choices(void)
{
    lv_obj_t *reader = orb_kit_detail_page(lv_screen_active(),
        &(orb_kit_detail_page_config_t){.top = 80, .height = 240});
    int32_t title = orb_kit_paragraph(reader, "Choose", ORB_TEXT_HEADLINE, true, 0, ORB_LABEL, false);
    lv_obj_t *choice = lv_obj_create(reader);
    lv_obj_remove_style_all(choice);
    lv_obj_set_pos(choice, 0, title + 12);
    lv_obj_set_size(choice, 330, ORB_KIT_ROW_H);
    lv_obj_set_scrollable(choice, false);
    lv_obj_set_clickable(choice, true);
    int32_t choice_end = title + 12 + ORB_KIT_ROW_H;
    orb_kit_paragraph(reader, "Éggs are the following context.", ORB_TEXT_BODY,
                       false, choice_end + 12, ORB_LABEL, false);
    lv_obj_t *body = lv_obj_get_child(reader, 2);
    const lv_font_t *font = lv_obj_get_style_text_font(body, 0);
    lv_font_glyph_dsc_t glyph;
    require(lv_font_get_glyph_dsc(font, &glyph, 0x00c9, 'g'), "context glyph unavailable");
    int32_t available = choice_end + 12 + font->line_height - font->base_line -
        glyph.ofs_y - glyph.box_h / 2;
    require(available > choice_end, "context fixture does not follow its choice");
    require(orb_kit_reader_whole_lines(reader, available) == choice_end + 1,
            "reader hid a complete choice to remove partial context");
    lv_obj_delete(reader);
}

static void state_heading_balance(void)
{
    const char *text = "No climate accessories";
    lv_obj_t *state = orb_kit_state_create(lv_screen_active(), &(orb_kit_state_config_t){
        .glyph = ORB_ICON_HOUSE, .title = text, .detail = "Add an accessory in the Home app.",
        .secondary = ORB_KIT_SECONDARY, .secondary_opa = ORB_KIT_SECONDARY_OPA,
    });
    lv_obj_t *title = lv_obj_get_child(state, 1);
    lv_obj_update_layout(state);
    lv_text_attributes_t attributes = {.max_width = lv_obj_get_content_width(title),
        .letter_space = lv_obj_get_style_text_letter_space(title, 0), .text_flags = LV_TEXT_FLAG_AVOID_WIDOW};
    uint32_t first = lv_text_get_next_line(text, LV_TEXT_LEN_MAX,
        lv_obj_get_style_text_font(title, 0), NULL, &attributes);
    require(!strcmp(lv_label_get_text(title), text), "state heading changed semantic text");
    require(first > strlen("No ") && first < strlen(text), "state heading strands its short opening word");
    uint32_t second = lv_text_get_next_line(text + first, LV_TEXT_LEN_MAX,
        lv_obj_get_style_text_font(title, 0), NULL, &attributes);
    require(first + second == strlen(text), "state heading gained a third line");
    lv_obj_delete(state);
}

static void reader_display_lifetime(void)
{
    for (unsigned cycle = 0; cycle < 2; cycle++) {
        lv_obj_t *reader = orb_kit_detail_page(lv_screen_active(),
            &(orb_kit_detail_page_config_t){.top = 80, .height = 240});
        orb_kit_paragraph(reader, "Élan\nÉlan\nÉlan", ORB_TEXT_BODY, false, 0, ORB_LABEL, false);
        lv_obj_t *label = lv_obj_get_child(reader, 0);
        const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
        lv_font_glyph_dsc_t glyph;
        require(lv_font_get_glyph_dsc(font, &glyph, 0x00c9, 'l'), "lifetime glyph unavailable");
        int32_t available = 2 * font->line_height + lv_obj_get_style_text_line_space(label, 0) -
            font->base_line - glyph.ofs_y - glyph.box_h / 2;
        lv_obj_set_height(reader, available);
        int32_t expected = orb_kit_reader_whole_lines(reader, available);
        require(expected < available, "lifetime fixture does not cut a line");
        lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, NULL);
        lv_obj_update_layout(reader);
        require(lv_obj_get_height(reader) == expected, "replacement display did not snap its reader");
        lv_display_delete(lv_display_get_default());
        require(sim_display_create() != NULL, "replacement display unavailable");
    }
}

static void value_without_reservation(void)
{
    lv_obj_t *label = lv_label_create(lv_screen_active());
    orb_text_apply(label, ORB_TEXT_BODY, ORB_TEXT_REGULAR, 0);
    orb_kit_value_set(label, "71% used", "100% used");
    kit_value_t *value = orb_kit_value_find(label);
    require(value != NULL, "live value registration failed");
    /* The simulator allocator has no failure injection. Seed the state left
     * by a failed reservation copy after the value has been registered. */
    lv_free(value->reservation);
    value->reservation = NULL;
    orb_kit_value_fit(label, width("71%", lv_obj_get_style_text_font(label, 0)));
    require(value->semantic && !strcmp(value->semantic, "71% used"), "unreserved value lost its text");
    require(value->reservation != NULL, "unreserved value did not recover its reservation");
    require(width(lv_label_get_text(label), lv_obj_get_style_text_font(label, 0)) <= value->fit,
            "unreserved value exceeds its fitted lane");
    lv_obj_delete(label);
}

static void reader_text_changes(void)
{
    lv_obj_t *reader = orb_kit_detail_page(lv_screen_active(),
        &(orb_kit_detail_page_config_t){.top = 80, .height = 200});
    orb_kit_paragraph(reader, "Élan", ORB_TEXT_BODY, false, 0, ORB_LABEL, false);
    lv_obj_t *label = lv_obj_get_child(reader, 0);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    /* Fix both axes so changing glyph ink cannot notify the reader through
     * a child SIZE_CHANGED event and hide a stale-cache regression. */
    lv_obj_set_height(label, font->line_height);
    lv_obj_update_layout(reader);
    lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, NULL);
    int32_t top_before = 200, bottom_before = 200;
    orb_kit_reader_fades(reader, &top_before, &bottom_before);
    int32_t label_height = lv_obj_get_height(label);
    lv_label_set_text(label, "x");
    lv_obj_update_layout(reader);
    orb_kit_reader_snap(reader);
    lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, NULL);
    int32_t top_after = 200, bottom_after = 200;
    orb_kit_reader_fades(reader, &top_after, &bottom_after);
    require(lv_obj_get_height(label) == label_height, "text fixture changed label geometry");
    require(top_after > top_before, "reader retained the old text's ink bounds");
    lv_obj_delete(reader);
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    require(sim_display_create() != NULL, "display unavailable");
    orb_ui_init(NULL);
    for (unsigned profile = 0; profile < 4; profile++) {
        require(orb_fonts_set_content_size(profile & 1 ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) ==
                ORB_FONTS_OK, "size");
        require(orb_fonts_set_bold_text(profile & 2) == ORB_FONTS_OK, "bold");
        truncation(); row_growth(); reader_choices(); state_heading_balance();
        reader_display_lifetime(); value_without_reservation();
        reader_text_changes();
    }
    printf("polish text: %u checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
