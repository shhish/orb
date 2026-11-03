#include "orb_round_source.h"
#include "orb_ui_private.h"

#include <string.h>
#include "src/misc/lv_text_private.h"

/* One physical line uses at most 1 KiB of transient PSRAM. Longer sources and
 * allocation failure retain the original scalar prefix measurement exactly. */
#define CHORD_PREFIX_LIMIT 256u

static unsigned chord_prefix_next(const char *text, unsigned end, unsigned length)
{
    unsigned next = end + 1;
    while (next < length && ((unsigned char)text[next] & 0xc0) == 0x80) next++;
    return next;
}

/* Marks a prefix that may not take the ellipsis. */
#define CHORD_CUT_SKIP INT32_MAX

static unsigned chord_prefix_meaningful(const char *text, unsigned end)
{
    unsigned meaningful = 0;
    for (uint32_t offset = 0; offset < end;) {
        uint32_t cp = lv_text_encoded_next(text, &offset);
        if (cp != '\n' && cp != '\r' && !lv_label_trims_before_ellipsis(cp) &&
            (cp < 0xE000 || cp > 0xF8FF)) meaningful++;
    }
    return meaningful;
}

/* Width kept in front of the ellipsis when the line is cut after `end` bytes.
 * HIG §4 / glossary §1.3: a cut never ends on a space or separator ("Living
 * …", "Wake o.…"), and it stops where the first dropped glyph's ink begins:
 * a negative left bearing would otherwise leave a sliver before the dots. */
static int32_t chord_prefix_cut(const char *text, unsigned end, unsigned length, const lv_font_t *font,
                                const lv_text_attributes_t *attributes, unsigned minimum)
{
    uint32_t previous = end;
    if (lv_label_trims_before_ellipsis(lv_text_encoded_prev(text, &previous))) return CHORD_CUT_SKIP;
    /* HIG §§4/7: a tiny shoulder preview such as R… carries no identity.
     * A wrapped label shares that budget across its complete visible lines. */
    if (chord_prefix_meaningful(text, end) < minimum) return CHORD_CUT_SKIP;
    int32_t width = lv_text_get_width(text, end, font, attributes);
    if (end < length && text[end] && text[end] != '\n') {
        uint32_t offset = end, lookahead;
        uint32_t letter = lv_text_encoded_next(text, &offset);
        lookahead = offset;
        uint32_t next = offset < length ? lv_text_encoded_next(text, &lookahead) : 0;
        lv_font_glyph_dsc_t glyph;
        if (lv_font_get_glyph_dsc(font, &glyph, letter, next) && glyph.box_w && glyph.ofs_x < 0)
            width += glyph.ofs_x;
    }
    return LV_MAX(0, width);
}

static int32_t *chord_prefix_widths(const char *text, unsigned length, const lv_font_t *font,
                                    const lv_text_attributes_t *attributes, unsigned minimum, unsigned *count)
{
    *count = 0;
    for (unsigned end = 0; end < length && text[end] && text[end] != '\n';) {
        if (++*count > CHORD_PREFIX_LIMIT) return NULL;
        end = chord_prefix_next(text, end, length);
    }
    if (!*count) return NULL;
    int32_t *widths = orb_ui_alloc(*count * sizeof(*widths));
    if (!widths) return NULL;
    unsigned end = 0;
    for (unsigned i = 0; i < *count; ++i) {
        end = chord_prefix_next(text, end, length);
        /* Keep LVGL's actual next-glyph/kerning and tracking semantics. The
         * length limits iteration; the decoder can still look ahead to the
         * following character exactly as the original per-bucket calls do. */
        widths[i] = chord_prefix_cut(text, end, length, font, attributes, minimum);
    }
    return widths;
}

static int32_t chord_cut_scalar(const char *text, unsigned length, const lv_font_t *font,
                                 const lv_text_attributes_t *attributes, int room, unsigned minimum,
                                 unsigned *visible)
{
    unsigned end = 0, kept = 0;
    int32_t cut = ORB_ROUND_CUT_HIDDEN;
    while (end < length && text[end] && text[end] != '\n') {
        end = chord_prefix_next(text, end, length);
        int32_t width = chord_prefix_cut(text, end, length, font, attributes, minimum);
        if (width == CHORD_CUT_SKIP) continue;
        if (width > room) break;
        cut = width;
        kept = end;
    }
    *visible = chord_prefix_meaningful(text, kept);
    return cut;
}

static int32_t chord_cut_memo(const int32_t *widths, unsigned count, const char *text, unsigned length,
                              int room, unsigned *visible)
{
    int32_t cut = ORB_ROUND_CUT_HIDDEN;
    unsigned end = 0, kept = 0;
    for (unsigned i = 0; i < count; ++i) {
        end = chord_prefix_next(text, end, length);
        if (widths[i] == CHORD_CUT_SKIP) continue;
        if (widths[i] > room) break;
        cut = widths[i];
        kept = end;
    }
    *visible = chord_prefix_meaningful(text, kept);
    return cut;
}

void orb_round_source_cuts(round_chord_child_t *c)
{
    const char *text = lv_label_get_text(c->obj);
    const lv_font_t *font = lv_obj_get_style_text_font(c->obj, 0);
    lv_text_attributes_t attributes = {
        .letter_space = lv_obj_get_style_text_letter_space(c->obj, 0),
        .line_space = lv_obj_get_style_text_line_space(c->obj, 0), .max_width = c->width,
    };
    c->line_height = LV_MAX(1, font->line_height + attributes.line_space);
    c->line_box = font->line_height;
    c->lines = LV_CLAMP(1, (c->height + attributes.line_space) / c->line_height, CHORD_TEXT_LINES);
    c->cut = orb_ui_alloc(CHORD_TEXT_BUCKETS * c->lines * sizeof(*c->cut));
    if (!c->cut) return;
    memset(c->cut, 0xff, CHORD_TEXT_BUCKETS * c->lines * sizeof(*c->cut));
    unsigned visible[CHORD_TEXT_BUCKETS] = {0};
    bool truncated[CHORD_TEXT_BUCKETS] = {0}, missing[CHORD_TEXT_BUCKETS] = {0};
    unsigned first_length = lv_text_get_next_line(text, LV_TEXT_LEN_MAX, font, NULL, &attributes);
    bool multiline = c->lines > 1 && text[first_length];
    unsigned minimum = multiline ? 1 : 4;
    unsigned start = 0;
    for (unsigned line = 0; line < c->lines && text[start]; line++) {
        unsigned length = line ? lv_text_get_next_line(text + start, LV_TEXT_LEN_MAX, font, NULL, &attributes) :
            first_length;
        if (!length) break;
        int full = lv_text_get_line_width(text + start, length, font, &attributes);
        unsigned count;
        int32_t *widths = chord_prefix_widths(text + start, length, font, &attributes, minimum, &count);
        unsigned meaningful = chord_prefix_meaningful(text + start, length);
        for (unsigned b = 0; b < CHORD_TEXT_BUCKETS; b++) {
            int available = LV_MAX(0, c->width - (int)b * ROUND_CHORD_WIDTH_STEP);
            if (full <= available) { visible[b] += meaningful; continue; }
            int room = LV_MAX(0, available - (int)c->dots->header.w);
            unsigned kept;
            uint16_t cut = widths ? chord_cut_memo(widths, count, text + start, length, room, &kept) :
                chord_cut_scalar(text + start, length, font, &attributes, room, minimum, &kept);
            c->cut[b * c->lines + line] = cut;
            visible[b] += kept;
            truncated[b] = true;
            missing[b] |= cut == ORB_ROUND_CUT_HIDDEN;
        }
        orb_ui_free(widths);
        start += length;
    }
    /* HIG §§4/7: never orphan a continuation (an empty line above "ring").
     * Keep shorter complete prefixes when the whole wrapped preview still
     * carries identity; otherwise suppress the label as one semantic block.
     * This is immutable preparation, so scrolling never typesets a line. */
    if (multiline) for (unsigned b = 0; b < CHORD_TEXT_BUCKETS; b++) {
        if (!truncated[b] || (!missing[b] && visible[b] >= 4)) continue;
        for (unsigned line = 0; line < c->lines; line++) c->cut[b * c->lines + line] = ORB_ROUND_CUT_HIDDEN;
    }
}

/* One prepared ellipsis per actual font/ink combination, shared by visible
 * rows. Keeping one ARGB glyph per child dominated small list sources. */
typedef struct chord_ellipsis {
    struct chord_ellipsis *next;
    const lv_font_t *font;
    lv_color_t color;
    lv_opa_t opa;
    int32_t tracking;
    uint32_t font_revision;
    unsigned refs;
    lv_draw_buf_t *buffer;
} chord_ellipsis_t;
static chord_ellipsis_t *s_chord_ellipses;

void orb_round_text_dots_release(lv_draw_buf_t *dots)
{
    if (!dots) return;
    chord_ellipsis_t **slot = &s_chord_ellipses;
    while (*slot && (*slot)->buffer != dots) slot = &(*slot)->next;
    if (!*slot) return;
    chord_ellipsis_t *entry = *slot;
    if (--entry->refs) return;
    *slot = entry->next;
    lv_draw_buf_destroy(entry->buffer);
    orb_ui_free(entry);
}

lv_draw_buf_t *orb_round_text_dots(lv_obj_t *label)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    lv_color_t color = lv_obj_get_style_text_color(label, 0);
    lv_opa_t opa = lv_obj_get_style_text_opa(label, 0);
    int32_t tracking = lv_obj_get_style_text_letter_space(label, 0);
    /* A transparent lane (a live value's anchor) draws no ink, so it has no
     * ellipsis either; an opaque one over its live text read "7…1%". The dots
     * carry the lane's own text opacity, so secondary text keeps grey dots. */
    if (opa == LV_OPA_TRANSP) return NULL;
    for (chord_ellipsis_t *entry = s_chord_ellipses; entry; entry = entry->next)
        if (entry->font == font && lv_color_eq(entry->color, color) && entry->opa == opa &&
            entry->tracking == tracking && entry->font_revision == orb_fonts_revision()) {
            entry->refs++;
            return entry->buffer;
        }
    lv_obj_t *temp = lv_label_create(lv_obj_get_parent(label));
    lv_obj_remove_style_all(temp);
    lv_obj_set_size(temp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_label_set_long_mode(temp, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(temp, lv_obj_get_style_text_font(label, 0), 0);
    lv_obj_set_style_text_letter_space(temp, lv_obj_get_style_text_letter_space(label, 0), 0);
    lv_obj_set_style_text_color(temp, lv_obj_get_style_text_color(label, 0), 0);
    lv_obj_set_style_text_opa(temp, opa, 0);
    lv_label_set_text(temp, "…");
    lv_obj_update_layout(temp);
    lv_area_t area; lv_obj_get_coords(temp, &area);
    lv_draw_buf_t *dots = lv_draw_buf_create(lv_area_get_width(&area), lv_area_get_height(&area),
                                          LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if (dots && lv_snapshot_take_area_to_draw_buf(temp, LV_COLOR_FORMAT_ARGB8888, &area, dots) != LV_RESULT_OK) {
        lv_draw_buf_destroy(dots); dots = NULL;
    }
    lv_obj_delete(temp);
    if (dots) {
        chord_ellipsis_t *entry = orb_ui_alloc(sizeof(*entry));
        if (!entry) { lv_draw_buf_destroy(dots); return NULL; }
        *entry = (chord_ellipsis_t){.next = s_chord_ellipses, .font = font,
            .color = color, .opa = opa, .tracking = tracking, .font_revision = orb_fonts_revision(),
            .refs = 1, .buffer = dots};
        s_chord_ellipses = entry;
    }
    return dots;
}

void orb_round_text_ink(round_chord_child_t *c)
{
    const char *text = lv_label_get_text(c->obj);
    const lv_font_t *font = lv_obj_get_style_text_font(c->obj, 0);
    lv_text_attributes_t attributes = {
        .letter_space = lv_obj_get_style_text_letter_space(c->obj, 0),
        .line_space = lv_obj_get_style_text_line_space(c->obj, 0), .max_width = c->width,
    };
    int line_height = LV_MAX(1, font->line_height + attributes.line_space);
    unsigned lines = LV_CLAMP(1, (c->height + attributes.line_space) / line_height, CHORD_TEXT_LINES);
    c->ink_top = c->height; c->ink_bottom = -1;
    unsigned start = 0;
    for (unsigned line = 0; line < lines && text[start]; line++) {
        unsigned length = lv_text_get_next_line(text + start, LV_TEXT_LEN_MAX, font, NULL, &attributes);
        if (!length) break;
        uint32_t offset = 0;
        while (offset < length && text[start + offset]) {
            uint32_t letter = lv_text_encoded_next(text + start, &offset);
            uint32_t lookahead = offset;
            uint32_t next = offset < length ? lv_text_encoded_next(text + start, &lookahead) : 0;
            lv_font_glyph_dsc_t glyph;
            if (!lv_font_get_glyph_dsc(font, &glyph, letter, next) || !glyph.box_h) continue;
            int top = (int)line * line_height + font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
            c->ink_top = LV_MIN(c->ink_top, top);
            c->ink_bottom = LV_MAX(c->ink_bottom, top + glyph.box_h - 1);
        }
        /* Native label dots and the compositor's ellipsis retain this lane's
         * baseline, even when its original text has no descending glyphs. */
        const uint32_t terminals[] = {'.', 0x2026};
        for (unsigned i = 0; i < sizeof(terminals) / sizeof(terminals[0]); i++) {
            lv_font_glyph_dsc_t glyph;
            if (!lv_font_get_glyph_dsc(font, &glyph, terminals[i], 0) || !glyph.box_h) continue;
            int top = (int)line * line_height + font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
            c->ink_top = LV_MIN(c->ink_top, top);
            c->ink_bottom = LV_MAX(c->ink_bottom, top + glyph.box_h - 1);
        }
        start += length;
    }
}
