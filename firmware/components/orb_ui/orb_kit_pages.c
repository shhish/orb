#include "orb_kit_pages.h"

static void list_edge(lv_event_t *event)
{
    lv_obj_t *list = lv_event_get_current_target_obj(event);
    if (!ORB_UI_SOFT_EDGES)
        orb_scroll_edge_apply(list, lv_obj_get_scroll_y(list) > 0 ? 16 : 0, 24);
}

lv_obj_t *orb_kit_list_page(lv_obj_t *parent,
                                  const orb_kit_list_page_config_t *config)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_scrollable(list, false);
    lv_obj_set_clickable(list, false);
    lv_obj_set_event_bubble(list, true);
    lv_obj_set_size(list, 466, config->height);
    lv_obj_set_pos(list, 0, config->top);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_clickable(list, true);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    if (config->reading) {
        lv_obj_set_scroll_momentum(list, true);
        lv_obj_set_scroll_elastic(list, !orb_ui_reduce_motion());
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_pad_bottom(list, config->pad_bottom, 0);
        if (ORB_UI_SOFT_EDGES) orb_scroll_edge_follow(list, 32, 48);
        else {
            orb_scroll_edge_apply(list, 0, 24);
            lv_obj_add_event_cb(list, list_edge, LV_EVENT_SCROLL, NULL);
        }
    } else {
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_momentum(list, true);
        if (config->pad_bottom) lv_obj_set_style_pad_bottom(list, config->pad_bottom, 0);
    }
    return list;
}

void orb_kit_list_page_finish(lv_obj_t *list, int pad_bottom)
{
    lv_obj_set_style_pad_bottom(list, pad_bottom, 0);
    orb_kit_list_apply(list);
    orb_kit_scroll_arc_attach(list);
}

void orb_kit_place_bottom_arc(lv_obj_t *obj)
{
    lv_obj_update_layout(obj);
    /* Retain the centre when Dynamic Type changes a capsule's width. */
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, 380 - lv_obj_get_height(obj) / 2);
}

void orb_kit_place_arc_pair(lv_obj_t *a, lv_obj_t *b)
{
    /* Timer.dc.html's device-pixel positions are authoritative over the
     * system sheet's approximate ±42-degree prose. */
    lv_obj_update_layout(a);
    lv_obj_update_layout(b);
    lv_obj_set_pos(a, 154 - lv_obj_get_width(a) / 2, 356 - lv_obj_get_height(a) / 2);
    lv_obj_set_pos(b, 312 - lv_obj_get_width(b) / 2, 356 - lv_obj_get_height(b) / 2);
}
