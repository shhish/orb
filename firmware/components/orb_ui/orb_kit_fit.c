#include "orb_kit.h"
#include "orb_ui_private.h"
#include "src/misc/lv_text_private.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    lv_obj_t *label;
    int32_t width;
    unsigned max_lines;
    orb_kit_text_fit_info_t info;
} kit_hero_t;

static void hero_event(lv_event_t *event);

static kit_hero_t *hero_state(lv_obj_t *label)
{
    if (!label) return NULL;
    for (unsigned i = 0; i < lv_obj_get_event_count(label); i++) {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(label, i);
        if (lv_event_dsc_get_cb(descriptor) == hero_event)
            return lv_event_dsc_get_user_data(descriptor);
    }
    return NULL;
}

static unsigned hero_lines(const char *text, const lv_font_t *font,
                            lv_text_attributes_t *attributes, bool *width_fits)
{
    unsigned lines = 0;
    *width_fits = true;
    while (*text) {
        unsigned length = lv_text_get_next_line(text, LV_TEXT_LEN_MAX, font, NULL, attributes);
        if (!length) break;
        if (lv_text_get_line_width(text, length, font, attributes) > attributes->max_width)
            *width_fits = false;
        lines++;
        bool last_newline = text[length - 1] == '\n' && text[length] == '\0';
        text += length;
        if (last_newline) lines++;
    }
    return LV_MAX(1, lines);
}

static void hero_layout(kit_hero_t *hero)
{
    const orb_text_style_t steps[] = {ORB_TEXT_TITLE3, ORB_TEXT_HEADLINE, ORB_TEXT_CAPTION2};
    lv_obj_t *label = hero->label;
    const char *text = lv_label_get_text(label);
    lv_point_t size = {0};
    bool width_fits = false;
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        orb_text_style_t style = steps[i];
        const lv_font_t *font = orb_font_variant(style, ORB_TEXT_EMPHASIZED);
        lv_text_attributes_t attributes = {
            .letter_space = orb_text_style_info(style)->letter_space,
            .line_space = orb_text_line_space(style, ORB_TEXT_EMPHASIZED),
            .max_width = hero->width,
        };
        hero->info.style = style;
        hero->info.lines = hero_lines(text, font, &attributes, &width_fits);
        hero->info.complete = width_fits && hero->info.lines <= hero->max_lines;
        lv_text_get_size_attributes(&size, text, font, &attributes);
        if (hero->info.complete) break;
    }
    /* No discarded tail: an impossible width/line budget is reported
     * explicitly, preserving semantic text for the caller to recompose. */
    orb_text_apply(label, hero->info.style, ORB_TEXT_EMPHASIZED, 0);
    lv_label_set_max_lines(label, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_size(label, hero->width, size.y);
    /* HIG §4: same lines, no widow. Balancing never adds a line, so the
     * measured height and the fit report above stay exact. */
    orb_kit_label_balance(label);
    lv_obj_update_layout(label);
}

static void hero_fonts(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)subject;
    kit_hero_t *hero = hero_state(lv_observer_get_target_obj(observer));
    if (hero) hero_layout(hero);
}

static void hero_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE) orb_ui_free(lv_event_get_user_data(event));
}

bool orb_kit_hero_fit(lv_obj_t *label, const char *text, int32_t width,
                       unsigned max_lines, orb_kit_text_fit_info_t *out)
{
    if (!label || !lv_obj_check_type(label, &lv_label_class) || width < 1 ||
        max_lines < 2 || max_lines > 3) return false;
    kit_hero_t *hero = hero_state(label);
    if (!hero) {
        hero = orb_ui_alloc(sizeof(*hero));
        if (!hero) return false;
        memset(hero, 0, sizeof(*hero));
        hero->label = label;
        if (!lv_obj_add_event_cb(label, hero_event, LV_EVENT_DELETE, hero)) {
            orb_ui_free(hero);
            return false;
        }
        /* The object observer owns its own deletion lifetime. */
        hero->width = width;
        hero->max_lines = max_lines;
        lv_label_set_text(label, text ? text : "");
        lv_subject_add_observer_obj(orb_fonts_subject(), hero_fonts, label, NULL);
    } else {
        hero->width = width;
        hero->max_lines = max_lines;
        lv_label_set_text(label, text ? text : "");
    }
    hero_layout(hero);
    if (out) *out = hero->info;
    return hero->info.complete;
}

bool orb_kit_hero_fit_info(lv_obj_t *label, orb_kit_text_fit_info_t *out)
{
    kit_hero_t *hero = hero_state(label);
    if (!hero || !out) return false;
    *out = hero->info;
    return true;
}

/* HIG §§4/8: static quick-action identities retain their distinguishing tail;
 * never run a marquee or shrink below the caller's semantic text style. */
bool orb_kit_label_middle_ellipsis(lv_obj_t *label, const char *text)
{
    if (!label || !text || !lv_obj_check_type(label, &lv_label_class)) return false;
    lv_obj_update_layout(label);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    lv_text_attributes_t attributes = {.letter_space=lv_obj_get_style_text_letter_space(label, 0)};
    int32_t width = lv_obj_get_content_width(label);
    if (width < 1) return false;
    size_t length = strlen(text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    if (lv_text_get_width(text, length, font, &attributes) <= width) {
        lv_label_set_text(label, text);
        return true;
    }
    char *display = orb_ui_alloc(length + sizeof("…"));
    if (!display) return false;
    uint32_t left = 0;
    for (uint32_t i = 0; i < lv_text_get_encoded_length(text) / 2; i++)
        lv_text_encoded_next(text, &left);
    uint32_t right = left;
    bool trim_prefix = true;
    do {
        if ((trim_prefix && left) || right == length) {
            do { left--; } while (left && ((unsigned char)text[left] & 0xc0) == 0x80);
        } else if (right < length) lv_text_encoded_next(text, &right);
        memcpy(display, text, left);
        memcpy(display + left, "…", sizeof("…") - 1);
        memcpy(display + left + sizeof("…") - 1, text + right, length - right + 1);
        trim_prefix = !trim_prefix;
    } while ((left || right < length) && lv_text_get_width(display, strlen(display), font, &attributes) > width);
    bool fits = lv_text_get_width(display, strlen(display), font, &attributes) <= width;
    lv_label_set_text(label, display);
    orb_ui_free(display);
    return fits;
}
