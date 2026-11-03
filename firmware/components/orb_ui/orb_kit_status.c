#include "orb_kit.h"

#include <stdio.h>
#include <string.h>

#include "src/misc/lv_text_private.h"

/* HIG §8 / polish review #12: the one unavailable-state treatment shared by
 * every surface. Kept apart from orb_kit.c for its size guard rail. */
void orb_kit_control_set_enabled(lv_obj_t *obj, bool enabled)
{
    if (!obj) return;
    if (enabled) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else {
        /* A held contact must not complete after the control is retired. */
        orb_kit_press_cancel(obj);
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
    /* Ink only: the glass keeps its material, never alpha-fading (HIG §2). */
    orb_kit_disc_set_ink_opa(obj, enabled ? LV_OPA_COVER : ORB_KIT_DISABLED_OPA);
}

lv_obj_t *orb_kit_status_caption_create(lv_obj_t *parent, const char *text, int32_t y)
{
    lv_obj_t *caption = lv_label_create(parent);
    orb_text_apply(caption, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(caption, ORB_KIT_SECONDARY, 0);
    lv_obj_set_style_text_opa(caption, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_DOTS);
    text = text ? text : "";
    const lv_font_t *font = lv_obj_get_style_text_font(caption, 0);
    int32_t height = font->line_height;
    int32_t width = orb_chord_width(y, y + height - 1, ORB_RIM_MARGIN_TEXT);
    /* Never an ellipsis mid-word (review #9): when "Reason · detail" does not
     * fit its chord (AX3), keep the whole reason and drop the detail. */
    char fitted[96];
    snprintf(fitted, sizeof(fitted), "%s", text);
    lv_point_t size;
    lv_text_get_size(&size, fitted, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    char *detail = strstr(fitted, " · ");
    if (size.x > width && detail) *detail = '\0';
    lv_label_set_text(caption, fitted);
    lv_obj_set_size(caption, width, height);
    lv_obj_set_pos(caption, ORB_SCREEN_R - width / 2, y);
    lv_obj_set_clickable(caption, false);
    lv_obj_set_event_bubble(caption, true);
    return caption;
}
