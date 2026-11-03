#include "orb_input.h"

#include "orb_fonts.h"
#include "orb_ui.h"

static const char s_synthetic_marker;
static lv_obj_t *s_hint;
static lv_timer_t *s_hint_timer;

void orb_input_mark_synthetic(lv_indev_t *input)
{
    if (input) lv_indev_set_user_data(input, (void *)&s_synthetic_marker);
}

bool orb_input_is_synthetic(const lv_indev_t *input)
{
    return input && lv_indev_get_user_data(input) == &s_synthetic_marker;
}

bool orb_input_active_is_synthetic(void)
{
    return orb_input_is_synthetic(lv_indev_active());
}

static void hint_deleted(lv_event_t *event)
{
    LV_UNUSED(event);
    s_hint = NULL;
    if (s_hint_timer) lv_timer_delete(s_hint_timer);
    s_hint_timer = NULL;
}

static void hint_expired(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    lv_obj_delete(s_hint);
}

bool orb_input_require_physical(lv_obj_t *surface)
{
    if (!orb_input_active_is_synthetic()) return true;
    if (!s_hint) {
        s_hint = lv_obj_create(lv_display_get_layer_top(lv_obj_get_display(surface)));
        lv_obj_remove_style_all(s_hint);
        lv_obj_set_clickable(s_hint, false);
        lv_obj_set_scrollable(s_hint, false);
        /* HIG §§3/4/8: the neutral hint briefly takes the central title lane,
         * between the diagonal controls. AX3 wraps here instead of obscuring
         * any action, including vertically stacked Allow and Deny buttons. */
        lv_obj_set_style_bg_color(s_hint, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_hint, LV_OPA_COVER, 0);
        lv_obj_set_size(s_hint, 190, 100);
        lv_obj_set_pos(s_hint, 138, 40);
        lv_obj_t *caption = lv_label_create(s_hint);
        lv_obj_set_clickable(caption, false);
        lv_label_set_text(caption, "Answer on Orb");
        orb_text_apply(caption, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, 0);
        lv_obj_set_style_text_color(caption, ORB_LABEL, 0);
        lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(caption, 190);
        lv_obj_center(caption);
        lv_obj_add_event_cb(s_hint, hint_deleted, LV_EVENT_DELETE, NULL);
        s_hint_timer = lv_timer_create(hint_expired, 1400, NULL);
    } else lv_timer_reset(s_hint_timer);
    lv_obj_move_foreground(s_hint);
    return false;
}
