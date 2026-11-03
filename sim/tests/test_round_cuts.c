#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_round_source.h"
#include "orb_ui_private.h"
#include "src/misc/lv_text_private.h"
#include "sim_display.h"

static unsigned s_checks, s_allocations, s_fail_on;
static bool s_failed;
#define CHECK(value) do { ++s_checks; if (!(value)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); } } while (0)

static void *allocate(size_t size)
{
    if (++s_allocations == s_fail_on) { s_failed = true; return NULL; }
    return lv_malloc(size);
}
static void release(void *pointer) { lv_free(pointer); }
static unsigned reference_meaningful(const char *text, unsigned length)
{
    unsigned count = 0;
    for (uint32_t offset = 0; offset < length;) {
        uint32_t cp = lv_text_encoded_next(text, &offset);
        if (cp != '\n' && cp != '\r' && !lv_label_trims_before_ellipsis(cp) &&
            (cp < 0xE000 || cp > 0xF8FF)) count++;
    }
    return count;
}
static void reference_cuts(round_chord_child_t *c)
{
    const char *text = lv_label_get_text(c->obj);
    const lv_font_t *font = lv_obj_get_style_text_font(c->obj, 0);
    lv_text_attributes_t attributes = {
        .letter_space = lv_obj_get_style_text_letter_space(c->obj, 0),
        .line_space = lv_obj_get_style_text_line_space(c->obj, 0), .max_width = c->width,
    };
    c->line_height = LV_MAX(1, font->line_height + attributes.line_space);
    c->lines = LV_CLAMP(1, (c->height + attributes.line_space) / c->line_height, CHORD_TEXT_LINES);
    c->cut = orb_ui_alloc(CHORD_TEXT_BUCKETS * c->lines * sizeof(*c->cut));
    if (!c->cut) return;
    memset(c->cut, 0xff, CHORD_TEXT_BUCKETS * c->lines * sizeof(*c->cut));
    unsigned shown[CHORD_TEXT_BUCKETS] = {0};
    bool truncated[CHORD_TEXT_BUCKETS] = {0}, missing[CHORD_TEXT_BUCKETS] = {0};
    unsigned first_length = lv_text_get_next_line(text, LV_TEXT_LEN_MAX, font, NULL, &attributes);
    bool multiline = c->lines > 1 && text[first_length];
    unsigned start = 0;
    for (unsigned line = 0; line < c->lines && text[start]; line++) {
        unsigned length = lv_text_get_next_line(text + start, LV_TEXT_LEN_MAX, font, NULL, &attributes);
        if (!length) break;
        int full = lv_text_get_line_width(text + start, length, font, &attributes);
        for (unsigned b = 0; b < CHORD_TEXT_BUCKETS; b++) {
            int available = LV_MAX(0, c->width - (int)b * ROUND_CHORD_WIDTH_STEP);
            if (full <= available) { shown[b] += reference_meaningful(text + start, length); continue; }
            int room = LV_MAX(0, available - (int)c->dots->header.w);
            unsigned end = 0, meaningful = 0, kept = 0;
            int cut = ORB_ROUND_CUT_HIDDEN;
            /* Measure each physical line's UTF-8/kerning once. A wrapped AX3
             * title must never acquire a spurious single-line ellipsis. */
            while (end < length && text[start + end] && text[start + end] != '\n') {
                unsigned next = end + 1;
                while (next < length && ((unsigned char)text[start + next] & 0xc0) == 0x80) next++;
                /* Glossary §1.3: never a space or separator before the
                 * ellipsis, and never a sliver of the first dropped glyph. */
                uint32_t previous = next;
                uint32_t cp = lv_text_encoded_prev(text + start, &previous);
                bool trimmed = lv_label_trims_before_ellipsis(cp);
                if (cp != '\n' && cp != '\r' && !trimmed && (cp < 0xE000 || cp > 0xF8FF)) meaningful++;
                int w = lv_text_get_width(text + start, next, font, &attributes);
                if (next < length && text[start + next] && text[start + next] != '\n') {
                    uint32_t at = next, after;
                    uint32_t letter = lv_text_encoded_next(text + start, &at);
                    after = at;
                    uint32_t following = at < length ? lv_text_encoded_next(text + start, &after) : 0;
                    lv_font_glyph_dsc_t glyph;
                    if (lv_font_get_glyph_dsc(font, &glyph, letter, following) && glyph.box_w && glyph.ofs_x < 0)
                        w += glyph.ofs_x;
                }
                w = LV_MAX(0, w);
                end = next;
                if (trimmed || meaningful < (multiline ? 1u : 4u)) continue;
                if (w > room) break;
                cut = w;
                kept = meaningful;
            }
            c->cut[b * c->lines + line] = cut;
            shown[b] += kept;
            truncated[b] = true;
            missing[b] |= cut == ORB_ROUND_CUT_HIDDEN;
        }
        start += length;
    }
    if (multiline) for (unsigned b = 0; b < CHORD_TEXT_BUCKETS; b++) {
        if (!truncated[b] || (!missing[b] && shown[b] >= 4)) continue;
        for (unsigned line = 0; line < c->lines; line++) c->cut[b * c->lines + line] = ORB_ROUND_CUT_HIDDEN;
    }
}

static void compare_cuts(lv_obj_t *label, int width, unsigned lines, int dots_width, unsigned fail_on)
{
    lv_draw_buf_t dots = {.header = {.w = dots_width}};
    round_chord_child_t reference = {.obj = label, .width = width,
        .height = lines * lv_obj_get_style_text_font(label, 0)->line_height, .dots = &dots};
    round_chord_child_t candidate = reference;
    s_allocations = s_fail_on = 0;
    reference_cuts(&reference);
    CHECK(reference.cut);
    s_allocations = 0;
    s_fail_on = fail_on;
    s_failed = false;
    orb_round_source_cuts(&candidate);
    CHECK(candidate.cut && reference.lines == candidate.lines && reference.line_height == candidate.line_height);
    CHECK(!memcmp(reference.cut, candidate.cut, CHORD_TEXT_BUCKETS * reference.lines * sizeof(*reference.cut)));
    if (fail_on) CHECK(s_failed);
    s_fail_on = 0;
    orb_ui_free(reference.cut);
    orb_ui_free(candidate.cut);
}

/* Semantic assertions, separate from the width oracle: narrowing one line
 * must never discard its ink while leaving a detached continuation visible. */
static void wrapped_preview(lv_obj_t *label)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    lv_text_attributes_t attributes = {.letter_space = lv_obj_get_style_text_letter_space(label, 0)};
    lv_draw_buf_t dots = {.header = {.w = lv_font_get_glyph_width(font, 0x2026, 0)}};
    int height = 2 * font->line_height + lv_obj_get_style_text_line_space(label, 0);
    const char *texts[] = {"Alarms still\nring", "WWWW\nilll", "Alarms still\nr", "A\nB"};
    for (unsigned sample = 0; sample < sizeof(texts) / sizeof(*texts); sample++) {
        lv_label_set_text(label, texts[sample]);
        round_chord_child_t child = {.obj = label, .width = 418, .height = height, .dots = &dots};
        orb_round_source_cuts(&child);
        CHECK(child.cut && child.lines == 2);
        CHECK(child.cut[0] == UINT16_MAX && child.cut[1] == UINT16_MAX);
        bool exercised = false;
        int first = lv_text_get_width(texts[sample], 1, font, &attributes);
        int three = lv_text_get_width(texts[sample], 3, font, &attributes);
        int four = lv_text_get_width(texts[sample], 4, font, &attributes);
        const char *last = strchr(texts[sample], '\n') + 1;
        int full = lv_text_get_width(last, strlen(last), font, &attributes);
        for (unsigned b = 0; b < CHORD_TEXT_BUCKETS; b++) {
            int available = LV_MAX(0, child.width - (int)b * ROUND_CHORD_WIDTH_STEP);
            uint16_t top = child.cut[b * child.lines], bottom = child.cut[b * child.lines + 1];
            if (sample == 0 && available >= full && available >= first + (int)dots.header.w &&
                available < four + (int)dots.header.w) {
                CHECK(top != ORB_ROUND_CUT_HIDDEN && top != UINT16_MAX);
                CHECK(top <= available - (int)dots.header.w && bottom == UINT16_MAX);
                exercised = true; /* Fewer than four prefix letters, plus the complete "ring". */
            } else if (sample == 1 && available >= full && available < first + (int)dots.header.w) {
                CHECK(top == ORB_ROUND_CUT_HIDDEN && bottom == ORB_ROUND_CUT_HIDDEN);
                exercised = true; /* No first-line prefix fits: hide the complete block. */
            } else if (sample == 2 && available >= first + (int)dots.header.w &&
                       available < three + (int)dots.header.w) {
                CHECK(top == ORB_ROUND_CUT_HIDDEN && bottom == ORB_ROUND_CUT_HIDDEN);
                exercised = true; /* A two-letter prefix plus "r" has too little identity. */
            } else if (sample == 3 && available >= LV_MAX(first, full)) {
                CHECK(top == UINT16_MAX && bottom == UINT16_MAX);
                exercised = true; /* Complete short text needs no truncation policy. */
            }
        }
        CHECK(exercised);
        orb_ui_free(child.cut);
    }
    /* Height reserves two lines, but this text occupies only one. Its
     * shoulder still needs the established four-character prefix. */
    lv_label_set_text(label, "Alarms still");
    round_chord_child_t tall = {.obj = label, .width = 418, .height = height, .dots = &dots};
    orb_round_source_cuts(&tall);
    CHECK(tall.cut && tall.lines == 2);
    int first = lv_text_get_width("Alarms still", 1, font, &attributes);
    int four = lv_text_get_width("Alarms still", 4, font, &attributes);
    bool exercised = false;
    for (unsigned b = 0; b < CHORD_TEXT_BUCKETS; b++) {
        int available = LV_MAX(0, tall.width - (int)b * ROUND_CHORD_WIDTH_STEP);
        if (available < first + (int)dots.header.w || available >= four + (int)dots.header.w) continue;
        CHECK(tall.cut[b * tall.lines] == ORB_ROUND_CUT_HIDDEN);
        CHECK(tall.cut[b * tall.lines + 1] == UINT16_MAX);
        exercised = true;
    }
    CHECK(exercised);
    orb_ui_free(tall.cut);
}

int main(void)
{
    lv_init();
    CHECK(sim_display_create());
    orb_ui_init(&(orb_ui_config_t){.alloc = allocate, .free = release});
    const char *texts[] = {
        "AVATAR To Wa would make a useful kerning example",
        "naïve façade • mañana — Á Ω 日本語 …",
        "First line\nSecond line with spaces \nThird line",
        "One\rTwo\r\nThree\tTabbed",
        "Wide prefix AV To with trailing spaces     ",
        "a\xe2\x80\x8b" "b\xcc\x81" "c", "", " ",
        "Living room \xc2\xb7 Wake on raise... Codex \xc2\xb7 71% used",
    };
    const int widths[] = {12, 24, 48, 96, 180, 310, 418};
    const int tracking[] = {-30, -3, 0, 2, 5};
    for (unsigned profile = 0; profile < 4; ++profile) {
        CHECK(orb_fonts_set_content_size(profile & 1 ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(profile & 2) == ORB_FONTS_OK);
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_obj_t *label = lv_label_create(screen);
        orb_text_apply(label, ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR, 0);
        wrapped_preview(label);
        for (unsigned t = 0; t < sizeof(texts) / sizeof(texts[0]); ++t) {
            lv_label_set_text(label, texts[t]);
            for (unsigned s = 0; s < sizeof(tracking) / sizeof(tracking[0]); ++s) {
                lv_obj_set_style_text_letter_space(label, tracking[s], 0);
                for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w)
                    for (unsigned lines = 1; lines <= 6; lines += 5)
                        compare_cuts(label, widths[w], lines, 17, 0);
            }
        }
        lv_obj_set_style_text_letter_space(label, 0, 0);
        lv_label_set_text(label, "A real allocation failure keeps these exact prefix cuts over several wrapped lines.");
        compare_cuts(label, 180, 6, 19, 2); /* The first transient memo fails; retained cuts succeed. */
        char long_line[401];
        memset(long_line, 'A', sizeof(long_line) - 1);
        long_line[400] = '\0';
        lv_label_set_text(label, long_line);
        lv_text_attributes_t attributes = {0};
        int full = lv_text_get_width(long_line, 400, lv_obj_get_style_text_font(label, 0), &attributes);
        compare_cuts(label, full + 100, 1, 19, 0);
        CHECK(s_allocations == 1); /* >256 prefixes uses scalar fallback without memo allocation. */
        lv_draw_buf_t dots = {.header = {.w = 17}};
        round_chord_child_t failed = {.obj = label, .width = 180, .height = 72, .dots = &dots};
        s_allocations = 0;
        s_fail_on = 1;
        orb_round_source_cuts(&failed);
        CHECK(!failed.cut && s_allocations == 1);
        s_fail_on = 0;
        lv_obj_delete(screen);
    }
    printf("round cuts: %u exact checks passed\n", s_checks);
    lv_deinit();
    return 0;
}
