#include "orb_kit_pages.h"

/* HIG §§1–4/7: preserve the reading chord and the caller's action lane.
 * Routing, scroll context and the viewport lifetime belong to orb_screen. */
lv_obj_t *orb_kit_detail_page(lv_obj_t *parent,
                               const orb_kit_detail_page_config_t *config)
{
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_set_scrollable(area, false);
    lv_obj_set_clickable(area, false);
    lv_obj_set_event_bubble(area, true);
    lv_obj_set_pos(area, 60, config->top);
    lv_obj_set_size(area, 346, config->height);
    lv_obj_set_scrollable(area, true);
    if (config->system) lv_obj_set_clickable(area, true);
    lv_obj_set_scroll_dir(area, LV_DIR_VER);
    lv_obj_set_scroll_momentum(area, true);
    lv_obj_set_scrollbar_mode(area, config->system ? LV_SCROLLBAR_MODE_OFF : LV_SCROLLBAR_MODE_AUTO);
    if (ORB_UI_SOFT_EDGES) orb_scroll_edge_follow(area, 24, 24);
    else orb_scroll_edge_apply(area, 0, 12);
    if (config->arc) orb_kit_scroll_arc_attach(area);
    /* Polish review #1: never rest on a half line under the fade. */
    orb_kit_reader_snap(area);
    return area;
}

int orb_kit_paragraph(lv_obj_t *parent, const char *text, orb_text_style_t style,
                       bool bold, int y, lv_color_t color, bool center)
{
    lv_obj_t *obj = lv_label_create(parent);
    orb_text_apply(obj, style, bold ? ORB_TEXT_EMPHASIZED : ORB_TEXT_REGULAR, 0);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_obj_set_style_text_align(obj, center ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_update_layout(parent);
    lv_obj_set_width(obj, lv_obj_get_width(parent) - 12);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(obj, 6, y);
    /* Centred paragraphs are headings and captions: no widowed last word. */
    if (center) orb_kit_label_balance(obj);
    lv_obj_update_layout(obj);
    lv_obj_set_event_bubble(obj, true);
    if (lv_obj_is_scrollable(parent)) orb_kit_reader_snap(parent);
    return y + lv_obj_get_height(obj) + 6;
}
