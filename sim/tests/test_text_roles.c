/* Public text roles, balanced captions and complete-line readers using the
 * real Inter pack. These exercise live settings and actual LVGL line breaks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit_pages.h"
#include "sim_display.h"
#include "src/misc/lv_text_private.h"
#include "../../firmware/components/orb_ui/orb_ui_private.h"

static unsigned s_checks;
static uint32_t s_now;
static uint8_t s_pixels[SIM_D * SIM_D * 3];
static uint32_t tick(void) { return s_now; }

static void require(bool condition, const char *message)
{
    s_checks++;
    if (!condition) {
        fprintf(stderr, "text roles: %s (size=%d bold=%d)\n", message,
                orb_fonts_content_size(), orb_fonts_bold_text());
        exit(EXIT_FAILURE);
    }
}

static void roles(lv_obj_t *root)
{
    for (unsigned role = 0; role < ORB_TEXT_STYLE_COUNT; role++) {
        for (unsigned flags = 0; flags < 4; flags++) {
            lv_obj_t *label = lv_label_create(root);
            lv_label_set_text(label, "Readable text 123");
            lv_obj_set_style_text_font(label, orb_font(ORB_TEXT_BODY), 0);
            lv_obj_set_style_text_letter_space(label, 9, 0);
            lv_obj_set_style_text_line_space(label, 9, 0);
            orb_text_apply(label, role, flags, 0);
            const lv_font_t *normal = lv_obj_get_style_text_font(label, 0);
            require(normal == orb_font_variant(role, flags), "local face overrides semantic role");
            require(lv_obj_get_style_text_letter_space(label, 0) == orb_text_style_info(role)->letter_space,
                    "local tracking overrides semantic role");
            require(lv_obj_get_style_text_line_space(label, 0) == orb_text_line_space(role, flags),
                    "local leading overrides semantic role");
            require(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "Bold Text unavailable");
            require(lv_obj_get_style_text_font(label, 0) == orb_font_variant(role, flags),
                    "text role did not follow live Bold Text");
            require(lv_obj_get_style_text_font(label, 0) != normal, "Bold Text retained regular face");
            lv_obj_delete(label);
            require(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "regular text unavailable");
        }
    }
}

typedef struct { unsigned lines; bool widow; } wrap_t;

static wrap_t wrapped(const char *text, const lv_font_t *font, int32_t tracking, int32_t width, bool avoid_widow)
{
    lv_text_attributes_t attributes = {
        .letter_space=tracking, .max_width=width,
        .text_flags=avoid_widow ? LV_TEXT_FLAG_AVOID_WIDOW : LV_TEXT_FLAG_NONE,
    };
    wrap_t result = {0};
    while (*text) {
        unsigned length = lv_text_get_next_line(text, LV_TEXT_LEN_MAX, font, NULL, &attributes);
        if (!length) break;
        unsigned visible = length;
        while (visible && (text[visible - 1] == ' ' || text[visible - 1] == '\n')) visible--;
        result.widow = !memchr(text, ' ', visible);
        result.lines++;
        text += length;
    }
    return result;
}

static void balanced(lv_obj_t *root)
{
    const char *captions[] = {"A keepsake for you", "Hold to steer by touch", "New updates appear here",
                             "Increase label contrast", "Use opaque controls"};
    for (unsigned i = 0; i < sizeof(captions) / sizeof(captions[0]); i++) {
        lv_obj_t *label = lv_label_create(root);
        orb_text_apply(label, ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR, 0);
        const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
        int32_t tracking = lv_obj_get_style_text_letter_space(label, 0);
        int32_t lane = 0;
        for (int32_t width = 100; width < 400; width++) {
            wrap_t greedy = wrapped(captions[i], font, tracking, width, false);
            int32_t balanced = orb_kit_text_balance_width(captions[i], font, tracking, width);
            wrap_t after = wrapped(captions[i], font, tracking, balanced, true);
            if (greedy.lines == 2 && greedy.widow && after.lines == 2 && !after.widow) {
                lane = width;
                break;
            }
        }
        require(lane > 0, "caption could not avoid a one-word widow");
        lv_label_set_text(label, captions[i]);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(label, lane);
        orb_kit_label_balance(label);
        lv_obj_update_layout(label);
        require(lv_obj_get_width(label) == lane, "balancing moved caption lane");
        require(!strcmp(lv_label_get_text(label), captions[i]), "balancing changed semantic copy");
        require(!wrapped(captions[i], font, tracking, lv_obj_get_content_width(label), true).widow,
                "balanced label still has a one-word widow");
        lv_obj_set_width(label, 420);
        lv_obj_update_layout(label);
        require(lv_obj_get_style_pad_left(label, 0) == 0 && lv_obj_get_style_pad_right(label, 0) == 0,
                "widened caption retained obsolete balance padding");
        lv_obj_delete(label);
    }
}

static int32_t label_ink_bottom(lv_obj_t *label, bool first_line)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    const char *text = lv_label_get_text(label);
    int32_t bottom = 0;
    for (uint32_t offset = 0, index = 0; text[offset]; index++) {
        uint32_t cp = lv_text_encoded_next(text, &offset), next = offset;
        lv_point_t position;
        lv_label_get_letter_pos(label, index, &position);
        if (first_line && position.y) break;
        lv_font_glyph_dsc_t glyph;
        if (lv_font_get_glyph_dsc(font, &glyph, cp, lv_text_encoded_next(text, &next)) && glyph.box_h)
            bottom = LV_MAX(bottom, position.y + font->line_height - font->base_line - glyph.ofs_y);
    }
    return bottom;
}

static void readers(lv_obj_t *root)
{
    const lv_font_t *font = orb_font(ORB_TEXT_BODY);
    int32_t pitch = font->line_height + orb_text_line_space(ORB_TEXT_BODY, ORB_TEXT_REGULAR);
    int32_t requested = font->line_height + pitch / 2;
    lv_obj_t *reader = orb_kit_detail_page(root, &(orb_kit_detail_page_config_t){.top=80, .height=requested});
    orb_kit_paragraph(reader,
        "Read the complete message before choosing an action. Évery line stays readable while the remaining text scrolls below.",
        ORB_TEXT_BODY, false, 0, ORB_LABEL, false);
    int32_t complete = label_ink_bottom(lv_obj_get_child(reader, 0), true);
    require(orb_kit_reader_whole_lines(reader, requested) == complete + 1,
            "reader did not snap a partial second line to a complete first line");
    lv_refr_now(NULL);
    require(lv_obj_get_height(reader) == complete + 1, "detail page did not apply complete-line snap");
    lv_area_t bounds;
    lv_obj_get_coords(reader, &bounds);
    sim_display_rgb(s_pixels);
    unsigned edge_ink = 0;
    int32_t first_ink = -1;
    for (int32_t x = bounds.x1; x <= bounds.x2; x++) {
        size_t pixel = ((size_t)bounds.y2 * SIM_D + x) * 3;
        if (first_ink < 0 && (s_pixels[pixel] | s_pixels[pixel + 1] | s_pixels[pixel + 2])) first_ink = x;
        edge_ink |= s_pixels[pixel] | s_pixels[pixel + 1] | s_pixels[pixel + 2];
    }
    if (first_ink >= 0) {
        size_t pixel = ((size_t)bounds.y2 * SIM_D + first_ink) * 3;
        fprintf(stderr, "reader edge (%d,%d)=#%02x%02x%02x, ink_bottom=%d height=%d\n",
                first_ink, bounds.y2, s_pixels[pixel], s_pixels[pixel + 1], s_pixels[pixel + 2],
                complete, lv_obj_get_height(reader));
    }
    require(edge_ink == 0, "reader ends on painted glyph pixels instead of its following gap");
    int32_t top = 0, bottom = 24;
    orb_kit_reader_fades(reader, &top, &bottom);
    require(top == 0 && bottom <= 1, "fade obscures the reader's only complete line");
    int32_t settled = lv_obj_get_height(reader);
    lv_obj_scroll_to_y(reader, pitch, LV_ANIM_OFF);
    lv_refr_now(NULL);
    require(lv_obj_get_height(reader) == settled, "scrolling remeasured the resting reader geometry");
    lv_obj_delete(reader);

    /* A fitting third line must survive unused font descent. This reproduces
     * Dot's 200 px AX reader, with accented capitals and negative leading. */
    requested = font->line_height + 2 * pitch - 1;
    reader = orb_kit_detail_page(root, &(orb_kit_detail_page_config_t){.top=80, .height=requested});
    orb_kit_paragraph(reader, "Éggy\nHÉg\nAfter four", ORB_TEXT_BODY, false, 0, ORB_LABEL, false);
    lv_obj_t *label = lv_obj_get_child(reader, 0);
    lv_obj_update_layout(label);
    require(label_ink_bottom(label, false) <= requested && lv_obj_get_height(label) > requested,
            "three-line fixture does not exercise empty descent");
    require(orb_kit_reader_whole_lines(reader, requested) == requested,
            "empty font descent hid a complete fitting third line");
    lv_refr_now(NULL);
    require(lv_obj_get_height(reader) == requested, "fitting three-line viewport moved");
    top = 24; bottom = 24;
    orb_kit_reader_fades(reader, &top, &bottom);
    require(bottom <= requested - label_ink_bottom(label, false),
            "fade obscures a complete fitting last line");
    lv_obj_delete(reader);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    require(sim_display_create() != NULL, "display unavailable");
    orb_ui_init(NULL);
    for (unsigned size = 0; size < ORB_CONTENT_SIZE_COUNT; size++) {
        require(orb_fonts_set_content_size(size) == ORB_FONTS_OK, "content size unavailable");
        lv_obj_t *root = lv_obj_create(NULL);
        lv_obj_remove_style_all(root);
        lv_obj_set_style_bg_color(root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_set_size(root, 466, 466);
        lv_screen_load(root);
        roles(root);
        for (unsigned bold = 0; bold < 2; bold++) {
            require(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK, "font setting unavailable");
            balanced(root);
            readers(root);
        }
        require(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "regular text unavailable");
        lv_obj_clean(root);
    }
    printf("text roles: %u checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
