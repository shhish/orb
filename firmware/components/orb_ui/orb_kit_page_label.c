#include "orb_kit_pages.h"

#include <stdlib.h>

#include "src/misc/lv_text_private.h"

lv_obj_t *orb_kit_page_label(lv_obj_t *parent, const char *value, orb_text_style_t style,
                       bool bold, int x, int y, int width, int lines, lv_color_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    orb_text_apply(obj, style, bold ? ORB_TEXT_EMPHASIZED : ORB_TEXT_REGULAR, 0);
    lv_label_set_text(obj, value);
    lv_obj_set_style_text_color(obj, color, 0);
    if (lv_color_eq(color, ORB_KIT_SECONDARY)) lv_obj_set_style_text_opa(obj, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
    int h = font->line_height + (lines - 1) *
            (font->line_height + lv_obj_get_style_text_line_space(obj, 0));
    int chord = orb_chord_width(y, y + h - 1, ORB_RIM_MARGIN_TEXT + 4) - 2 * abs(x - 233);
    if (width > chord) width = chord;
    width = LV_MAX(16, width);
    lv_obj_set_size(obj, width, h);
    lv_obj_set_pos(obj, x - width / 2, y);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_DOTS);
    /* Balance only a caption that fits its lines; an ellipsised one keeps
     * LVGL's own last line (its dots must not become the semantic text). */
    lv_point_t size;
    lv_text_get_size(&size, value, font, lv_obj_get_style_text_letter_space(obj, 0),
                     lv_obj_get_style_text_line_space(obj, 0), width, LV_TEXT_FLAG_NONE);
    if (lines > 1 && size.y <= h) orb_kit_label_balance(obj);
    lv_obj_set_event_bubble(obj, true);
    return obj;
}

