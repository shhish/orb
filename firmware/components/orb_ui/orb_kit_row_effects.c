#include "orb_kit_row.h"
#include "orb_kit_effects.h"
#include "orb_ui_private.h"

/* Named waits promote native ink only once. The overlay preserves the
 * prepared row, layout and hit map while the shared clock changes its ink. */
static lv_obj_t *waiting_label(lv_obj_t *row, orb_kit_row_text_role_t role, bool waiting)
{
    kit_row_t *state = orb_kit_row_state(row);
    if (!state || role > ORB_KIT_ROW_TEXT_VALUE) return NULL;
    if (waiting && !state->live[role]) {
        lv_obj_t *native[] = {state->title, state->subtitle, state->value};
        if (native[role]) orb_kit_row_set_live_text(row, role, lv_label_get_text(native[role]), NULL);
    }
    return state->live[role];
}

void orb_kit_row_set_live_accent_animated(lv_obj_t *row, lv_color_t fill, lv_color_t ink)
{
    kit_row_t *state = orb_kit_row_state(row);
    if (!state) return;
    if (orb_kit_disc_light_up_matches(state->live_disc, fill, ink)) return;
    lv_color_t before = lv_obj_get_style_bg_color(state->live_disc ? state->live_disc : state->disc, 0);
    orb_kit_row_set_live_accent(row, fill, ink);
    if (!lv_obj_has_state(row, LV_STATE_PRESSED)) orb_kit_disc_light_up(state->live_disc, before, fill);
}

void orb_kit_row_live_accent_cancel(lv_obj_t *row)
{
    kit_row_t *state = orb_kit_row_state(row);
    if (state) orb_kit_disc_light_up_cancel(state->live_disc);
}

void orb_kit_row_set_waiting(lv_obj_t *row, orb_kit_row_text_role_t role, bool waiting)
{
    orb_kit_breathe(waiting_label(row, role, waiting), waiting);
}

void orb_kit_row_set_waiting_since(lv_obj_t *row, orb_kit_row_text_role_t role, bool waiting, uint32_t started)
{
    orb_kit_breathe_since(waiting_label(row, role, waiting), waiting, started);
}

void orb_kit_row_set_text_colour(lv_obj_t *row, orb_kit_row_text_role_t role, lv_color_t colour)
{
    kit_row_t *state = orb_kit_row_state(row);
    if (!state || role > ORB_KIT_ROW_TEXT_VALUE) return;
    lv_obj_t *labels[] = {state->title, state->subtitle, state->value};
    lv_obj_t *label = state->live[role] ? state->live[role] : labels[role];
    if (!label) return;
    lv_opa_t opacity = lv_obj_has_state(row, LV_STATE_DISABLED) ? ORB_KIT_DISABLED_OPA :
        lv_color_eq(colour, ORB_KIT_SECONDARY) ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER;
    bool colour_changed = !lv_color_eq(lv_obj_get_style_text_color(label, 0), colour);
    bool opacity_changed = lv_obj_get_style_text_opa(label, 0) != opacity;
    if (colour_changed) lv_obj_set_style_text_color(label, colour, 0);
    if (opacity_changed) orb_kit_live_ink_opa(label, opacity);
    if (!state->live[role] && (colour_changed || opacity_changed)) orb_round_list_update(state->list);
}

void orb_kit_row_set_focus(lv_obj_t *row, bool focused)
{
    kit_row_t *state = orb_kit_row_state(row);
    if (state && state->disc) orb_kit_disc_set_focus(state->disc, focused);
}
