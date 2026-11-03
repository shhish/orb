#include "orb_kit_row.h"

#include <string.h>

#include "src/misc/lv_text_private.h"

#define ROW_SOURCE_W 418
#define ROW_TEXT_X 70
#define ROW_AX_PAD 10
#define ROW_AX_GAP 4
#define ROW_LIST_GAP 12
/* A resting row off the equator shows a chord up to ~48 px narrower than its
 * 418 px source. AX3 text that would need that slack wraps instead, so the
 * chord compositor does not have to truncate it ("Take the medici…"). */
#define ROW_AX_CHORD_SLACK 48

static uint32_t row_codepoint(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t cp = *p++;
    unsigned remaining = cp < 0x80 ? 0 : cp < 0xE0 ? 1 : cp < 0xF0 ? 2 : 3;
    if (remaining) cp &= (1u << (6 - remaining)) - 1;
    while (remaining-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    *text = (const char *)p;
    return cp;
}

/* Glyph ink of a lane: the first line's top to the last line's bottom. */
static void row_ink(lv_obj_t *label, int32_t *top, int32_t *height)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    const char *text = lv_label_get_text(label);
    int32_t first = INT32_MAX, last = INT32_MIN;
    while (*text) {
        uint32_t cp = row_codepoint(&text);
        const char *peek = text;
        uint32_t next = *peek ? row_codepoint(&peek) : 0;
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, next) || !glyph.box_h) continue;
        int32_t y = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        first = LV_MIN(first, y);
        last = LV_MAX(last, y + glyph.box_h - 1);
    }
    int32_t pitch = font->line_height + lv_obj_get_style_text_line_space(label, 0);
    int32_t lines = LV_MAX(1, lv_label_get_max_lines(label));
    *top = first == INT32_MAX ? 0 : first;
    *height = first == INT32_MAX ? 0 : last - first + 1 + (lines - 1) * pitch;
}

/* A row has immutable lanes. Measuring before setting DOTS matters: LVGL
 * temporarily replaces the label's tail with dots, so first restore its
 * semantic text through a CLIP refresh rather than measuring a previous fit. */
static int32_t row_text_width(lv_obj_t *label, orb_text_style_t style, orb_text_variant_t variant)
{
    const lv_font_t *font = orb_font_variant(style, variant);
    lv_text_attributes_t attributes = {
        .letter_space = orb_text_style_info(style)->letter_space,
        .text_flags = LV_TEXT_FLAG_EXPAND,
    };
    return lv_text_get_width(lv_label_get_text(label), strlen(lv_label_get_text(label)), font, &attributes);
}

static unsigned row_text_lines(lv_obj_t *label, orb_text_style_t style, orb_text_variant_t variant,
                               int32_t width)
{
    const lv_font_t *font = orb_font_variant(style, variant);
    int32_t space = orb_text_line_space(style, variant);
    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(label), font, orb_text_style_info(style)->letter_space,
                     space, width, LV_TEXT_FLAG_NONE);
    return (unsigned)LV_MAX(1, (size.y + space) / LV_MAX(1, font->line_height + space));
}

static void row_restore_text(lv_obj_t *label)
{
    if (!label) return;
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_update_layout(label);
}

/* Headline is Semibold in its regular variant (Bold with Bold Text). Smaller
 * steps keep that weight through their emphasized variant: review #17 saw
 * long titles silently fall back to Regular beside Semibold siblings. */
static orb_text_variant_t row_step_variant(orb_text_style_t first, orb_text_style_t step,
                                           orb_text_variant_t variant)
{
    return first == ORB_TEXT_HEADLINE && step != ORB_TEXT_HEADLINE ? variant | ORB_TEXT_EMPHASIZED : variant;
}

static void row_set_lines(lv_obj_t *label, int32_t width, orb_text_style_t style,
                          orb_text_variant_t variant, unsigned lines)
{
    orb_text_apply(label, style, variant, 0);
    const lv_font_t *font = orb_font_variant(style, variant);
    lv_obj_set_size(label, LV_MAX(1, width), (int32_t)lines * font->line_height +
                    ((int32_t)lines - 1) * orb_text_line_space(style, variant));
    lv_label_set_max_lines(label, (int32_t)lines);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_update_layout(label);
}

static void row_fit_line(lv_obj_t *label, int32_t width, orb_text_style_t first,
                         orb_text_variant_t variant)
{
    const orb_text_style_t title_steps[] = {
        ORB_TEXT_HEADLINE, ORB_TEXT_CAPTION1, ORB_TEXT_CAPTION2,
    };
    unsigned index = first == ORB_TEXT_HEADLINE ? 0 : 2;
    while (index + 1 < sizeof(title_steps) / sizeof(title_steps[0]) &&
           row_text_width(label, title_steps[index], row_step_variant(first, title_steps[index], variant)) > width)
        index++;
    /* HIG §§3/4: a source must remain at least20px at the chord compositor's
     * 82% shoulder scale. Footnote2(21px) becomes17px and can make an entire
     * 72px row disappear, despite a valid native tap target. Stop at Caption2
     * (25px ->20.5px) and ellipsise a long title instead of losing the row. */
    row_set_lines(label, width, title_steps[index], row_step_variant(first, title_steps[index], variant), 1);
}

/* HIG §4: use two lines at the lane's semantic style before truncating.
 * Reserve the shoulder slack once; the compositor then only chooses a
 * cached whole-glyph cut while the row travels around the round screen. */
static void row_fit_wrapped(lv_obj_t *label, int32_t width, orb_text_style_t style,
                            orb_text_variant_t variant)
{
    int32_t wrap = LV_MAX(1, width - ROW_AX_CHORD_SLACK);
    unsigned lines = row_text_width(label, style, variant) <= wrap ? 1 :
        LV_MIN(2u, row_text_lines(label, style, variant, wrap));
    row_set_lines(label, lines > 1 ? wrap : width, style, variant, lines);
}

static void row_fit_full_title(kit_row_t *state, int32_t width)
{
    int32_t viewport = lv_obj_get_height(state->list), top = lv_obj_get_y(state->list);
    int32_t centred = 2 * LV_MIN(ORB_SCREEN_D / 2 - top, top + viewport - ORB_SCREEN_D / 2);
    /* HIG §§3/4: a complete identity must fit above and below the round
     * display's equator, even when the title leaves an asymmetric viewport. */
    int32_t budget = LV_MIN(viewport, centred > 0 ? centred : viewport) - 2 * ROW_AX_PAD;
    lv_obj_t *other[] = {state->subtitle, state->stacked ? state->value : NULL};
    for (unsigned i = 0; i < sizeof(other) / sizeof(*other); ++i) {
        if (!other[i]) continue;
        budget -= lv_obj_get_height(other[i]) + ROW_AX_GAP;
    }
    if (state->switch_below) budget -= 46 + 20;
    int32_t wrap = LV_MAX(1, width - ROW_AX_CHORD_SLACK);
    const orb_text_style_t styles[] = {ORB_TEXT_HEADLINE, ORB_TEXT_CAPTION1, ORB_TEXT_CAPTION2};
    for (unsigned i = 0; i < sizeof(styles) / sizeof(*styles); ++i) {
        orb_text_variant_t variant = row_step_variant(ORB_TEXT_HEADLINE, styles[i], state->title_variant);
        unsigned lines = row_text_lines(state->title, styles[i], variant, wrap);
        const lv_font_t *font = orb_font_variant(styles[i], variant);
        int32_t height = (int32_t)lines * font->line_height +
            ((int32_t)lines - 1) * orb_text_line_space(styles[i], variant);
        if (height > budget && i + 1 < sizeof(styles) / sizeof(*styles)) continue;
        /* A one-line name keeps the whole lane: the chord compositor preserves
         * a label's native right gap, so a narrower one cuts it at the shoulder. */
        row_set_lines(state->title, lines > 1 ? wrap : width, styles[i], variant, lines);
        return;
    }
}

static int32_t row_value_rail(kit_row_t *state)
{
    /* Values own their measured rail (at most half of the whole row).
     * The title yields first; a short count no longer reserves92px and
     * long real states such as No Response can use the available chord. */
    int32_t measured = row_text_width(state->value, ORB_TEXT_CAPTION2, state->value_variant);
    int32_t rail = LV_MIN(ROW_SOURCE_W / 2, measured);
    row_fit_line(state->value, rail, ORB_TEXT_CAPTION2, state->value_variant);
    measured = lv_text_get_width(lv_label_get_text(state->value), strlen(lv_label_get_text(state->value)),
                                 lv_obj_get_style_text_font(state->value, 0),
                                 &(lv_text_attributes_t){
                                     .letter_space=lv_obj_get_style_text_letter_space(state->value, 0),
                                     .text_flags=LV_TEXT_FLAG_EXPAND,
                                 });
    return LV_MIN(rail, measured);
}

static void row_layout_default(lv_obj_t *row, kit_row_t *state, int32_t width)
{
    row_fit_line(state->title, width, ORB_TEXT_HEADLINE, state->title_variant);
    if (state->subtitle) row_fit_line(state->subtitle, width, ORB_TEXT_CAPTION2, state->subtitle_variant);
    lv_obj_set_height(row, ORB_KIT_ROW_H);
    lv_obj_set_y(state->disc, (ORB_KIT_ROW_H - ORB_KIT_DISC_ROW) / 2);
    int32_t title_height = lv_obj_get_height(state->title);
    if (!state->subtitle) {
        lv_obj_set_pos(state->title, ROW_TEXT_X, (ORB_KIT_ROW_H - title_height) / 2);
    } else {
        int32_t title_top, title_ink, subtitle_top, subtitle_ink;
        row_ink(state->title, &title_top, &title_ink);
        row_ink(state->subtitle, &subtitle_top, &subtitle_ink);
        int32_t gap = LV_CLAMP(3, ORB_KIT_ROW_H - title_ink - subtitle_ink, 4);
        int32_t top = (ORB_KIT_ROW_H - title_ink - subtitle_ink - gap) / 2;
        /* The baseline separation follows actual glyph ink, not font leading.
         * This keeps AX3 descenders and two single-line lanes apart. */
        lv_obj_set_pos(state->title, ROW_TEXT_X, top - title_top);
        lv_obj_set_pos(state->subtitle, ROW_TEXT_X, top + title_ink + gap - subtitle_top);
    }
}

static void row_layout_ax(lv_obj_t *row, kit_row_t *state, int32_t width)
{
    lv_obj_t *lanes[] = {state->title, state->subtitle, state->stacked ? state->value : NULL};
    /* Live lanes are single-line counter streams. Their transparent anchors
     * must share that contract: wrapping a reservation grew Stack's compact
     * peek into an oval while its visible CLIP label still had only one line. */
    if (state->live[ORB_KIT_ROW_TEXT_TITLE])
        row_fit_line(state->title, width, ORB_TEXT_HEADLINE, state->title_variant);
    else if (!state->full_title) row_fit_wrapped(state->title, width, ORB_TEXT_HEADLINE, state->title_variant);
    if (state->subtitle) {
        if (state->live[ORB_KIT_ROW_TEXT_SUBTITLE])
            row_fit_line(state->subtitle, width, ORB_TEXT_CAPTION2, state->subtitle_variant);
        else row_fit_wrapped(state->subtitle, width, ORB_TEXT_CAPTION2, state->subtitle_variant);
    }
    if (state->stacked) {
        /* iOS/watchOS stack a value under its title at accessibility sizes,
         * so "Repeat" is never truncated to make room for "One time". */
        lv_obj_set_style_text_align(state->value, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_align(state->value, LV_ALIGN_TOP_LEFT);
        row_set_lines(state->value, width, ORB_TEXT_CAPTION2, state->value_variant, 1);
    }
    if (state->full_title && !state->live[ORB_KIT_ROW_TEXT_TITLE]) row_fit_full_title(state, width);
    int32_t top[3] = {0}, ink[3] = {0}, total = 0, count = 0;
    bool wrapped = false;
    for (unsigned i = 0; i < 3; i++) {
        if (!lanes[i]) continue;
        row_ink(lanes[i], &top[i], &ink[i]);
        total += ink[i] + (count++ ? ROW_AX_GAP : 0);
        wrapped |= lv_label_get_max_lines(lanes[i]) > 1;
    }
    if (count == 1 && !wrapped && !state->switch_below) {
        /* One line still centres in the 72 px capsule, exactly as before. */
        lv_obj_set_height(row, ORB_KIT_ROW_H);
        lv_obj_set_y(state->disc, (ORB_KIT_ROW_H - ORB_KIT_DISC_ROW) / 2);
        lv_obj_set_pos(state->title, ROW_TEXT_X, (ORB_KIT_ROW_H - lv_obj_get_height(state->title)) / 2);
        return;
    }
    /* HIG §§3/4/7: a switch yields its rail before enlarged text becomes a
     * one-letter shoulder preview. Keep the full title above a separate
     * control line; list flow and cached chord geometry use this same row. */
    int32_t controls = state->switch_below ? 46 + 20 : 0;
    int32_t height = LV_MAX(ORB_KIT_ROW_H, total + controls + 2 * ROW_AX_PAD);
    lv_obj_set_height(row, height);
    lv_obj_set_y(state->disc, (height - ORB_KIT_DISC_ROW) / 2);
    int32_t y = (height - total - controls) / 2;
    for (unsigned i = 0; i < 3; i++) {
        if (!lanes[i]) continue;
        lv_obj_set_pos(lanes[i], ROW_TEXT_X, y - top[i]);
        y += ink[i] + ROW_AX_GAP;
    }
    if (state->switch_below) {
        lv_obj_set_align(state->anchor, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(state->anchor, ROW_TEXT_X, height - ROW_AX_PAD - 46);
    }
}

void orb_kit_row_layout(lv_obj_t *row, kit_row_t *state)
{
    int32_t width = ROW_SOURCE_W - ROW_TEXT_X - 16;
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    row_restore_text(state->title);
    row_restore_text(state->subtitle);
    row_restore_text(state->value);
    state->stacked = false;
    state->switch_below = state->toggle && ax &&
        row_text_width(state->title, ORB_TEXT_HEADLINE, state->title_variant) >
            width - 108 - ROW_AX_CHORD_SLACK;
    if (state->toggle) {
        state->trailing = state->switch_below ? 0 : 108;
        if (!state->switch_below) lv_obj_align(state->anchor, LV_ALIGN_RIGHT_MID, -28, 0);
    }
    if (state->value) {
        int32_t rail = row_value_rail(state);
        int32_t trailing = 28 + 12 + rail - 16;
        /* At AX3 the title wins: when it cannot keep one Headline line beside
         * the value, the value moves under it instead of truncating both. */
        int32_t beside = width - trailing - ROW_AX_CHORD_SLACK;
        state->stacked = ax && (row_text_width(state->title, ORB_TEXT_HEADLINE, state->title_variant) > beside ||
            (state->full_title && state->subtitle &&
                row_text_width(state->subtitle, ORB_TEXT_CAPTION2, state->subtitle_variant) > beside));
        if (!state->stacked) {
            lv_obj_set_width(state->value, LV_MAX(1, rail));
            lv_obj_set_style_text_align(state->value, LV_TEXT_ALIGN_RIGHT, 0);
            state->trailing = trailing;
            lv_obj_align(state->value, LV_ALIGN_RIGHT_MID, -28, 0);
        } else {
            state->trailing = 0;
        }
    }
    width -= state->trailing;
    if (ax || state->wrap_title) row_layout_ax(row, state, width);
    else row_layout_default(row, state, width);
}

