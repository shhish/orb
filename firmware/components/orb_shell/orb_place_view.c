#include "orb_place_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "orb_ui.h"

static void charging_percent(float value, char *out, size_t capacity, void *context)
{
    (void)context;
    snprintf(out, capacity, "%d%%", (int)(value + .5f));
}

void orb_charging_view_update(orb_charging_view_t *view, int battery_pct,
                              orb_charging_copy_t state, lv_color_t colour)
{
    char value[24]; snprintf(value, sizeof(value), battery_pct < 0 ? "—" : "%d%%", battery_pct);
    orb_kit_ring_set_formatter(view->ring, charging_percent, NULL);
    orb_kit_ring_set_value_label(view->ring, battery_pct < 0 ? NULL : view->number, "100%");
    orb_kit_ring_set_value(view->ring, LV_CLAMP(0, battery_pct, 100), view->known && battery_pct >= 0);
    if (battery_pct < 0) orb_kit_value_set(view->number, value, "100%");
    view->known = battery_pct >= 0;
    if (!lv_color_eq(lv_obj_get_style_arc_color(view->ring, LV_PART_INDICATOR), colour))
        lv_obj_set_style_arc_color(view->ring, colour, LV_PART_INDICATOR);
    if (!lv_color_eq(lv_obj_get_style_arc_color(view->ring, LV_PART_MAIN), colour))
        lv_obj_set_style_arc_color(view->ring, colour, LV_PART_MAIN);
    lv_obj_set_hidden(view->icon, !state.glyph);
    if (state.glyph && strcmp(lv_label_get_text(view->icon), state.glyph))
        lv_label_set_text(view->icon, state.glyph);
    /* A grey state keeps a white glyph; green/red glyphs share the ring. */
    lv_color_t ink = state.accent == ORB_DELIGHT_GRAY ? ORB_LABEL : colour;
    if (!lv_color_eq(lv_obj_get_style_text_color(view->icon, 0), ink))
        lv_obj_set_style_text_color(view->icon, ink, 0);
    if (view->caption && strcmp(lv_label_get_text(view->caption), state.caption))
        lv_label_set_text(view->caption, state.caption);
}

void orb_place_title_lane(lv_obj_t *title, lv_obj_t *arrival)
{
    /* A transient owns this lane; partial letters behind it are noise. */
    if (title) {
        lv_obj_set_hidden(title, arrival != NULL);
        lv_obj_move_foreground(title);
    }
    if (arrival) lv_obj_move_foreground(arrival);
}

void orb_place_increase_contrast(lv_obj_t *obj)
{
    lv_obj_set_style_text_color(obj, orb_color_increased_contrast(lv_obj_get_style_text_color(obj, 0)), 0);
    lv_color_t background = lv_obj_get_style_bg_color(obj, 0);
    /* App identity fills keep their flat palette values: making a blue circle
     * lighter would reduce contrast against its required white glyph. Neutral
     * surfaces can lift while semantic text uses the stronger palette above. */
    if (background.red == background.green && abs((int)background.blue - background.red) <= 4)
        lv_obj_set_style_bg_color(obj, orb_color_increased_contrast(background), 0);
    /* HIG §6's white 50% rim belongs to bezelled controls. Plain content rows
     * and transparent complications retain their layout without new outlines. */
    if (lv_obj_check_type(obj, &lv_button_class) &&
        (lv_obj_get_style_bg_opa(obj, 0) != LV_OPA_TRANSP || lv_obj_get_style_blur_backdrop(obj, 0))) {
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, lv_color_white(), 0);
        lv_obj_set_style_border_opa(obj, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(obj, orb_color_from_rgb(ORB_RGB_GRAY4_CONTRAST), LV_STATE_PRESSED);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        orb_place_increase_contrast(lv_obj_get_child(obj, i));
}

lv_obj_t *orb_place_layer_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_event_bubble(obj, true);
    return obj;
}

void orb_system_increase_contrast(lv_obj_t *obj)
{
    lv_color_t color = lv_obj_get_style_text_color(obj, 0);
    if (lv_color_eq(color, orb_color_from_rgb(ORB_RGB_SYSTEM_SECONDARY)))
        lv_obj_set_style_text_color(obj, ORB_LABEL, 0);
    color = lv_obj_get_style_bg_color(obj, 0);
    if (lv_color_eq(color, ORB_BLUE)) lv_obj_set_style_bg_color(obj, orb_color_from_rgb(ORB_RGB_BLUE_CONTRAST), 0);
    else if (lv_color_eq(color, ORB_GREEN)) lv_obj_set_style_bg_color(obj, orb_color_from_rgb(ORB_RGB_GREEN_CONTRAST), 0);
    if (lv_obj_check_type(obj, &lv_arc_class))
        lv_obj_set_style_arc_color(obj, orb_color_from_rgb(ORB_RGB_BLUE_CONTRAST), LV_PART_INDICATOR);
    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, ORB_LABEL, 0);
        lv_obj_set_style_border_opa(obj, LV_OPA_50, 0);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        orb_system_increase_contrast(lv_obj_get_child(obj, i));
}

/* Design 1.0 content colour is independent of material opacity. Reduce
 * Transparency changes glass, while Increase Contrast selects this palette. */
lv_color_t orb_place_identity_accent(orb_delight_color_t color, bool system, bool contrast)
{
    if (system) {
        switch (color) {
        case ORB_DELIGHT_ORANGE: return ORB_KIT_CLAUDE;
        case ORB_DELIGHT_BLUE: return ORB_KIT_CODEX;
        case ORB_DELIGHT_INDIGO: return ORB_KIT_DOT;
        case ORB_DELIGHT_YELLOW: return ORB_KIT_HOME;
        case ORB_DELIGHT_TEAL: return ORB_KIT_TIME;
        case ORB_DELIGHT_GREEN: return ORB_KIT_PLAY;
        case ORB_DELIGHT_RED: return ORB_KIT_DATE;
        case ORB_DELIGHT_GRAY: return ORB_KIT_GREY;
        default: break;
        }
    }
    return orb_color_from_rgb(orb_delight_color(color, contrast));
}

lv_color_t orb_place_agent_accent(orb_agent_t agent, bool system, bool contrast)
{
    /* Design 1.0, owner 2026-10-28: identity hues and step states are content. */
    return orb_place_identity_accent(agent == ORB_AGENT_CLAUDE ? ORB_DELIGHT_ORANGE :
        agent == ORB_AGENT_CODEX ? ORB_DELIGHT_BLUE : ORB_DELIGHT_INDIGO, system, contrast);
}
