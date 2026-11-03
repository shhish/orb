#include "orb_kit_pages.h"

/* HIG §§4/7/8: full consequences remain scrollable at AX3; the host supplies
 * neutral choices and requires explicit activation with physical provenance. */
void orb_kit_confirm_content(lv_obj_t *reading, const char *consequences)
{
    lv_obj_set_pos(reading, 66, 126);
    lv_obj_set_size(reading, 334, 122);
    lv_obj_t *copy = lv_label_create(reading);
    orb_text_apply(copy, ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR, 0);
    lv_obj_set_width(copy, 334);
    lv_label_set_long_mode(copy, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(copy, ORB_KIT_SECONDARY, 0);
    lv_obj_set_style_text_opa(copy, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_set_style_text_align(copy, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(copy, consequences);
    orb_kit_scroll_arc_attach(reading);
}

lv_obj_t *orb_kit_confirm_action(lv_obj_t *parent, const char *text, int center_y)
{
    /* No primary/default option: dangerous recovery decisions remain equal. */
    lv_obj_t *button = orb_kit_button_secondary_create(parent, text);
    if (!button) return NULL;
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, center_y - ORB_KIT_SECONDARY_H / 2);
    return button;
}
