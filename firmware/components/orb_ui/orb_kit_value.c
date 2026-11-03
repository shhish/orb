#include "orb_kit_value.h"
#include "orb_ui_private.h"

#include <string.h>

#include "src/misc/lv_text_private.h"
#include "src/widgets/label/lv_label_private.h"

/* Live values, split from orb_kit.c for its size guard rail: one reserved
 * lane per changing label, refreshed in place without allocation per tick. */
static void value_event(lv_event_t *event);

kit_value_t *orb_kit_value_find(lv_obj_t *label)
{
    if (!label) return NULL;
    for (unsigned i = 0; i < lv_obj_get_event_count(label); i++) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(label, i);
        if (lv_event_dsc_get_cb(event) == value_event) return lv_event_dsc_get_user_data(event);
    }
    return NULL;
}

static void value_tabular(kit_value_t *value)
{
    const lv_font_t *font = lv_obj_get_style_text_font(value->label, 0);
    /* Clock faces already use tabular assets. Do not replace their family
     * with a body face just because both contain changing figures. */
    for (unsigned clock = 0; clock < ORB_CLOCK_COUNT; clock++) {
        if (font == orb_clock_font(clock)) { value->tabular = true; return; }
    }
    for (unsigned style = 0; style < ORB_TEXT_STYLE_COUNT; style++) {
        for (unsigned variant = 0; variant < 4; variant++) {
            if (font != orb_font_variant(style, variant)) continue;
            if (variant & ORB_TEXT_TABULAR) { value->tabular = true; return; }
            lv_obj_remove_local_style_prop(value->label, LV_STYLE_TEXT_FONT, 0);
            orb_text_apply(value->label, style, variant | ORB_TEXT_TABULAR, 0);
            value->tabular = true;
            return;
        }
    }
    /* Custom callers can supply a preselected tabular family. Keep it intact
     * and expose actual advances through the diagnostics rather than claim
     * that an unknown face is tabular. */
    lv_font_glyph_dsc_t first, glyph;
    value->tabular = lv_font_get_glyph_dsc(font, &first, '0', 0);
    for (unsigned digit = '1'; digit <= '9' && value->tabular; digit++)
        value->tabular = lv_font_get_glyph_dsc(font, &glyph, digit, 0) && glyph.adv_w == first.adv_w;
}

void orb_kit_value_measure(kit_value_t *value)
{
    value_tabular(value);
    lv_text_attributes_t attributes = {
        .letter_space = lv_obj_get_style_text_letter_space(value->label, 0),
        .text_flags = LV_TEXT_FLAG_EXPAND,
    };
    const char *text = value->reservation ? value->reservation : "";
    int32_t width = lv_text_get_width(text, strlen(text),
        lv_obj_get_style_text_font(value->label, 0), &attributes);
    value->width = LV_MAX(value->minimum_width, LV_MAX(1, width));
    /* A chord row can reserve a narrower, fitted lane. Its constraint changes
     * only with typography/template configuration, never with a value tick. */
    value->measured_max_width = lv_obj_get_style_max_width(value->label, 0);
    if (value->measured_max_width > 0 && value->measured_max_width < ORB_SCREEN_D * 2)
        value->width = LV_MIN(value->width, value->measured_max_width);
    value->measured_font = lv_obj_get_style_text_font(value->label, 0);
    value->measured_spacing = attributes.letter_space;
    value->template_measures++;
    lv_obj_set_width(value->label, value->width);
    lv_obj_set_height(value->label, lv_font_get_line_height(lv_obj_get_style_text_font(value->label, 0)));
    lv_label_set_max_lines(value->label, 1);
    lv_label_set_long_mode(value->label, LV_LABEL_LONG_MODE_CLIP);
}

static void value_fonts(lv_observer_t *observer, lv_subject_t *subject)
{
    LV_UNUSED(subject);
    kit_value_t *value = orb_kit_value_find(lv_observer_get_target_obj(observer));
    if (value && value->reservation) { value->fit_font = NULL; orb_kit_value_measure(value); }
}

static void value_event(lv_event_t *event)
{
    kit_value_t *value = lv_event_get_user_data(event);
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    lv_free(value->reservation);
    lv_free(value->semantic);
    lv_free(value->fitted);
    lv_free(value->fit_prefix);
    /* The LVGL label owns the reserved buffer and frees it in its destructor.
     * Dynamic ownership also makes each queued draw task snapshot its text. */
    lv_free(value);
}

static void value_refresh(kit_value_t *value)
{
    /* The LVGL public text setter allocates a one-shot display callback on
     * every refresh, even when both axes are fixed. Live values are a single
     * CLIP line with immutable metrics: update their owned text in place and
     * retain the reserved self size. Keep this dependency inside the kit,
     * never in screens. Font/template configuration uses public APIs above. */
    lv_label_t *label = (lv_label_t *)value->label;
    label->size_cache = (lv_point_t){value->width,
        lv_font_get_line_height(lv_obj_get_style_text_font(value->label, 0))};
    label->text_size = label->size_cache;
    label->invalid_size_cache = false;
#if LV_LABEL_LONG_TXT_HINT
    label->hint.line_start = -1;
#endif
    lv_obj_invalidate(value->label);
}

/* Prepare one width per UTF-8 boundary when semantic text or typography
 * changes. Scrolling only scans these cached integers: no font lookup,
 * measurement or allocation is needed at a new chord width (HIG §4). */
static bool value_fit_prepare(kit_value_t *value, const char *text)
{
    const lv_font_t *font = lv_obj_get_style_text_font(value->label, 0);
    int32_t spacing = lv_obj_get_style_text_letter_space(value->label, 0);
    bool same = value->semantic && !strcmp(text, value->semantic);
    if (same && value->fit_font == font && value->fit_spacing == spacing) return true;
    /* Registration survives a failed reservation copy, so fitting must also
     * work before a later update succeeds in reserving the lane. */
    const char *reservation = value->reservation ? value->reservation : "";
    size_t capacity = LV_MAX(strlen(text), strlen(reservation)) + 4;
    if (capacity > value->semantic_capacity) {
        char *semantic = lv_malloc(capacity), *fitted = lv_malloc(capacity);
        int32_t *prefix = lv_malloc(capacity * sizeof(*prefix));
        if (!semantic || !fitted || !prefix) {
            lv_free(semantic); lv_free(fitted); lv_free(prefix);
            return false;
        }
        memcpy(semantic, text, strlen(text) + 1);
        lv_free(value->semantic); lv_free(value->fitted); lv_free(value->fit_prefix);
        value->semantic = semantic;
        value->fitted = fitted;
        value->fit_prefix = prefix;
        value->semantic_capacity = capacity;
    } else if (!same) memmove(value->semantic, text, strlen(text) + 1);
    value->fit_font = font;
    value->fit_spacing = spacing;
    value->fit_ellipsis_width = lv_font_get_glyph_width(font, 0x2026, 0);
    int32_t advance = 0;
    size_t length = strlen(value->semantic);
    for (size_t i = 0; i <= length; i++) value->fit_prefix[i] = -1;
    for (uint32_t offset = 0; offset < length;) {
        uint32_t cp = lv_text_encoded_next(value->semantic, &offset), peek = offset;
        uint32_t next = lv_text_encoded_next(value->semantic, &peek);
        /* Every earlier glyph still has its original next code point. Only
         * this final glyph changes neighbor, so cache its exact advance to
         * U+2026 plus the interglyph tracking, not the original prefix width. */
        int32_t candidate = advance;
        int32_t final_width = lv_font_get_glyph_width(font, cp, 0x2026);
        if (final_width > 0) candidate += final_width + spacing;
        if (value->fit_ellipsis_width > 0) candidate += value->fit_ellipsis_width + spacing;
        if (!lv_label_trims_before_ellipsis(cp))
            value->fit_prefix[offset] = candidate > 0 ? candidate - spacing : 0;
        int32_t width = lv_font_get_glyph_width(font, cp, next);
        if (width > 0) advance += width + spacing;
    }
    value->fit_full_width = advance > 0 ? advance - spacing : 0;
    return true;
}

static const char *value_fit_text(kit_value_t *value, const char *text)
{
    if (!value_fit_prepare(value, text)) return "";
    if (value->fit_full_width <= value->fit) return value->semantic;
    if (value->fit_ellipsis_width > value->fit) return "";
    size_t keep = 0, length = strlen(value->semantic);
    for (size_t i = 1; i <= length; i++) {
        int32_t width = value->fit_prefix[i];
        if (width >= 0 && width <= value->fit) keep = i;
    }
    memcpy(value->fitted, value->semantic, keep);
    memcpy(value->fitted + keep, "\xE2\x80\xA6", 4);
    return value->fitted;
}

void orb_kit_value_set(lv_obj_t *label, const char *text, const char *reserve_template)
{
    if (!label || !lv_obj_check_type(label, &lv_label_class)) return;
    orb_kit_value_roll_finish(label);
    /* DOTS keeps replacement bytes from the previous string. Retire that
     * storage before changing a live buffer, including when a legacy caller
     * changes its long mode after registration. Otherwise an ellipsis refresh
     * can splice an old room name into the next temperature or percentage. */
    bool restore_clip = lv_label_get_long_mode(label) != LV_LABEL_LONG_MODE_CLIP;
    if (restore_clip) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
        /* Long-mode changes retire DOTS only on layout completion. Drain
         * that refresh while its saved bytes still belong to the old text. */
        lv_obj_update_layout(label);
    }
    kit_value_t *value = orb_kit_value_find(label);
    if (!value) {
        value = lv_malloc_zeroed(sizeof(*value));
        if (!value) return;
        value->label = label;
        /* A fixed layout lane remains the caller's lane. Content-sized ink
         * gets exactly the template reservation, independent of its value. */
        int32_t width = lv_obj_get_style_width(label, 0);
        value->minimum_width = width > 0 && width < ORB_SCREEN_D * 2 ? width : 0;
        lv_obj_add_event_cb(label, value_event, LV_EVENT_DELETE, value);
        lv_subject_add_observer_obj(orb_fonts_subject(), value_fonts, label, NULL);
    }
    const char *reservation = reserve_template ? reserve_template : value->reservation;
    value->projected = false;
    if (!reservation) reservation = text ? text : "";
    if (!value->reservation || strcmp(reservation, value->reservation)) {
        size_t bytes = strlen(reservation) + 1;
        char *copy = lv_malloc(bytes);
        if (!copy) return;
        memcpy(copy, reservation, bytes);
        lv_free(value->reservation);
        value->reservation = copy;
        /* A live label never retains ink. Raster preparation is intentionally
         * absent even at rest: a timer/clock can begin changing on any frame. */
        if (lv_obj_get_raster(label)) lv_obj_set_raster(label, false);
        orb_kit_value_measure(value);
    }
    else if (value->measured_font != lv_obj_get_style_text_font(label, 0) ||
             value->measured_spacing != lv_obj_get_style_text_letter_space(label, 0) ||
             value->measured_max_width != lv_obj_get_style_max_width(label, 0)) orb_kit_value_measure(value);
    if (lv_obj_get_raster(label)) lv_obj_set_raster(label, false);
    text = text ? text : "";
    /* A live value in a narrowed chord lane ends in one U+2026 on a whole
     * glyph instead of being hard-clipped mid-glyph ("Codex · 71'"). Only
     * fitted lanes measure; ordinary ticks keep their allocation-free copy. */
    /* A re-sync passes the label's own (possibly truncated) buffer. */
    if (value->fit > 0 && value->semantic && text == lv_label_get_text(label)) text = value->semantic;
    if (value->fit > 0) {
        text = value_fit_text(value, text);
    }
    lv_label_t *native = (lv_label_t *)label;
    if (native->text != value->text) {
        /* Defend against a legacy public setter replacing the owned buffer;
         * migrate back to a reserved live buffer without retaining its lease. */
        value->text = NULL;
        value->text_capacity = 0;
    }
    size_t bytes = strlen(text) + 1;
    /* Replacing one ASCII byte with U+2026 can grow the displayed byte count.
     * Reserve that suffix on first fitting, before scrolling changes widths. */
    size_t capacity = LV_MAX(bytes, strlen(value->reservation ? value->reservation : "") +
        (value->fit > 0 ? 4 : 1));
    if (capacity > value->text_capacity) {
        char *buffer = lv_malloc(capacity);
        if (!buffer) return;
        memcpy(buffer, text, bytes);
        lv_label_set_text_static(label, buffer);
        /* The fork freezes dynamically owned text when enqueueing draw tasks.
         * A static borrowed buffer would let an input tick rewrite bytes that
         * an in-flight band still reads. Transfer ownership to the label while
         * retaining its capacity here; never use a public setter per detent. */
        native->static_txt = 0;
        value->text = buffer;
        value->text_capacity = capacity;
        value->updates++;
    } else if (strcmp(lv_label_get_text(label), text)) {
        /* This buffer belongs to the kit, so native LVGL text can refresh
         * without an allocation at every timer tick or slider detent. The
         * caller's stack/static buffer is never retained. */
        memmove(value->text, text, bytes);
        value_refresh(value);
        value->updates++;
    }
    if (restore_clip && value->text) value_refresh(value);
}

void orb_kit_value_fit(lv_obj_t *label, int32_t width)
{
    kit_value_t *value = orb_kit_value_find(label);
    if (!value || value->fit == width) return;
    /* Keep the untruncated text before the first fitted display replaces it. */
    if (!value->fit && !value_fit_prepare(value, lv_label_get_text(label))) return;
    value->fit = width;
    orb_kit_value_set(label, value->semantic, NULL);
}

void orb_kit_hero_set_value(lv_obj_t *label, const char *text, const char *reserve_template)
{
    orb_kit_value_set(label, text, reserve_template);
}

void orb_kit_value_set_projection(lv_obj_t *label, const char *presented_text,
                                const char *target_text, const char *reserve_template)
{
    orb_kit_value_set(label, presented_text, reserve_template);
    kit_value_t *value = orb_kit_value_find(label);
    if (!value) return;
    lv_snprintf(value->projection, sizeof(value->projection), "%s", target_text ? target_text : "");
    value->projected = true;
}

bool orb_kit_value_projection_semantic_text(lv_obj_t *label, const char **out)
{
    kit_value_t *value = orb_kit_value_find(label);
    if (!value || !value->projected || !out) return false;
    *out = value->projection;
    return true;
}

