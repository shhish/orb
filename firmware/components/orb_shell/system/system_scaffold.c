/* Preserve the shared system hierarchy and chord fitting (HIG §§1/3/4/7/8).
 * Layout opportunities remain explicit; retained label styles are not reset. */
#include "system_scaffold.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_status.h"

#include <string.h>

static void visible(lv_obj_t *obj, bool show)
{
    lv_obj_set_hidden(obj, !show);
    orb_kit_scroll_arc_refresh(obj);
}

static void text(lv_obj_t *label, const char *value)
{
    orb_kit_value_info_t live;
    if (orb_kit_value_info(label, &live)) orb_kit_value_set(label, value, NULL);
    else if (strcmp(lv_label_get_text(label), value) != 0) lv_label_set_text(label, value);
}

void orb_system_scaffold_line(orb_system_scaffold_t *s, view_t view, line_t which, const char *value, orb_text_style_t style,
                 orb_text_variant_t variant, unsigned max_lines)
{
    lv_obj_t *label = s->lines[which];
    visible(label, value && value[0]);
    text(label, value ? value : "");
    orb_text_apply(label, style, variant, 0);
    if (which == LINE_CODE && (view == VIEW_DOWNLOAD || view == VIEW_VERIFY))
        orb_kit_value_set(label, value ? value : "", "100%");
    s->max_lines[which] = max_lines;
}

void orb_system_scaffold_primary(orb_system_scaffold_t *s, const char *label, bool enabled)
{
    visible(s->primary, label && label[0]);
    orb_kit_button_set_text(s->primary, label ? label : "");
    if (enabled) lv_obj_remove_state(s->primary, LV_STATE_DISABLED);
    else lv_obj_add_state(s->primary, LV_STATE_DISABLED);
    lv_obj_set_style_opa(s->primary, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s->primary, LV_OPA_50, LV_STATE_DISABLED);
}

static int32_t line_height(orb_system_scaffold_t *s, unsigned i, int32_t width)
{
    lv_obj_t *label = s->lines[i];
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    int32_t step = font->line_height + lv_obj_get_style_text_line_space(label, 0);
    int32_t cap = font->line_height + (int32_t)(s->max_lines[i] - 1) * step;
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_update_layout(label);
    int32_t height = lv_obj_get_height(label);
    if (height > cap) height = cap;
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, height);
    return height;
}

/* Design 1.0 coordinates are panel pixels. Semantic type replaces only the
 * unavailable font sizes; AX3 reflows the same hierarchy (HIG §§3/4/8). */
int32_t orb_system_scaffold_design_line(orb_system_scaffold_t *s, line_t which, int32_t y, int32_t width,
                           lv_color_t color)
{
    lv_obj_t *obj = s->lines[which];
    if (lv_obj_get_parent(obj) != s->system) lv_obj_set_parent(obj, s->system);
    lv_obj_set_size(obj, width, LV_SIZE_CONTENT);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_obj_update_layout(obj);
    int32_t height = lv_obj_get_height(obj);
    int32_t chord = orb_chord_width(y, y + height - 1, ORB_RIM_MARGIN_TEXT);
    if (chord < width) {
        width = LV_MAX(32, chord);
        lv_obj_set_width(obj, width);
        lv_obj_update_layout(obj);
        height = lv_obj_get_height(obj);
    }
    lv_obj_set_pos(obj, (466 - width) / 2, y);
    return y + height;
}

void orb_system_scaffold_layout(orb_system_scaffold_t *s, view_t view)
{
    bool confirm = view == VIEW_FORGET || view == VIEW_UNPAIR;
    bool onboarding = view == VIEW_SETUP || view == VIEW_PAIR;
    bool reading = confirm || onboarding || view == VIEW_CONNECTION || view == VIEW_UPDATES;
    visible(s->reading, reading);
    visible(s->cancel, confirm);
    for (unsigned i = 0; i < LINE_COUNT; i++) {
        lv_obj_t *parent = reading ? s->reading : s->system;
        if (lv_obj_get_parent(s->lines[i]) != parent) lv_obj_set_parent(s->lines[i], parent);
    }
    orb_kit_place_bottom_arc(s->primary);
    orb_text_apply(lv_obj_get_child(s->cancel, 0), ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, 0);
    if (reading) {
        /* HIG §§3/7/8: consequences are readable content, never an ellipsis.
         * Reserve both equal neutral decisions and scroll the complete copy. */
        lv_obj_set_size(s->reading, 356, confirm ? 118 : onboarding ? 211 : 197);
        lv_obj_set_pos(s->reading, 55, 143);
        int32_t y = 0;
        for (unsigned i = 0; i < LINE_COUNT; i++) {
            lv_obj_t *label = s->lines[i];
            if (lv_obj_is_hidden(label)) continue;
            lv_obj_set_width(label, 356);
            lv_obj_set_height(label, LV_SIZE_CONTENT);
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_update_layout(label);
            lv_obj_set_pos(label, 0, y);
            y += lv_obj_get_height(label) + 16;
        }
        lv_obj_set_height(s->reading, orb_kit_reader_whole_lines(s->reading, lv_obj_get_height(s->reading)));
        lv_obj_update_layout(s->reading);
        lv_obj_move_foreground(s->primary);
        lv_obj_move_foreground(s->cancel);
        return;
    }
    /* HIG §§3/4/8: keep the numeric hierarchy at every text size. Reflow
     * supporting copy instead of demoting codes or the download percentage. */
    int32_t heights[LINE_COUNT] = { 0 }, total = 0;
    int32_t gap = 7;
    unsigned count = 0;
    for (unsigned i = 0; i < LINE_COUNT; i++) {
        if (lv_obj_is_hidden(s->lines[i])) continue;
        heights[i] = line_height(s, i, 398);
        total += heights[i];
        count++;
    }
    if (count > 1) total += (int32_t)(count - 1) * gap;
    bool has_primary = !lv_obj_is_hidden(s->primary);
    int32_t top = 143;
    int32_t bottom = has_primary ? 340 : 392;
    if (view == VIEW_SETUP || view == VIEW_PAIR) bottom = 410;
    if (view == VIEW_DOWNLOAD) bottom = 406;
    if (view == VIEW_MAINTENANCE) bottom = 187;
    if (total > bottom - top) {
        total -= count > 1 ? (int32_t)(count - 1) * (gap - 3) : 0;
        gap = 3;
    }
    /* Recovery guidance stays visible at AX3. Only the variable explanation
     * may use fewer lines, with explicit ellipsis rather than lost reassurance. */
    while (total > bottom - top && s->max_lines[LINE_BODY] > 1 &&
           !lv_obj_is_hidden(s->lines[LINE_BODY])) {
        total -= heights[LINE_BODY];
        s->max_lines[LINE_BODY]--;
        heights[LINE_BODY] = line_height(s, LINE_BODY, 398);
        total += heights[LINE_BODY];
    }
    int32_t y = top + (bottom - top - total) / 2;
    if (y < top) y = top;
    for (unsigned i = 0; i < LINE_COUNT; i++) {
        lv_obj_t *label = s->lines[i];
        if (lv_obj_is_hidden(label)) continue;
        int32_t width = orb_chord_width(y, y + heights[i] - 1, ORB_RIM_MARGIN_TEXT + 4);
        width = LV_CLAMP(32, width, 398);
        int32_t height = line_height(s, i, width);
        /* Recheck the chord against the final wrapped height, not just its top. */
        width = LV_CLAMP(32, orb_chord_width(y, y + height - 1, ORB_RIM_MARGIN_TEXT + 4), 398);
        height = line_height(s, i, width);
        lv_obj_set_pos(label, (ORB_SCREEN_D - width) / 2, y);
        y += height + gap;
    }
}

void orb_system_scaffold_layout_status(orb_system_scaffold_t *s, const orb_shell_snapshot_t *status)
{
    lv_obj_update_layout(s->wifi);
    lv_obj_update_layout(s->battery);
    int32_t wifi_w = lv_obj_get_width(s->wifi);
    int32_t battery_w = lv_obj_get_width(s->battery);
    if (status->battery_pct >= 0) {
        int32_t x = (ORB_SCREEN_D - wifi_w - battery_w - 18) / 2;
        lv_obj_set_pos(s->wifi, x, 64);
        lv_obj_set_pos(s->battery, x + wifi_w + 18, 64);
    } else {
        lv_obj_update_layout(s->power);
        int32_t power_w = lv_obj_get_width(s->power);
        int32_t x = (ORB_SCREEN_D - wifi_w - power_w - 18) / 2;
        lv_obj_set_pos(s->wifi, x, 64);
        lv_obj_set_pos(s->power, x + wifi_w + 18, 64);
    }
}

void orb_system_scaffold_status(orb_system_scaffold_t *s, const orb_shell_snapshot_t *status)
{
    const orb_shell_snapshot_t *m = status;
    orb_wifi_set_rssi(s->wifi, m->rssi, m->net == ORB_SHELL_NET_CONNECTED);
    visible(s->battery, m->battery_pct >= 0);
    visible(s->power, m->battery_pct < 0);
    if (m->battery_pct >= 0) orb_battery_set_state(s->battery, m->battery_pct, m->charging, false);
    text(s->power, m->usb ? "USB" : "-" );

}

void orb_system_scaffold_identity(orb_system_scaffold_t *s, bool shown)
{
    visible(s->system_title, shown);
    if (!shown) return;
    orb_kit_rim_text_set(s->system_title, lv_label_get_text(s->lines[LINE_TITLE]), NULL);
    visible(s->lines[LINE_TITLE], false);
}

void orb_system_scaffold_create(orb_system_scaffold_t *s, lv_obj_t *system,
    lv_event_cb_t clicked, void *context)
{
    s->system = system;
    s->reading = lv_obj_create(s->system);
    lv_obj_remove_style_all(s->reading);
    lv_obj_set_scroll_dir(s->reading, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s->reading, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_chain(s->reading, false);
    lv_obj_set_event_bubble(s->reading, true);
    orb_kit_scroll_arc_attach(s->reading);
    /* Review #1: a real scroll view; continuing copy fades, never hard-cut. */
    if (ORB_UI_SOFT_EDGES) orb_scroll_edge_follow(s->reading, 16, 24); else orb_scroll_edge_apply(s->reading, 0, 24);
    s->ring = orb_kit_ring_create(s->system, 218, 10, ORB_KIT_CODEX);
    for (unsigned i = 0; i < LINE_COUNT; i++) {
        s->lines[i] = lv_label_create(s->system);
        lv_obj_set_style_text_align(s->lines[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s->lines[i], ORB_LABEL, 0);
    }
    s->system_title = orb_kit_rim_title_create(s->system, "Orb", ORB_KIT_GREY);
    s->art = orb_kit_disc_create(s->system, ORB_KIT_DISC_STANDARD,
                               ORB_KIT_ACCENT_FILL, ORB_KIT_CODEX, ORB_ICON_DESKTOP);
    lv_obj_set_clickable(s->art, false);
    s->wifi = orb_wifi_create(s->system, ORB_TEXT_BODY, ORB_TEXT_REGULAR);
    s->battery = orb_battery_create(s->system, ORB_TEXT_BODY, ORB_TEXT_REGULAR);
    orb_battery_set_percentage(s->battery, true);
    s->power = lv_label_create(s->system);
    orb_text_apply(s->power, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, 0);
    s->settings = orb_kit_disc_create(s->system, ORB_KIT_DISC_STANDARD,
                                     ORB_KIT_NEUTRAL_GLASS, ORB_KIT_GREY, ORB_ICON_GEAR);
    lv_obj_set_pos(s->settings, 325, 61);
    lv_obj_set_event_bubble(s->settings, true);
    lv_obj_add_event_cb(s->settings, clicked, LV_EVENT_CLICKED, context);
    s->primary = orb_kit_button_primary_create(s->system, "Updates", ORB_KIT_CODEX);
    lv_obj_set_event_bubble(s->primary, true);
    lv_obj_add_event_cb(s->primary, clicked, LV_EVENT_CLICKED, context);
}
