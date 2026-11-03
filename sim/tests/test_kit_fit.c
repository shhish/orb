/* Real Inter metrics, never ASCII width estimates. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "sim_display.h"
#include "src/misc/lv_text_private.h"

static unsigned s_checks;
static uint32_t s_now;
static uint32_t tick(void) { return s_now; }

static void require(bool valid, const char *message)
{
    s_checks++;
    if (!valid) {
        fprintf(stderr, "kit fit: %s (size=%d bold=%d)\n", message,
                orb_fonts_content_size(), orb_fonts_bold_text());
        exit(EXIT_FAILURE);
    }
}

static int32_t text_width(lv_obj_t *label)
{
    return lv_text_get_width(lv_label_get_text(label), strlen(lv_label_get_text(label)),
                              lv_obj_get_style_text_font(label, 0),
                              &(lv_text_attributes_t){
                                  .letter_space=lv_obj_get_style_text_letter_space(label, 0),
                                  .text_flags=LV_TEXT_FLAG_EXPAND,
                              });
}

static void ink(lv_obj_t *label, int32_t *top, int32_t *bottom)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    const unsigned char *text = (const unsigned char *)lv_label_get_text(label);
    *top = INT32_MAX; *bottom = INT32_MIN;
    for (unsigned i = 0; text[i]; i++) {
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, text[i], text[i + 1]) || !glyph.box_h) continue;
        int32_t y = lv_obj_get_y(label) + font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        *top = LV_MIN(*top, y); *bottom = LV_MAX(*bottom, y + glyph.box_h - 1);
    }
}

static void line(lv_obj_t *label)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    /* Default sizes bound a lane to one line; AX3 wraps to at most two. */
    int32_t lines = lv_label_get_max_lines(label);
    int32_t limit = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? 2 : 1;
    require(lines >= 1 && lines <= limit, "lane permits a second line");
    require(lv_obj_get_height(label) == lines * font->line_height +
            (lines - 1) * lv_obj_get_style_text_line_space(label, 0), "lane is not bounded to one line");
    require(orb_fonts_nominal_px(font) >= 21, "lane shrunk below HIG minimum style");
    if (lines == 1) require(text_width(label) <= lv_obj_get_width(label), "ellipsis still exceeds lane");
    int32_t top, bottom; ink(label, &top, &bottom);
    require(top >= 0 && bottom < lv_obj_get_height(lv_obj_get_parent(label)), "glyph ink escapes72px row");
}

static void rows(lv_obj_t *root)
{
    const orb_kit_row_config_t fixtures[] = {
        {.title="Repeat", .value="Weekdays"},
        {.title="Reading Room & Family Library", .subtitle="Disconnected from hub", .value="No Response"},
        {.title="Living Room Floor Lamp", .subtitle="Last updated yesterday", .value="Last known"},
        {.title="Pick up prescription before closing", .subtitle="Wednesday, October28 at12:59PM"},
        {.title="Lap16", .subtitle="00:28.88 split", .value="02:59:59.99"},
        {.title="Accessibility", .subtitle="Reduce Transparency"},
        {.title="A very long name with an ordinary value", .subtitle="A long secondary lane", .value="4"},
    };
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, SIM_D, SIM_D);
    for (unsigned i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        orb_kit_row_config_t config = fixtures[i];
        config.glyph = ORB_ICON_GEAR; config.accent = ORB_KIT_GREY;
        lv_obj_t *row = orb_kit_row_create(list, &config);
        lv_obj_update_layout(row);
        lv_obj_t *title = lv_obj_get_child(row, 1);
        line(title);
        lv_obj_t *subtitle = config.subtitle ? lv_obj_get_child(row, 2) : NULL;
        lv_obj_t *value = config.value ? lv_obj_get_child(row, subtitle ? 3 : 2) : NULL;
        if (subtitle) {
            line(subtitle);
            int32_t tt, tb, st, sb; ink(title, &tt, &tb); ink(subtitle, &st, &sb);
            require(tb + 2 < st, "title and subtitle glyph ink overlaps");
        }
        /* At AX3 a value that cannot sit beside its title stacks under it. */
        bool stacked = value && lv_obj_get_style_text_align(value, 0) == LV_TEXT_ALIGN_LEFT;
        if (value) {
            line(value);
            if (stacked) {
                int32_t tt, tb, vt, vb; ink(subtitle ? subtitle : title, &tt, &tb); ink(value, &vt, &vb);
                require(orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 && tb + 2 < vt,
                        "stacked value overlaps its title");
            } else {
                require(lv_obj_get_x(title) + lv_obj_get_width(title) + 12 <= lv_obj_get_x(value),
                        "title/value lanes overlap");
                if (subtitle) require(lv_obj_get_x(subtitle) + lv_obj_get_width(subtitle) + 12 <=
                                      lv_obj_get_x(value), "subtitle/value lanes overlap");
            }
            if (i < 3) require(!strcmp(lv_label_get_text(value), config.value),
                               "real trailing state was unnecessarily ellipsised");
        }
        if (i == 4) {
            require(orb_kit_row_set_text_role(row, ORB_KIT_ROW_TEXT_VALUE, ORB_TEXT_TABULAR), "tabular role rejected");
            const lv_font_t *font = lv_obj_get_style_text_font(value, 0);
            require(lv_font_get_glyph_width(font, '1', 0) == lv_font_get_glyph_width(font, '8', 0),
                    "duration figures are not tabular");
            orb_kit_row_set_value(row, "1");
            require(!strcmp(lv_label_get_text(value), "1") && lv_obj_get_width(value) < 92,
                    "short value did not release its rail");
            orb_kit_row_set_value(row, "No Response");
            require(!strcmp(lv_label_get_text(value), "No Response") &&
                    (lv_obj_get_width(value) > 92 || lv_obj_get_style_text_align(value, 0) == LV_TEXT_ALIGN_LEFT),
                    "updated real value kept fixed92px rail");
        }
        lv_obj_delete(row);
    }
    lv_obj_delete(list);
}

static void heroes(lv_obj_t *root)
{
    lv_obj_t *label = lv_label_create(root);
    const char *name = "Reading Room & Family Library";
    orb_kit_text_fit_info_t info;
    require(orb_kit_hero_fit(label, name, 220, 3, &info), "real room name needs more than220px/3lines");
    require(info.complete && info.lines <= 3 && !strcmp(lv_label_get_text(label), name),
            "hero lost room identity");
    require(lv_obj_get_width(label) == 220 && lv_obj_get_height(label) > 0,
            "hero budget was not applied");
    require(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_WRAP, "hero uses clipped/ellipsis mode");
    require(!orb_kit_hero_fit(label, name, 1, 2, &info) && !info.complete &&
            !strcmp(lv_label_get_text(label), name), "impossible hero budget silently hid text");
    /* Every32byte printableASCII candidate is rendered completely. Wider
     * caller budgets must be explicit rather than shrinkingAX3 belowCaption2. */
    unsigned minimum_width = 0;
    char worst = 0;
    for (unsigned cp = 32; cp < 127; cp++) {
        char text[33]; memset(text, cp, 32); text[32] = 0;
        unsigned width = 1;
        while (width < 466 && !orb_kit_hero_fit(label, text, width, 3, &info)) width++;
        require(width < 466 && info.complete && info.lines <= 3, "32byte room identity cannot fit within device width");
        require(!strcmp(lv_label_get_text(label), text), "32byte hero name was modified");
        if (width > minimum_width) { minimum_width = width; worst = cp; }
    }
    char worst_name[33]; memset(worst_name, worst, 32); worst_name[32] = 0;
    require(orb_kit_hero_fit(label, worst_name, minimum_width, 3, &info), "worst32byte measured name no longer fits");
    printf("hero32 size=%d bold=%d worst=%c minimum_width=%u lines=%u style=%d\n",
           orb_fonts_content_size(), orb_fonts_bold_text(), worst, minimum_width, info.lines, info.style);
    require(orb_kit_hero_fit(label, "Guest Suite Bath", 220, 3, &info), "ordinary room did not fit");
    require(orb_kit_hero_fit_info(label, &info) && info.complete, "hero info unavailable");
    lv_obj_delete(label);
}

static void middle_names(lv_obj_t *root)
{
    lv_obj_t *label = lv_label_create(root);
    orb_text_apply(label, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED, 0);
    lv_obj_set_width(label, 168);
    require(orb_kit_label_middle_ellipsis(label, "Desk"), "short identity failed");
    require(!strcmp(lv_label_get_text(label), "Desk"), "short identity changed");
    const char *names[] = {"Guest Suite Bath ceiling East", "Guest Suite Bath ceiling West",
                            "Café bibliothèque fenêtre Est"};
    const char *suffixes[] = {"East", "West", "Est"};
    for (unsigned i = 0; i < 3; i++) {
        require(orb_kit_label_middle_ellipsis(label, names[i]), "identity cannot fit fixed lane");
        const char *shown = lv_label_get_text(label);
        size_t length = strlen(shown), suffix = strlen(suffixes[i]);
        require(length >= suffix && !strcmp(shown + length - suffix, suffixes[i]),
                "distinguishing identity suffix clipped");
        require(strstr(shown, "…") && text_width(label) <= 168, "middle elision not bounded");
        uint32_t offset = 0;
        while (shown[offset]) require(lv_text_encoded_next(shown, &offset) != 0,
                                      "middle elision split a UTF-8 character");
    }
    lv_obj_delete(label);
}

static void live_fonts(lv_obj_t *root)
{
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "default fonts unavailable");
    require(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "regular fonts unavailable");
    lv_obj_t *label = lv_label_create(root);
    orb_kit_text_fit_info_t before, after;
    require(orb_kit_hero_fit(label, "Reading Room & Family Library", 220, 3, &before), "default live hero failed");
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "AX3 live fonts unavailable");
    require(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "bold live fonts unavailable");
    require(orb_kit_hero_fit_info(label, &after) && after.complete && after.lines <= 3,
            "liveAX3/Bold change did not refit hero");
    require(lv_obj_get_style_text_font(label, 0) == orb_font_variant(after.style, ORB_TEXT_EMPHASIZED),
            "live hero retains stale font");
    lv_obj_delete(label);
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    require(sim_display_create() != NULL, "display unavailable");
    orb_ui_init(NULL);
    lv_obj_t *root = lv_screen_active(); lv_obj_set_scrollable(root, false);
    for (unsigned mode = 0; mode < 3; mode++) {
        require(orb_fonts_set_content_size(mode ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
                "font mode unavailable");
        require(orb_fonts_set_bold_text(mode == 2) == ORB_FONTS_OK, "bold mode unavailable");
        orb_ui_set_reduce_transparency(mode == 2);
        rows(root); heroes(root); middle_names(root);
    }
    live_fonts(root);
    printf("kit fit: %u checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
