#include "orb_fonts_selftest.h"

#include <stdio.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_fonts_coverage.h"

static int failures;

#define CHECK(cond, ...)                                                                                 \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            failures++;                                                                                  \
            if (failures <= 40) {                                                                        \
                printf("orb_fonts FAIL: ");                                                              \
                printf(__VA_ARGS__);                                                                     \
                printf("\n");                                                                            \
            }                                                                                            \
        }                                                                                                \
    } while (0)

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const char *const size_names[ORB_CONTENT_SIZE_COUNT] = {"Default", "Larger", "AX3"};

static const uint32_t icon_codepoints[] = {
#define ICON_CP(name, cp) cp,
    ORB_ICONS_FOREACH(ICON_CP)
#undef ICON_CP
};

/* The font that draws `cp` when a label uses `font`, or NULL (missing or placeholder). */
static const lv_font_t *resolve(const lv_font_t *font, uint32_t cp, lv_font_glyph_dsc_t *g)
{
    lv_font_glyph_dsc_t tmp;
    if (!g) {
        g = &tmp;
    }
    memset(g, 0, sizeof(*g));
    if (!lv_font_get_glyph_dsc(font, g, cp, 0) || g->is_placeholder) {
        return NULL;
    }
    return g->resolved_font ? g->resolved_font : font;
}

static void check_equal_widths(const char *what, const lv_font_t *font, const uint32_t *cps, size_t n)
{
    uint16_t w0 = lv_font_get_glyph_width(font, cps[0], 0);
    for (size_t i = 1; i < n; i++) {
        uint16_t w = lv_font_get_glyph_width(font, cps[i], 0);
        CHECK(w == w0, "%s: U+%04X is %u px wide, U+%04X %u px", what, (unsigned)cps[i], w, (unsigned)cps[0], w0);
    }
}

/* Top of the ink above the baseline. */
static int32_t ink_top(const lv_font_t *font, uint32_t cp)
{
    lv_font_glyph_dsc_t g;
    return resolve(font, cp, &g) ? g.box_h + g.ofs_y : -1000;
}

static void check_face(const orb_text_style_info_t *info, orb_text_style_t style, orb_text_variant_t flags,
                       const char *set)
{
    char what[96];
    snprintf(what, sizeof(what), "%s, %s %s", set, info->name, (flags & ORB_TEXT_EMPHASIZED) ? "emphasized" : "regular");
    const lv_font_t *text = orb_font_variant(style, flags);
    const lv_font_t *tabular = orb_font_variant(style, flags | ORB_TEXT_TABULAR);
    const lv_font_t *icons = orb_icon_font_variant(style, flags);
    CHECK(text && tabular && icons, "%s: missing font", what);
    if (!text || !tabular || !icons) {
        return;
    }

    for (size_t i = 0; i < COUNT(orb_test_text_codepoints); i++) {
        uint32_t cp = orb_test_text_codepoints[i];
        CHECK(resolve(text, cp, NULL) == text, "%s: no U+%04X", what, (unsigned)cp);
        CHECK(resolve(tabular, cp, NULL) != NULL, "%s tabular: no U+%04X", what, (unsigned)cp);
    }
    for (size_t i = 0; i < COUNT(icon_codepoints); i++) {
        uint32_t cp = icon_codepoints[i];
        CHECK(resolve(text, cp, NULL) == icons, "%s: icon U+%04X not inline", what, (unsigned)cp);
        CHECK(resolve(icons, cp, NULL) == icons, "%s icons: no U+%04X", what, (unsigned)cp);
    }
    for (size_t i = 0; i < COUNT(orb_test_tabular_codepoints); i++) {
        uint32_t cp = orb_test_tabular_codepoints[i];
        CHECK(resolve(tabular, cp, NULL) == tabular, "%s: U+%04X not tabular", what, (unsigned)cp);
    }
    static const uint32_t digits[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 0x2007};
    check_equal_widths(what, tabular, digits, COUNT(digits));
    /* After a word ':' sits on the baseline like the text colon; the time separator is
     * raised; '-' is the text hyphen, not a figure-wide one. */
    CHECK(ink_top(tabular, ':') == ink_top(text, ':'), "%s: tabular ':' is raised", what);
    CHECK(ink_top(tabular, 0x2236) > ink_top(tabular, ':'), "%s: ORB_TIME_SEP isn't raised", what);
    CHECK(resolve(tabular, '-', NULL) == text, "%s: '-' comes from the tabular font", what);

    /* Tabular fonts carry their text font's line box; the API's line space gives Apple's leading. */
    CHECK(lv_font_get_line_height(tabular) == lv_font_get_line_height(text), "%s: tabular line box", what);
    int32_t pitch = lv_font_get_line_height(text) + orb_text_line_space(style, flags);
    CHECK(pitch == info->leading_px, "%s: line pitch %d, leading %d", what, (int)pitch, info->leading_px);

    /* Icon-only fonts: the icons' visual centre (half the text's cap height above the
     * baseline) is the middle of the line box, so centring the label centres the icon. */
    lv_font_glyph_dsc_t g, h;
    if (resolve(icons, icon_codepoints[0], &g) && resolve(text, 'H', &h)) {
        int32_t lh = lv_font_get_line_height(icons);
        int32_t box_mid_above_baseline = lh / 2 - icons->base_line;
        int32_t cap_mid = (h.box_h + 2 * h.ofs_y) / 2;
        CHECK(LV_ABS(box_mid_above_baseline - cap_mid) <= 1, "%s icons: line box centre %d, cap centre %d", what,
              (int)box_mid_above_baseline, (int)cap_mid);
    }
    /* A filled icon covers its outline: same ink box to the pixel, so swapping moves nothing. */
#define FILL_CP(name, cp)                                                                                           \
    if ((cp) >= 0xE100) {                                                                                           \
        lv_font_glyph_dsc_t o, f;                                                                                   \
        if (resolve(icons, (cp) - 0x100, &o) && resolve(icons, (cp), &f)) {                                         \
            CHECK(LV_ABS(o.ofs_x - f.ofs_x) <= 1 && LV_ABS(o.ofs_y - f.ofs_y) <= 1 &&                               \
                      LV_ABS(o.box_w - f.box_w) <= 1 && LV_ABS(o.box_h - f.box_h) <= 1 && o.adv_w == f.adv_w,         \
                  "%s: " #name " (%dx%d at %d,%d) doesn't cover its outline (%dx%d at %d,%d)", what, f.box_w, f.box_h, \
                  f.ofs_x, f.ofs_y, o.box_w, o.box_h, o.ofs_x, o.ofs_y);                                             \
        }                                                                                                           \
    }
    ORB_ICONS_FOREACH(FILL_CP)
#undef FILL_CP
}

static void check_set(const char *set)
{
    for (int s = 0; s < ORB_TEXT_STYLE_COUNT; s++) {
        const orb_text_style_info_t *info = orb_text_style_info((orb_text_style_t)s);
        CHECK(info->size_px > 0 && info->leading_px > info->size_px, "%s, style %d: metrics", set, s);
        check_face(info, (orb_text_style_t)s, ORB_TEXT_REGULAR, set);
        check_face(info, (orb_text_style_t)s, ORB_TEXT_EMPHASIZED, set);
        CHECK(orb_font((orb_text_style_t)s) == orb_font_variant((orb_text_style_t)s, 0), "%s: orb_font", info->name);
        CHECK(orb_icon_font((orb_text_style_t)s) == orb_icon_font_variant((orb_text_style_t)s, ORB_TEXT_TABULAR),
              "%s: icon variant", info->name);
    }
    /* Larger styles are never smaller. */
    for (int s = 1; s < ORB_TEXT_STYLE_COUNT; s++) {
        CHECK(orb_text_style_info((orb_text_style_t)s)->size_px <= orb_text_style_info((orb_text_style_t)(s - 1))->size_px,
              "%s: style %d is bigger than style %d", set, s, s - 1);
    }
    for (int c = 0; c < ORB_CLOCK_COUNT; c++) {
        const lv_font_t *font = orb_clock_font((orb_clock_t)c);
        for (size_t i = 0; i < COUNT(orb_test_clock_codepoints); i++) {
            uint32_t cp = orb_test_clock_codepoints[i];
            CHECK(resolve(font, cp, NULL) == font, "%s, clock %d: no U+%04X", set, c, (unsigned)cp);
        }
        static const uint32_t digits[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
        check_equal_widths("clock", font, digits, COUNT(digits));
        CHECK(ink_top(font, ':') == ink_top(font, 0x2236), "%s, clock %d: ':' and ORB_TIME_SEP differ", set, c);
    }
}

static uint32_t lit_pixels;

static void count_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t n = (uint32_t)lv_area_get_size(area);
    uint32_t bpp = lv_color_format_get_size(lv_display_get_color_format(disp));
    uint32_t colour_bytes = bpp == 4 ? 3 : bpp; /* skip the X/alpha byte of 32-bit formats */
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t b = 0; b < colour_bytes; b++) {
            if (px_map[i * bpp + b]) {
                lit_pixels++;
                break;
            }
        }
    }
    lv_display_flush_ready(disp);
}

static const char *const lines[] = {
    ORB_ICON_TERMINAL " Sessions " ORB_ICON_CHEVRON_RIGHT,
    "Needs your approval",
    "The quick brown fox",
    "jumps over the lazy dog.",
    "Été — “café” … 3½ × 2 ≈ 7 €",
    ORB_ICON_MIC " 12" ORB_TIME_SEP "45 " ORB_ICON_WIFI_HIGH " " ORB_ICON_BATTERY_CHARGING,
};

static void render(lv_display_t *disp, uint64_t (*now_us)(void), const char *set)
{
    lv_display_set_flush_cb(disp, count_flush);
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_screen_load(scr);

    printf("orb_fonts render (%s):", set);
    for (int s = 0; s < ORB_TEXT_STYLE_COUNT; s++) {
        for (int v = 0; v < 4; v++) {
            lv_obj_clean(scr);
            int32_t y = 0;
            for (size_t i = 0; y < 466; i = (i + 1) % COUNT(lines)) {
                lv_obj_t *l = lv_label_create(scr);
                orb_text_apply(l, (orb_text_style_t)s, (orb_text_variant_t)v, 0);
                lv_label_set_text_static(l, lines[i]);
                lv_obj_set_pos(l, 8, y);
                y += orb_text_style_info((orb_text_style_t)s)->leading_px;
            }
            lv_obj_invalidate(scr);
            lit_pixels = 0;
            uint64_t t0 = now_us();
            lv_refr_now(disp);
            uint64_t us = now_us() - t0;
            CHECK(lit_pixels > 0, "%s: %s/%d rendered nothing", set, orb_text_style_info((orb_text_style_t)s)->name, v);
            if (v == 0) {
                printf(" %s %u us%s", orb_text_style_info((orb_text_style_t)s)->name, (unsigned)us,
                       s + 1 < ORB_TEXT_STYLE_COUNT ? "," : "");
            }
        }
    }
    printf("\n");
    for (int c = 0; c < ORB_CLOCK_COUNT; c++) {
        lv_obj_clean(scr);
        lv_obj_t *l = lv_label_create(scr);
        orb_clock_apply(l, (orb_clock_t)c, 0);
        lv_label_set_text_static(l, "10" ORB_TIME_SEP "09");
        lv_obj_center(l);
        lit_pixels = 0;
        uint64_t t0 = now_us();
        lv_refr_now(disp);
        printf("orb_fonts render (%s) clock %d: %u us\n", set, c, (unsigned)(now_us() - t0));
        CHECK(lit_pixels > 0, "%s: clock %d rendered nothing", set, c);
    }
    lv_obj_clean(scr);
}

/* Labels styled through the API follow content size and Bold Text changes. */
static void check_dynamic_type(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *body = lv_label_create(scr);
    orb_text_apply(body, ORB_TEXT_BODY, ORB_TEXT_REGULAR, 0);
    orb_text_apply(body, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0); /* replaces, doesn't stack */
    lv_label_set_text_static(body, "Needs your approval");
    lv_obj_t *clock = lv_label_create(scr);
    orb_clock_apply(clock, ORB_CLOCK_MEDIUM, 0);
    int32_t before = lv_subject_get_int(orb_fonts_subject());

    for (int size = 0; size < ORB_CONTENT_SIZE_COUNT; size++) {
        for (int bold = 0; bold < 2; bold++) {
            if (orb_fonts_set_content_size((orb_content_size_t)size) != ORB_FONTS_OK ||
                orb_fonts_set_bold_text(bold) != ORB_FONTS_OK) {
                continue;
            }
            lv_obj_update_layout(scr);
            CHECK(lv_obj_get_style_text_font(body, 0) == orb_font(ORB_TEXT_HEADLINE), "label didn't follow %s%s",
                  size_names[size], bold ? " Bold Text" : "");
            CHECK(lv_obj_get_style_text_line_space(body, 0) == orb_text_line_space(ORB_TEXT_HEADLINE, 0),
                  "label's line space didn't follow");
            CHECK(lv_obj_get_style_text_font(clock, 0) == orb_clock_font(ORB_CLOCK_MEDIUM), "clock didn't follow");
            CHECK(orb_clock_info(ORB_CLOCK_MEDIUM)->weight == (bold ? 700 : 600), "clock weight with Bold Text");
            CHECK(orb_text_style_info(ORB_TEXT_HEADLINE)->weight == (bold ? 700 : 600), "Headline weight with Bold Text");
        }
    }
    orb_fonts_set_bold_text(false);
    orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT);
    CHECK(lv_subject_get_int(orb_fonts_subject()) != before || orb_fonts_set_bold_text(true) != ORB_FONTS_OK,
          "orb_fonts_subject() didn't notify");
    orb_fonts_set_bold_text(false);
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_COUNT) == ORB_FONTS_ERR_ARG, "bad content size accepted");
    lv_obj_delete(body);
    lv_obj_delete(clock);
}

int orb_fonts_selftest(lv_display_t *disp, uint64_t (*now_us)(void), bool require_pack)
{
    failures = 0;
    const char *notices = orb_fonts_notices();
    CHECK(strstr(notices, "SIL OPEN FONT LICENSE") && strstr(notices, "Inter Project Authors") &&
              strstr(notices, "Phosphor Icons") && strstr(notices, "Framework7 Icons") &&
              strstr(notices, "MIT License"),
          "licence notices missing");
    int sets = 0;
    for (int size = 0; size < ORB_CONTENT_SIZE_COUNT; size++) {
        for (int bold = 0; bold < 2; bold++) {
            orb_fonts_err_t err = orb_fonts_set_content_size((orb_content_size_t)size);
            if (err == ORB_FONTS_OK) {
                err = orb_fonts_set_bold_text(bold);
            }
            char set[48];
            snprintf(set, sizeof(set), "%s%s", size_names[size], bold ? ", Bold Text" : "");
            if (err != ORB_FONTS_OK) {
                printf("orb_fonts: %s skipped: %s\n", set, orb_fonts_err_name(err));
                CHECK(!require_pack, "%s unavailable: %s", set, orb_fonts_err_name(err));
                orb_fonts_set_bold_text(false);
                continue;
            }
            CHECK(orb_fonts_content_size() == (orb_content_size_t)size && orb_fonts_bold_text() == bold, "%s: setting", set);
            check_set(set);
            if (disp && now_us) {
                render(disp, now_us, set);
            }
            sets++;
        }
    }
    orb_fonts_set_bold_text(false);
    orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT);
    check_dynamic_type();
    printf("orb_fonts self-test: %s (%d of 6 sets, %d failures)\n", failures ? "FAILED" : "passed", sets, failures);
    return failures;
}
