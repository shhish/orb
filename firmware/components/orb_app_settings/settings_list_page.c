#include "settings_list_page.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_screen.h"
#include "orb_page.h"
#include "orb_ui.h"

void orb_settings_list_page_begin(lv_obj_t *root, const char *title, orb_settings_list_page_t *page)
{
    lv_obj_clean(root);
    /* A plain wrapper must add neither ink nor an extra semantic control. */
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    page->heading = orb_kit_rim_title_create(root, title, ORB_KIT_SECONDARY);
    lv_obj_update_layout(root);
    page->list = orb_kit_list_page(root,
        &(orb_kit_list_page_config_t){.top = 88, .height = 360, .pad_bottom = 24, .reading = true});
    lv_obj_set_style_pad_top(page->list, ORB_KIT_LIST_INSET, 0);
}

lv_obj_t *orb_settings_list_page_status(lv_obj_t *root, lv_obj_t *list, const char *text)
{
    lv_obj_t *caption = orb_kit_status_caption_create(root, text, 84);
    lv_obj_update_layout(root); /* Positions below are read from resolved coordinates. */
    int32_t top = lv_obj_get_y(caption) + lv_obj_get_height(caption) + 4;
    int32_t bottom = lv_obj_get_y(list) + lv_obj_get_height(list);
    lv_obj_set_y(list, top);
    lv_obj_set_height(list, bottom - top);
    return caption;
}

void orb_settings_list_page_end(const orb_settings_list_page_t *page)
{
    orb_kit_list_page_finish(page->list, 120);
    lv_obj_move_foreground(page->heading);
}

void orb_settings_list_page_focus(lv_obj_t *list, lv_obj_t *row)
{
    if (!list || !row) return;
    lv_obj_update_layout(list);
    /* HIG §§3/7: centre the selected choice, including the first/last option;
     * the same inset makes a one-row destination useful without scrolling. */
    int32_t inset = LV_MAX(0, 233 - lv_obj_get_y(list) - lv_obj_get_height(row) / 2);
    lv_obj_set_style_pad_top(list, inset, 0);
    lv_obj_set_style_pad_bottom(list, LV_MAX(0, lv_obj_get_height(list) - inset - lv_obj_get_height(row)), 0);
    lv_obj_update_layout(list);
    orb_round_list_update(list);
    if (!orb_round_list_scroll_to(list, lv_obj_get_y(row)))
        lv_obj_scroll_to_y(list, lv_obj_get_y(row), LV_ANIM_OFF);
}

void orb_settings_list_page_reveal(lv_obj_t *list, lv_obj_t *row)
{
    if (!list || !row) return;
    lv_obj_update_layout(list);
    lv_area_t viewport, bounds;
    lv_obj_get_coords(list, &viewport);
    lv_obj_get_coords(row, &bounds);
    /* HIG §§3/4/8: keep a changed font choice out of the narrow shoulders.
     * Move only enough to enter the reading band, without recentering it. */
    viewport.y1 = LV_MAX(viewport.y1, ORB_SCREEN_R - 100);
    viewport.y2 = LV_MIN(viewport.y2, ORB_SCREEN_R + 100);
    int32_t scroll = lv_obj_get_scroll_y(list) + LV_MAX(0, bounds.y2 - viewport.y2) -
        LV_MAX(0, viewport.y1 - bounds.y1);
    orb_page_restore(list, &(orb_page_context_t){.scroll = scroll});
    orb_round_list_update(list);
}

int32_t orb_settings_list_page_maximum(const orb_settings_list_page_t *page)
{
    lv_obj_update_layout(page->list);
    return LV_MAX(0, lv_obj_get_scroll_y(page->list) + lv_obj_get_scroll_bottom(page->list));
}

void orb_settings_list_page_scroll(const orb_settings_list_page_t *page, int32_t value)
{
    value = orb_screen_clamp_scroll(value, orb_settings_list_page_maximum(page));
    if (value == lv_obj_get_scroll_y(page->list)) return;
    if (!orb_round_list_scroll_to(page->list, value))
        lv_obj_scroll_to_y(page->list, value, LV_ANIM_OFF);
}
