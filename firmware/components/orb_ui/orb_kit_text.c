#include "orb_kit.h"
#include "orb_ui_private.h"

#include <string.h>

#include "src/misc/lv_text_private.h"

/* Shared text primitives (polish-text lane): one truncation, one balanced
 * wrap, one command wrap and one whole-line reader rule, so every screen that
 * uses the kit gets the same typography (HIG §4, docs/design/glossary.md §1). */

#define KIT_ELLIPSIS "\xE2\x80\xA6"

static lv_text_attributes_t text_attributes(int32_t letter_space, int32_t max_width)
{
    return (lv_text_attributes_t){
        .letter_space = letter_space, .max_width = max_width, .text_flags = LV_TEXT_FLAG_NONE,
    };
}

static int32_t text_width(const char *text, uint32_t length, const lv_font_t *font, int32_t letter_space)
{
    lv_text_attributes_t attributes = text_attributes(letter_space, LV_COORD_MAX);
    return lv_text_get_width(text, length, font, &attributes);
}

static uint32_t text_next(const char *text, uint32_t offset)
{
    lv_text_encoded_next(text, &offset);
    return offset;
}

size_t orb_kit_text_truncate(const char *text, const lv_font_t *font, int32_t letter_space,
                             int32_t width, char *out, size_t capacity)
{
    if (!out || !capacity) return 0;
    out[0] = '\0';
    if (!text || !font) return 0;
    size_t length = strlen(text);
    if (length < capacity && text_width(text, length, font, letter_space) <= width) {
        size_t copy = LV_MIN(length, capacity - 1);
        while (copy && copy < length && ((unsigned char)text[copy] & 0xc0) == 0x80) copy--;
        memcpy(out, text, copy);
        out[copy] = '\0';
        return copy;
    }
    const char *dots = KIT_ELLIPSIS;
    if (capacity < sizeof(KIT_ELLIPSIS) ||
        text_width(dots, sizeof(KIT_ELLIPSIS) - 1, font, letter_space) > width) return 0;
    /* Whole code points only, never a space or separator in front of the
     * ellipsis (glossary §1.3), and never more than the caller's buffer. */
    uint32_t keep = 0;
    for (uint32_t end = 0; end < length;) {
        uint32_t next = text_next(text, end);
        if (next + strlen(dots) + 1 > capacity) break;
        uint32_t previous = next;
        if (!lv_label_trims_before_ellipsis(lv_text_encoded_prev(text, &previous))) {
            memcpy(out, text, next);
            memcpy(out + next, dots, sizeof(KIT_ELLIPSIS));
            /* Measure the real neighbour: the kept glyph's kerning against
             * U+2026 differs from its kerning against the discarded text. */
            if (text_width(out, next + sizeof(KIT_ELLIPSIS) - 1, font, letter_space) > width) break;
            keep = next;
        }
        end = next;
    }
    memcpy(out, text, keep);
    memcpy(out + keep, dots, strlen(dots) + 1);
    return keep + strlen(dots);
}

/* ---- Balanced wrapping ------------------------------------------------------------ */

typedef struct {
    unsigned lines;
    int32_t widest, last;
    bool last_single_word;
} kit_wrap_t;

/* LVGL's own widow-aware wrap at `width`, so a break computed here is exactly a
 * break the label would make, and every balanced line re-fits the label. */
static kit_wrap_t wrap_measure(const char *text, const lv_font_t *font, int32_t letter_space, int32_t width)
{
    kit_wrap_t wrap = {0};
    lv_text_attributes_t attributes = text_attributes(letter_space, width);
    attributes.text_flags |= LV_TEXT_FLAG_AVOID_WIDOW;
    for (uint32_t start = 0; text[start];) {
        int32_t used = 0;
        uint32_t length = lv_text_get_next_line(text + start, LV_TEXT_LEN_MAX, font, &used, &attributes);
        if (!length) break;
        uint32_t visible = length;
        while (visible && (text[start + visible - 1] == ' ' || text[start + visible - 1] == '\n')) visible--;
        int32_t line = text_width(text + start, visible, font, letter_space);
        wrap.widest = LV_MAX(wrap.widest, line);
        wrap.last = line;
        wrap.last_single_word = !memchr(text + start, ' ', visible);
        wrap.lines++;
        start += length;
    }
    return wrap;
}

static int32_t widest_word(const char *text, const lv_font_t *font, int32_t letter_space)
{
    int32_t widest = 0;
    for (const char *word = text; *word;) {
        const char *end = strchr(word, ' ');
        size_t length = end ? (size_t)(end - word) : strlen(word);
        widest = LV_MAX(widest, text_width(word, length, font, letter_space));
        word += length + (end ? 1 : 0);
    }
    return widest;
}

int32_t orb_kit_text_balance_width(const char *text, const lv_font_t *font, int32_t letter_space,
                                   int32_t width)
{
    if (!text || !font || width < 1 || strchr(text, '\n')) return width;
    kit_wrap_t greedy = wrap_measure(text, font, letter_space, width);
    /* HIG §4: a wrapped heading never leaves a widow ("A keepsake for / you").
     * A short or single-word last line is rebalanced at the narrowest width
     * that keeps the same line count; other wraps stay exactly as LVGL made
     * them, so layouts that already read well do not move. */
    if (greedy.lines < 2 || (!greedy.last_single_word && greedy.last * 2 >= greedy.widest)) return width;
    int32_t low = LV_MAX(widest_word(text, font, letter_space), width / (int32_t)greedy.lines), high = width;
    if (low > high) return width;
    while (low < high) {
        int32_t middle = (low + high) / 2;
        if (wrap_measure(text, font, letter_space, middle).lines == greedy.lines) high = middle;
        else low = middle + 1;
    }
    return wrap_measure(text, font, letter_space, high).last_single_word ? width : high;
}

/* ---- Label wrappers --------------------------------------------------------------- */

/* The balanced wrap is a narrower text box inside the caller's lane, made by
 * horizontal padding on the alignment's free side(s): the label keeps its
 * geometry and its semantic text (no inserted breaks), and LVGL, the chord
 * painter and hit testing all wrap it the same way. */
typedef struct {
    int32_t left, right; /* Padding this wrapper applied. */
    bool updating;
} kit_balance_t;

static void balance_apply(lv_obj_t *label, kit_balance_t *state);

static void balance_event(lv_event_t *event)
{
    kit_balance_t *state = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) orb_ui_free(state);
    else if (!state->updating && (code == LV_EVENT_SIZE_CHANGED || code == LV_EVENT_STYLE_CHANGED))
        balance_apply(lv_event_get_current_target_obj(event), state);
}

static kit_balance_t *balance_find(lv_obj_t *label)
{
    for (unsigned i = 0; i < lv_obj_get_event_count(label); i++) {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(label, i);
        if (lv_event_dsc_get_cb(descriptor) == balance_event) return lv_event_dsc_get_user_data(descriptor);
    }
    return NULL;
}

static void balance_apply(lv_obj_t *label, kit_balance_t *state)
{
    if (state->updating) return;
    state->updating = true;
    lv_obj_update_layout(label);
    int32_t lane = lv_obj_get_content_width(label) + state->left + state->right;
    int32_t width = orb_kit_text_balance_width(lv_label_get_text(label), lv_obj_get_style_text_font(label, 0),
                                               lv_obj_get_style_text_letter_space(label, 0), lane);
    int32_t spare = LV_MAX(0, lane - width);
    lv_text_align_t align = lv_obj_get_style_text_align(label, 0);
    int32_t left = align == LV_TEXT_ALIGN_CENTER ? spare / 2 : align == LV_TEXT_ALIGN_RIGHT ? spare : 0;
    int32_t right = spare - left;
    int32_t base_left = lv_obj_get_style_pad_left(label, 0) - state->left;
    int32_t base_right = lv_obj_get_style_pad_right(label, 0) - state->right;
    if (left != state->left) lv_obj_set_style_pad_left(label, base_left + left, 0);
    if (right != state->right) lv_obj_set_style_pad_right(label, base_right + right, 0);
    state->left = left;
    state->right = right;
    state->updating = false;
}

static void balance_fonts(lv_observer_t *observer, lv_subject_t *subject)
{
    LV_UNUSED(subject);
    lv_obj_t *label = lv_observer_get_target_obj(observer);
    kit_balance_t *state = balance_find(label);
    if (state) balance_apply(label, state);
}

void orb_kit_label_balance(lv_obj_t *label)
{
    if (!label || !lv_obj_check_type(label, &lv_label_class)) return;
    lv_label_set_avoid_widow(label, true);
    kit_balance_t *state = balance_find(label);
    if (!state) {
        state = orb_ui_alloc(sizeof(*state));
        if (!state) return;
        *state = (kit_balance_t){0};
        if (!lv_obj_add_event_cb(label, balance_event, LV_EVENT_ALL, state)) {
            orb_ui_free(state);
            return;
        }
        lv_subject_add_observer_obj(orb_fonts_subject(), balance_fonts, label, NULL);
    }
    balance_apply(label, state);
}

void orb_kit_label_set_command(lv_obj_t *label, const char *command)
{
    if (!label || !lv_obj_check_type(label, &lv_label_class)) return;
    /* Polish review (approvals #2): a command breaks only at spaces and after
     * slashes, never at "-j4"'s hyphen or "build.c"'s period; the text stays
     * the exact command. */
    lv_label_set_path_breaks(label, true);
    lv_label_set_text(label, command ? command : "");
}

/* ---- Readers ---------------------------------------------------------------------- */

typedef struct {
    int32_t top, bottom, left, right;
} kit_block_t;

#define KIT_READER_BLOCKS 96

typedef struct {
    kit_block_t block[KIT_READER_BLOCKS];
    unsigned count;
    int32_t origin, content;
} kit_reader_t;

static void reader_add(kit_reader_t *reader, int32_t top, int32_t bottom, int32_t left, int32_t right)
{
    reader->content = LV_MAX(reader->content, bottom);
    if (reader->count < KIT_READER_BLOCKS)
        reader->block[reader->count++] = (kit_block_t){top, bottom, left, right};
}

/* Inter's line box overlaps the next line (negative leading). Measure the
 * actual ink, including accents and descenders: blank font descent must not
 * hide a complete last line that already fits the caller's reading budget. */
static void reader_label(kit_reader_t *reader, lv_obj_t *label, int32_t top)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    int32_t pitch = font->line_height + lv_obj_get_style_text_line_space(label, 0);
    lv_text_attributes_t attributes = text_attributes(lv_obj_get_style_text_letter_space(label, 0),
                                                       lv_obj_get_content_width(label));
    if (lv_label_get_avoid_widow(label)) attributes.text_flags |= LV_TEXT_FLAG_AVOID_WIDOW;
    if (lv_label_get_path_breaks(label)) attributes.text_flags |= LV_TEXT_FLAG_PATH_BREAKS;
    const char *text = lv_label_get_text(label);
    int32_t height = lv_obj_get_height(label);
    uint32_t start = 0;
    for (int32_t line = top; text[start] && line + font->line_height <= top + height + 1 && pitch > 0; line += pitch) {
        uint32_t length = lv_text_get_next_line(text + start, LV_TEXT_LEN_MAX, font, NULL, &attributes);
        if (!length) break;
        int32_t ink_top = INT32_MAX, ink_bottom = INT32_MIN;
        for (uint32_t offset = start; offset < start + length;) {
            uint32_t cp = lv_text_encoded_next(text, &offset), next = offset;
            lv_font_glyph_dsc_t glyph;
            if (lv_font_get_glyph_dsc(font, &glyph, cp, lv_text_encoded_next(text, &next)) && glyph.box_h) {
                int32_t bottom = line + font->line_height - font->base_line - glyph.ofs_y;
                ink_top = LV_MIN(ink_top, bottom - glyph.box_h);
                ink_bottom = LV_MAX(ink_bottom, bottom);
            }
        }
        if (ink_top < ink_bottom) {
            uint32_t visible = length;
            while (visible && (text[start + visible - 1] == ' ' || text[start + visible - 1] == '\n')) visible--;
            int32_t width = lv_text_get_width(text + start, visible, font, &attributes);
            lv_area_t bounds;
            lv_obj_get_coords(label, &bounds);
            int32_t left = bounds.x1 + lv_obj_get_style_pad_left(label, 0);
            lv_text_align_t align = lv_obj_get_style_text_align(label, 0);
            int32_t spare = lv_obj_get_content_width(label) - width;
            if (align == LV_TEXT_ALIGN_CENTER) left += spare / 2;
            else if (align == LV_TEXT_ALIGN_RIGHT) left += spare;
            reader_add(reader, ink_top, ink_bottom, left, left + width);
        }
        start += length;
    }
}

static void reader_collect(kit_reader_t *reader, lv_obj_t *parent)
{
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); i++) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        if (lv_obj_is_hidden(child) || lv_obj_is_floating(child)) continue;
        lv_area_t area;
        lv_obj_get_coords(child, &area);
        int32_t top = area.y1 - reader->origin;
        if (lv_obj_check_type(child, &lv_label_class)) {
            if (lv_label_get_text(child)[0]) reader_label(reader, child, top);
        } else if (lv_obj_is_clickable(child)) {
            /* A control may rest partly below the fold, as in a watchOS
             * list, but its complete end is a safe stop before later prose.
             * Without that boundary a partial context line can snap past
             * every choice to the heading above them. A zero-height marker
             * keeps the control splittable while retaining its full end. */
            int32_t end = area.y2 + 1 - reader->origin;
            reader_add(reader, end, end, 0, 0);
        } else {
            reader->content = LV_MAX(reader->content, area.y2 + 1 - reader->origin);
            reader_collect(reader, child);
        }
    }
}

static bool reader_straddles(const kit_reader_t *reader, int32_t cut)
{
    for (unsigned i = 0; i < reader->count; i++)
        if (reader->block[i].top < cut && reader->block[i].bottom > cut) return true;
    return false;
}

static void reader_measure(lv_obj_t *reader, kit_reader_t *blocks)
{
    lv_obj_update_layout(reader);
    lv_area_t area;
    lv_obj_get_coords(reader, &area);
    /* Resting geometry is the unscrolled one; this is never a scroll path. */
    *blocks = (kit_reader_t){.origin = area.y1 - lv_obj_get_scroll_y(reader)};
    reader_collect(blocks, reader);
}

static int32_t reader_height(const kit_reader_t *blocks, int32_t available)
{
    /* HIG §3: a vertically complete line can still lose its first/last
     * letters to the round bezel. Rest before that line; scrolling brings
     * the exact unshortened text into the wide reading band. */
    for (unsigned i = 0; i < blocks->count; i++) {
        const kit_block_t *line = &blocks->block[i];
        if (blocks->origin + line->top < ORB_SCREEN_R || line->bottom > available ||
            line->left >= line->right) continue;
        int32_t chord = orb_chord_width(blocks->origin + line->top,
            blocks->origin + line->bottom - 1, ORB_RIM_MARGIN_TEXT);
        if (line->left < ORB_SCREEN_R - chord / 2 || line->right > ORB_SCREEN_R + chord / 2)
            available = LV_MIN(available, line->top);
    }
    int32_t best = available;
    if (blocks->content > available && reader_straddles(blocks, available)) {
        best = 0;
        for (unsigned i = 0; i < blocks->count; i++) {
            int32_t cut = blocks->block[i].bottom;
            /* End in the following blank scanline when it fits. The full
             * glyph then has breathing room at the viewport edge, without
             * requiring unused descent that could hide a fitting last line. */
            if (cut < available && !reader_straddles(blocks, cut + 1)) cut++;
            if (cut <= available && cut > best && !reader_straddles(blocks, cut)) best = cut;
        }
        if (!best) best = available;
    }
    return best;
}

int32_t orb_kit_reader_whole_lines(lv_obj_t *reader, int32_t available)
{
    if (!reader) return available;
    kit_reader_t *blocks = orb_ui_alloc(sizeof(*blocks));
    if (!blocks) return available;
    reader_measure(reader, blocks);
    int32_t height = reader_height(blocks, available);
    orb_ui_free(blocks);
    return height;
}

/* Readers snap on the display's next refresh, before LVGL's layout pass, so
 * content added after the viewport (paragraphs, chips, rows) is included and
 * nothing measures while a list scrolls: only a content change marks one. */
#define KIT_SNAP_READERS 8

typedef struct {
    lv_obj_t *reader;
    kit_reader_t *lines;
    int32_t requested, applied;
    bool dirty;
} kit_snap_t;

static kit_snap_t s_snap[KIT_SNAP_READERS];
static lv_display_t *s_snap_display;

static void snap_run(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    /* DELETE precedes the readers' own cache cleanup. Retire the display
     * pointer now so registering a replacement never uses freed memory. */
    if (code == LV_EVENT_DELETE) { s_snap_display = NULL; return; }
    if (code != LV_EVENT_REFR_START) return;
    for (unsigned i = 0; i < KIT_SNAP_READERS; i++) {
        kit_snap_t *snap = &s_snap[i];
        if (!snap->reader || !snap->dirty) continue;
        if (lv_obj_is_scrolling(snap->reader)) continue;
        snap->dirty = false;
        /* The rule is for the resting top. A reader already scrolled (a
         * restored position, or a target brought into view) keeps its
         * geometry, so the scrolled-to content stays exactly in view. */
        int32_t height = lv_obj_get_style_height(snap->reader, 0);
        if (height != snap->applied) snap->requested = height; /* The caller resized it. */
        if (!snap->lines) snap->lines = orb_ui_alloc(sizeof(*snap->lines));
        if (!snap->lines) continue;
        reader_measure(snap->reader, snap->lines);
        if (lv_obj_get_scroll_y(snap->reader) != 0) continue;
        int32_t snapped = reader_height(snap->lines, snap->requested);
        snap->applied = snapped;
        if (snapped != height) lv_obj_set_height(snap->reader, snapped);
        snap->dirty = false; /* Its own resize is not new content. */
    }
}

static void snap_event(lv_event_t *event)
{
    kit_snap_t *snap = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        orb_ui_free(snap->lines);
        *snap = (kit_snap_t){0};
    } else if (code == LV_EVENT_CHILD_CHANGED || code == LV_EVENT_CHILD_CREATED) {
        /* Floating overlays (row switches, live values) follow every scroll
         * frame; they are not reading content and must not cost a measure. */
        lv_obj_t *child = lv_event_get_param(event);
        if (!child || !lv_obj_is_floating(child)) snap->dirty = true;
    } else if (code == LV_EVENT_SIZE_CHANGED || code == LV_EVENT_STYLE_CHANGED) {
        snap->dirty = true;
    }
}

static void snap_fonts(lv_observer_t *observer, lv_subject_t *subject)
{
    LV_UNUSED(subject);
    lv_obj_t *reader = lv_observer_get_target_obj(observer);
    for (unsigned i = 0; i < KIT_SNAP_READERS; i++)
        if (s_snap[i].reader == reader) s_snap[i].dirty = true;
}

void orb_kit_reader_fades(lv_obj_t *reader, int32_t *top, int32_t *bottom)
{
    for (unsigned i = 0; i < KIT_SNAP_READERS; i++) {
        kit_snap_t *snap = &s_snap[i];
        if (snap->reader != reader || !snap->lines) continue;
        int32_t scroll = lv_obj_get_scroll_y(reader), height = lv_obj_get_height(reader);
        int32_t first = INT32_MAX, last = INT32_MIN;
        for (unsigned n = 0; n < snap->lines->count; n++) {
            const kit_block_t *line = &snap->lines->block[n];
            /* Control-end markers are snap candidates, not glyph ink. */
            if (line->top == line->bottom || line->top < scroll || line->bottom > scroll + height) continue;
            first = LV_MIN(first, line->top);
            last = LV_MAX(last, line->bottom);
        }
        /* HIG §§4/8: fades may dissolve partial lines leaving the viewport,
         * but every complete line stays readable. Cached ink bounds make this
         * arithmetic only; list scrolling never typesets. */
        if (first < last) {
            *top = LV_MIN(*top, LV_MAX(0, first - scroll));
            *bottom = LV_MIN(*bottom, LV_MAX(0, height - (last - scroll)));
        } else *top = *bottom = 0;
        return;
    }
}

void orb_kit_reader_snap(lv_obj_t *reader)
{
    if (!reader) return;
    /* This fork's label setters emit no text-change event. Repeat this call
     * after changing label text so cached ink updates even at a fixed size. */
    for (unsigned i = 0; i < KIT_SNAP_READERS; i++) {
        if (s_snap[i].reader != reader) continue;
        s_snap[i].dirty = true;
        return;
    }
    lv_display_t *display = lv_obj_get_display(reader);
    if (display && display != s_snap_display) {
        if (s_snap_display) lv_display_remove_event_cb_with_user_data(s_snap_display, snap_run, NULL);
        s_snap_display = lv_display_add_event_cb(display, snap_run, LV_EVENT_ALL, NULL) ? display : NULL;
    }
    for (unsigned i = 0; i < KIT_SNAP_READERS; i++) {
        kit_snap_t *snap = &s_snap[i];
        if (snap->reader) continue;
        int32_t height = lv_obj_get_style_height(reader, 0);
        *snap = (kit_snap_t){.reader = reader, .requested = height, .applied = height, .dirty = true};
        lv_obj_add_event_cb(reader, snap_event, LV_EVENT_ALL, snap);
        lv_subject_add_observer_obj(orb_fonts_subject(), snap_fonts, reader, NULL);
        return;
    }
}
