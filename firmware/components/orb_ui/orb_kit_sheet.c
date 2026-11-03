#include "orb_kit_pages.h"

/* HIG §§1/7: the sheet is content; only its controls use glass. Layer
 * ownership, input capture and the underlying screen's pause stay outside kit. */
lv_obj_t *orb_kit_sheet_surface(lv_obj_t *parent)
{
    lv_obj_t *sheet = lv_obj_create(parent);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, 466, 466);
    lv_obj_set_scrollable(sheet, false);
    lv_obj_set_gesture_bubble(sheet, false);
    lv_obj_set_style_bg_color(sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    return sheet;
}
