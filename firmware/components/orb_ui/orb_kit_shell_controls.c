#include "orb_kit_shell_controls.h"
#include "orb_kit.h"

#include <math.h>
#include <string.h>

/* HIG §§2/7: preserve the existing material/typography of reachable recovery
 * and card controls. This extraction introduces no new control behavior. */
lv_obj_t *orb_kit_card_action_create(lv_obj_t *parent)
{
    const int width = 230, height = 72;
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_pos(obj, 233 - width / 2, 399 - height / 2);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

void orb_kit_card_action_finish(lv_obj_t *obj, const char *caption, bool primary,
                                bool disabled, lv_color_t tint, bool materialise)
{
    const int width = 230, height = 72;
    orb_glass_apply(obj, primary ? ORB_GLASS_TINTED : ORB_GLASS_REGULAR, tint);
    if (materialise) orb_glass_materialise(obj, true);
    lv_obj_t *text = lv_label_create(obj);
    orb_text_apply(text, ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED, 0);
    lv_label_set_text(text, caption);
    /* The shell's primary hues remain light enough at the renderer's 70%
     * minimum tint for black emphasized ink (HIG §5), including RT/contrast. */
    lv_obj_set_style_text_color(text, primary ? lv_color_black() : ORB_LABEL, 0);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    int available = width - 20;
    {
        lv_point_t measured;
        const lv_font_t *font = lv_obj_get_style_text_font(text, 0);
        lv_text_get_size(&measured, caption, font, lv_obj_get_style_text_letter_space(text, 0),
                         lv_obj_get_style_text_line_space(text, 0), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (measured.x > available) orb_text_apply(text, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED, 0);
    }
    lv_obj_set_size(text, available, height - 8);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_update_layout(text);
    if (lv_obj_get_height(text) > height - 8) {
        lv_obj_set_height(text, height - 8);
        lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_center(text);
    if (disabled) {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(text, ORB_LABEL_LEGACY_SECONDARY, 0);
    }
}

lv_obj_t *orb_kit_recovery_button_create(lv_obj_t *parent, const char *label,
                                       int32_t cx, int32_t cy, int32_t width)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, ORB_CONTROL);
    lv_obj_set_pos(obj, cx - width / 2, cy - ORB_CONTROL / 2);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_event_bubble(obj, true);
    lv_obj_t *caption = lv_label_create(obj);
    orb_kit_value_info_t live;
    if (orb_kit_value_info(caption, &live)) orb_kit_value_set(caption, label, NULL);
    else if (strcmp(lv_label_get_text(caption), label) != 0) lv_label_set_text(caption, label);
    orb_text_apply(caption, ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED, 0);
    lv_obj_set_style_text_color(caption, ORB_LABEL, 0);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(caption, width - 24);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(caption);
    orb_glass_apply(obj, ORB_GLASS_REGULAR, ORB_BLUE);
    return obj;
}

lv_obj_t *orb_kit_demo_button_create(lv_obj_t *parent, const char *text, int32_t cx, int32_t cy, int32_t w,
                           orb_glass_kind_t kind, lv_color_t tint)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, ORB_CONTROL);
    lv_obj_set_pos(b, cx - w / 2, cy - ORB_CONTROL / 2);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    orb_text_apply(l, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(l, ORB_LABEL, 0);
    lv_obj_center(l);
    lv_obj_set_raster(l, true);
    orb_glass_apply(b, kind, tint);
    return b;
}

lv_obj_t *orb_kit_contact_mark_create(lv_obj_t *parent)
{
    lv_obj_t *mark = lv_obj_create(parent);
    lv_obj_remove_style_all(mark);
    lv_obj_set_scrollable(mark, false);
    lv_obj_set_clickable(mark, false);
    lv_obj_set_event_bubble(mark, true);
    lv_obj_set_size(mark, 32, 32);
    lv_obj_t *arc = lv_arc_create(mark);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, 432, 432);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, ORB_LABEL, LV_PART_INDICATOR);
    return mark;
}

void orb_kit_contact_mark_move(lv_obj_t *mark, int x, int y)
{
    lv_obj_set_pos(mark, x - 16, y - 16);
    lv_obj_t *arc = lv_obj_get_child(mark, 0);
    lv_obj_set_pos(arc, 33 - x, 33 - y);
    float angle = atan2f(y - 233, x - 233) * 180.f / (float)M_PI;
    int degrees = (int)angle + 360;
    lv_arc_set_angles(arc, (degrees - 4) % 360, (degrees + 4) % 360);
}
