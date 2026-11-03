#include "orb_kit_pages.h"

#include <string.h>

#include "src/misc/lv_text_private.h"

#define STATE_TEXT_WIDTH 340

/* Shared full-lane wrapping decides the composition before compact boxes
 * are considered, so a narrower box cannot inflate the chosen line count. */
static bool state_same_breaks(const char *text, const lv_font_t *font, int32_t spacing,
                               int32_t first_width, int32_t second_width)
{
    lv_text_attributes_t first = {
        .letter_space = spacing, .max_width = first_width, .text_flags = LV_TEXT_FLAG_AVOID_WIDOW,
    };
    lv_text_attributes_t second = first;
    second.max_width = second_width;
    for (uint32_t offset = 0; text[offset];) {
        uint32_t length = lv_text_get_next_line(text + offset, LV_TEXT_LEN_MAX, font, NULL, &first);
        if (!length || length != lv_text_get_next_line(text + offset, LV_TEXT_LEN_MAX, font, NULL, &second))
            return false;
        offset += length;
    }
    return true;
}

typedef struct {
    unsigned lines, first_words;
    int32_t width[2];
} state_wrap_t;

static state_wrap_t state_wrap(const char *text, const lv_font_t *font, int32_t spacing, int32_t width)
{
    state_wrap_t wrap = {0};
    lv_text_attributes_t attributes = {
        .letter_space = spacing, .max_width = width, .text_flags = LV_TEXT_FLAG_AVOID_WIDOW,
    };
    for (uint32_t offset = 0; text[offset] && wrap.lines < 3;) {
        uint32_t length = lv_text_get_next_line(text + offset, LV_TEXT_LEN_MAX, font, NULL, &attributes);
        if (!length) break;
        if (wrap.lines < 2) wrap.width[wrap.lines] = lv_text_get_line_width(text + offset, length, font, &attributes);
        if (!wrap.lines) {
            bool word = false;
            for (uint32_t at = offset; at < offset + length;) {
                bool space = lv_text_is_hanging_space(lv_text_encoded_next(text, &at));
                if (!space && !word) wrap.first_words++;
                word = !space;
            }
        }
        wrap.lines++;
        offset += length;
    }
    return wrap;
}

static bool state_heading_balance(const char *text, const lv_font_t *font, int32_t spacing,
                                    int32_t balanced, int32_t compact)
{
    if (strchr(text, '\n')) return false;
    state_wrap_t full = state_wrap(text, font, spacing, balanced);
    if (full.lines != 2 || full.first_words != 1) return false;
    state_wrap_t candidate = state_wrap(text, font, spacing, compact);
    /* A three-word title cannot put two words on both lines. HIG §4 keeps
     * the more even heading instead of creating a short first-line orphan. */
    return candidate.lines == 2 && candidate.first_words >= 2 &&
        LV_ABS(candidate.width[0] - candidate.width[1]) < LV_ABS(full.width[0] - full.width[1]);
}

static void state_compact(lv_obj_t *label, bool heading)
{
    const char *text = lv_label_get_text(label);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    int32_t spacing = lv_obj_get_style_text_letter_space(label, 0);
    int32_t leading = lv_obj_get_style_text_line_space(label, 0);
    int32_t single = lv_text_get_width(text, strlen(text), font,
        &(lv_text_attributes_t){.letter_space = spacing});
    int32_t lines = (single + STATE_TEXT_WIDTH - 1) / STATE_TEXT_WIDTH;
    int32_t width = lines > 1 ? single / lines + 16 : STATE_TEXT_WIDTH;
    /* This is the established state-page box, including its wrapping slack.
     * Keep it only for identical breaks or a more even two-line heading. */
    for (; width < STATE_TEXT_WIDTH; width += 12) {
        lv_point_t size;
        lv_text_get_size(&size, text, font, spacing, leading, width, LV_TEXT_FLAG_NONE);
        if (size.y <= lines * font->line_height + (lines - 1) * leading) break;
    }
    width = LV_MIN(STATE_TEXT_WIDTH, width);
    if (width == STATE_TEXT_WIDTH) return;
    int32_t balanced = lv_obj_get_content_width(label);
    int32_t compact = orb_kit_text_balance_width(text, font, spacing, width);
    bool same = state_same_breaks(text, font, spacing, balanced, width) &&
        state_same_breaks(text, font, spacing, balanced, compact);
    if (same || (heading && state_heading_balance(text, font, spacing, balanced, compact)))
        lv_obj_set_width(label, width);
}

/* HIG §§1/3/4/8: state is ordinary content, with measured wrapping at AX3.
 * Domain classification and any retry intent stay with the screen. */
static lv_obj_t *state_compact_create(lv_obj_t *parent,
                               const orb_kit_state_config_t *config)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_scrollable(group, false);
    lv_obj_set_clickable(group, false);
    lv_obj_set_event_bubble(group, true);
    lv_obj_set_size(group, 466, 466);
    lv_obj_t *icon = lv_label_create(group);
    lv_label_set_text(icon, config->glyph);
    orb_icon_apply(icon, ORB_TEXT_TITLE2, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(icon, config->secondary, 0);
    lv_obj_t *name = lv_label_create(group);
    orb_text_apply(name, ORB_TEXT_TITLE3, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(name, config->title);
    lv_obj_t *sub = lv_label_create(group);
    orb_text_apply(sub, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(sub, config->detail ? config->detail : "");
    lv_obj_set_style_text_color(name, ORB_LABEL, 0);
    lv_obj_set_style_text_color(sub, config->secondary, 0);
    if (config->secondary_opa != LV_OPA_COVER) {
        lv_obj_set_style_text_opa(icon, config->secondary_opa, 0);
        lv_obj_set_style_text_opa(sub, config->secondary_opa, 0);
    }
    lv_obj_t *text[] = {name, sub};
    for (unsigned i = 0; i < 2; i++) {
        /* HIG §4: balance within the full lane before positioning the group. */
        lv_label_set_long_mode(text[i], LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(text[i], STATE_TEXT_WIDTH);
        lv_obj_set_style_text_align(text[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_update_layout(group);
    for (unsigned i = 0; i < 2; i++) orb_kit_label_balance(text[i]);
    lv_obj_update_layout(group);
    for (unsigned i = 0; i < 2; i++) state_compact(text[i], i == 0);
    lv_obj_update_layout(group);
    bool detail = config->detail && config->detail[0];
    int height = lv_obj_get_height(icon) + 18 + lv_obj_get_height(name) +
        (detail ? 20 + lv_obj_get_height(sub) : 0);
    int y = 233 - height / 2;
    lv_obj_set_pos(icon, 233 - lv_obj_get_width(icon) / 2, y);
    y += lv_obj_get_height(icon) + 18;
    lv_obj_set_pos(name, 233 - lv_obj_get_width(name) / 2, y);
    lv_obj_set_pos(sub, 233 - lv_obj_get_width(sub) / 2, y + lv_obj_get_height(name) + 20);
    lv_obj_set_hidden(sub, !detail);
    orb_kit_pop_in_arrival(icon);
    return group;
}

/* Wrapped centred line box whose width fits its circle chord at its own top
 * and bottom (HIG §3), measured again after wrapping changes its height. */
static int32_t state_line(lv_obj_t *label, int32_t y, int32_t width)
{
    for (unsigned pass = 0; pass < 2; pass++) {
        lv_obj_set_width(label, width);
        orb_kit_label_balance(label);
        lv_obj_update_layout(label);
        int32_t chord = orb_chord_width(y, y + lv_obj_get_height(label) - 1, ORB_RIM_MARGIN_TEXT);
        if (chord >= width) break;
        width = LV_MAX(120, chord);
    }
    lv_obj_set_pos(label, ORB_SCREEN_R - lv_obj_get_width(label) / 2, y);
    return lv_obj_get_height(label);
}

static lv_obj_t *state_text(lv_obj_t *group, const char *text, orb_text_style_t style,
                            orb_text_variant_t variant, lv_color_t colour, lv_opa_t opa)
{
    lv_obj_t *label = lv_label_create(group);
    orb_text_apply(label, style, variant, 0);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_color(label, colour, 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_clickable(label, false);
    lv_obj_set_event_bubble(label, true);
    return label;
}

/* Polish review #28: the one empty/status template. A flat 80 px symbol disc,
 * a Headline that names the state and a secondary hint, as one flow group
 * (never fixed rows, so AX3 cannot overlap them) centred in the caller's band
 * between its rim title and any bottom control. HIG §§1/3/4/8: state is
 * ordinary content; retry and dismissal belong to the screen. */
lv_obj_t *orb_kit_state_create(lv_obj_t *parent,
                               const orb_kit_state_config_t *config)
{
    /* Home keeps its established compact composition. The explicit band
     * opts status screens into the Agents lane's symbol-disc flow. */
    if (!config->top && !config->bottom && !config->accented)
        return state_compact_create(parent, config);
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_scrollable(group, false);
    lv_obj_set_clickable(group, false);
    lv_obj_set_event_bubble(group, true);
    lv_obj_set_size(group, 466, 466);
    lv_color_t accent = config->accented ? config->accent : ORB_KIT_GREY;
    lv_obj_t *symbol = orb_kit_disc_create(group, ORB_KIT_DISC_STANDARD, ORB_KIT_TRACK,
                                           accent, config->glyph);
    lv_obj_set_clickable(symbol, false);
    lv_obj_t *name = state_text(group, config->title, ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED,
                                ORB_LABEL, LV_OPA_COVER);
    lv_obj_t *sub = config->detail && config->detail[0] ?
        state_text(group, config->detail, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR,
                   config->secondary, config->secondary_opa) : NULL;
    int32_t top = config->top > 0 ? config->top : 88;
    int32_t bottom = config->bottom > top ? config->bottom : 378;
    int32_t hint_width = 300;
    /* Narrow compact accessibility hints to keep their final phrase together.
     * Longer explanations retain their measure and the action clearance. */
    if (sub && orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 &&
        state_line(sub, ORB_SCREEN_R, 300) <= 2 * lv_obj_get_style_text_font(sub, 0)->line_height)
        hint_width = 280;
    /* Wrapping depends on each line's final chord, not the band's top.
     * Re-centre the complete flow after measuring there, so larger type
     * cannot leave a title overlapping its hint (review #5/#28). */
    int32_t y = top;
    for (unsigned pass = 0; pass < 4; pass++) {
        int32_t name_y = y + ORB_KIT_DISC + 16;
        int32_t name_h = state_line(name, name_y, 330);
        /* The hint has a narrower reading measure than the title. At AX3
         * this keeps “appear here” and “your Mac” together (review #28). */
        int32_t sub_h = sub ? state_line(sub, name_y + name_h + 8, hint_width) : 0;
        int32_t height = ORB_KIT_DISC + 16 + name_h + (sub ? 8 + sub_h : 0);
        int32_t next = LV_MAX(top, top + (bottom - top - height) / 2);
        if (next == y) break;
        y = next;
    }
    lv_obj_set_pos(symbol, ORB_SCREEN_R - ORB_KIT_DISC / 2, y);
    y += ORB_KIT_DISC + 16;
    y += state_line(name, y, 330) + 8;
    if (sub) state_line(sub, y, hint_width);
    orb_kit_pop_in_arrival(symbol);
    return group;
}

/* Both state composers keep their named state in the second child. */
void orb_kit_state_set_waiting(lv_obj_t *state, bool waiting)
{
    if (state) orb_kit_breathe(lv_obj_get_child(state, 1), waiting);
}

void orb_kit_state_set_waiting_since(lv_obj_t *state, bool waiting, uint32_t started_ms)
{
    if (state) orb_kit_breathe_since(lv_obj_get_child(state, 1), waiting, started_ms);
}
