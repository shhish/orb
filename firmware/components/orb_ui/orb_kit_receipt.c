#include "orb_kit_pages.h"

lv_obj_t *orb_kit_receipt_mark(lv_obj_t *area)
{
    lv_obj_t *mark = lv_obj_create(area);
    lv_obj_remove_style_all(mark);
    lv_obj_set_scrollable(mark, false);
    lv_obj_set_clickable(mark, false);
    lv_obj_set_event_bubble(mark, true);
    lv_obj_set_pos(mark, 24, 12);
    lv_obj_set_size(mark, 38, 34);
    return mark;
}

/* HIG §§4/8: outcomes stay in reading order. Correlation, result vocabulary
 * and dismissal lifetime are framework/app policy, never a visual timer. */
void orb_kit_receipt_content(lv_obj_t *area, const orb_kit_receipt_config_t *config)
{
    int y = orb_kit_paragraph(area, config->title, ORB_TEXT_TITLE2, true,
                              0, ORB_LABEL, true) + 10;
    y = orb_kit_paragraph(area, config->decision, ORB_TEXT_FOOTNOTE1, true,
                          y, config->secondary, true);
    if (config->answer && config->answer[0])
        y = orb_kit_paragraph(area, config->answer, ORB_TEXT_BODY, false,
                              y, ORB_LABEL, false);
    orb_kit_paragraph(area, config->explanation, ORB_TEXT_BODY, false,
                      y + 10, ORB_LABEL, false);
    lv_obj_update_layout(area);
}
