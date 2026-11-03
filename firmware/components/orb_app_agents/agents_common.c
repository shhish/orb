#include "agents_internal.h"

lv_obj_t *agents_status(agents_state_t *w, const char *glyph, const char *title,
                               const char *detail, bool action)
{
    lv_obj_t *state = orb_kit_state_create(w->page, &(orb_kit_state_config_t){
        .glyph = glyph, .title = title, .detail = detail,
        .secondary = ORB_KIT_SECONDARY, .secondary_opa = ORB_KIT_SECONDARY_OPA,
        .top = 88, .bottom = action ? 292 : 378,
    });
    w->agent_empty = state;
    agents_state_wait(w, state, title, 0);
    return state;
}

/* Review #6: every talk control is one 80 px disc on the 352 px line, so the
 * "Hold BOOT to talk" rim caption below it keeps a 20 px gap (HIG §3). */

lv_obj_t *agents_action(agents_state_t *w, const char *text, int action,
                                int x, int y, bool primary, bool glyph, lv_color_t colour)
{
    lv_obj_t *control = agents_kit_action(w, text, action, x, y, primary, glyph, colour);
    if (w->materialise) orb_glass_materialise(control, true);
    return control;
}

void agents_chrome_clear(agents_state_t *w)
{
    if (w->heading) { lv_obj_delete(w->heading); w->heading = NULL; }
    if (w->heading_glyph) { lv_obj_delete(w->heading_glyph); w->heading_glyph = NULL; }
    if (w->navigation) { lv_obj_delete(w->navigation); w->navigation = NULL; }
}

lv_obj_t *agents_text(agents_state_t *w, const char *text, orb_text_style_t style,
                              bool bold, int y, int width, unsigned lines, bool secondary)
{
    lv_obj_t *obj = agents_label(w->page, text, style, bold, 233, y, width, lines,
                           secondary ? ORB_KIT_SECONDARY : ORB_LABEL);
    if (secondary) lv_obj_set_style_text_opa(obj, ORB_KIT_SECONDARY_OPA, 0);
    return obj;
}

/* Review #12: the mic and "Hold BOOT to talk" exist only as a pair. */
bool agents_talk_available(agents_state_t *w, const orb_session_t *s)
{
    if (!s || !agents_ready(w) || !s->can_message || !w->model->prefs.sources[s->agent]) return false;
    for (unsigned i = 0; i < w->model->session_count; i++)
        if (!strcmp(w->model->sessions[i].id, s->id)) return true;
    return false;
}

/* Review #31: the latest steps as one left-aligned column, centred as a
 * block inside the narrowest row chord. Count rails use their actual ink,
 * so "+42 −8" never takes the label's room (review #31, step-counts). */
lv_obj_t *agents_row(agents_state_t *w, const char *title, const char *detail,
                             orb_agent_t agent, int action)
{
    orb_kit_row_config_t config = {.title = title, .subtitle = detail,
        .glyph = agents_icon(agent), .accent = agents_colour(agent)};
    lv_obj_t *row = orb_kit_row_create(w->list, &config);
    lv_obj_set_user_data(row, (void *)(intptr_t)action);
    lv_obj_add_event_cb(row, agents_clicked, LV_EVENT_CLICKED, w);
    return row;
}

const orb_session_t *agents_target(agents_state_t *w, const char *target)
{
    for (size_t i = 0; i < w->model->session_count; i++)
        if (!strcmp(w->model->sessions[i].id, target)) return &w->model->sessions[i];
    if (w->has_selection && !strcmp(w->selection->id, target)) return w->selection;
    return NULL;
}

lv_obj_t *agents_capsule(agents_state_t *w, const char *caption, int action,
                               int cx, int cy, bool primary, lv_color_t accent_colour)
{
    lv_obj_t *control = agents_action(w, caption, action, cx, cy, primary, false, accent_colour);
    /* HIG §§7/9: the kit handles contact retirement; availability still belongs
     * to the action boundary, including retained offline review transcripts. */
    if ((action >= ACT_OPTION && action < ACT_OPTION + ORB_MODEL_MAX_OPTIONS && !agents_ready(w)) ||
        (action == ACT_REPLY && !agents_ready(w)))
        orb_kit_control_set_enabled(control, false);
    return control;
}

/* Review #27: two equal-status choices share the wider one's width. They sit
 * side by side (cancel role leading) when both fit the 6 o'clock chord, and
 * otherwise stack with the cancel role at 6 o'clock, as HIG §7 alerts put
 * Cancel last. Returns the pair's top edge for the reading viewport above. */
int agents_pair(lv_obj_t *cancel, lv_obj_t *other)
{
    lv_obj_update_layout(cancel); lv_obj_update_layout(other);
    int width = LV_MAX(lv_obj_get_width(cancel), lv_obj_get_width(other));
    orb_kit_button_set_min_width(cancel, width);
    orb_kit_button_set_min_width(other, width);
    lv_obj_update_layout(cancel); lv_obj_update_layout(other);
    width = LV_MAX(lv_obj_get_width(cancel), lv_obj_get_width(other));
    const int h = ORB_KIT_SECONDARY_H;
    if (2 * width + 20 <= orb_chord_width(356 - h / 2, 356 + h / 2, 12)) {
        lv_obj_set_pos(cancel, 233 - 10 - width, 356 - h / 2);
        lv_obj_set_pos(other, 233 + 10, 356 - h / 2);
        return 356 - h / 2;
    }
    lv_obj_set_pos(other, 233 - width / 2, 308 - h / 2);
    lv_obj_set_pos(cancel, 233 - width / 2, 380 - h / 2);
    return 308 - h / 2;
}


lv_obj_t *agents_secondary_text(agents_state_t *w, const char *text, int y, int width, int lines)
{
    return agents_text(w, text, ORB_TEXT_FOOTNOTE2, false, y, width, lines, true);
}

lv_obj_t *agents_reading(agents_state_t *w, int x, int y, int width, int height)
{
    lv_obj_t *area = agents_prose(w, y, height);
    lv_obj_set_pos(area, x, y); lv_obj_set_width(area, width);
    lv_obj_set_clickable(area, true);
    lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_OFF);
    return area;
}

int agents_orb_kit_paragraph(lv_obj_t *parent, const char *text, orb_text_style_t style,
                            bool bold, int y, lv_color_t colour, bool centred)
{
    int bottom = orb_kit_paragraph(parent, text, style, bold, y, colour, centred);
    if (lv_color_eq(colour, ORB_KIT_SECONDARY))
        lv_obj_set_style_text_opa(lv_obj_get_child(parent, lv_obj_get_child_count(parent) - 1),
                                 ORB_KIT_SECONDARY_OPA, 0);
    return bottom;
}

/* Review #24/#25: one result layout for card answers, session messages and
 * Time saves. The outcome has a symbol as well as words (HIG §5: never colour
 * alone); failures offer Try again above Done, everything else ends on Done.
 * Neither action resends anything: Try again only returns to the decision. */
void agents_result(agents_state_t *w, const orb_result_copy_t *copy, const char *decision,
                          const char *body, unsigned surface, const char *identity, bool retry)
{
    bool failed = copy->outcome == ORB_RESULT_FAILED || copy->outcome == ORB_RESULT_UNKNOWN;
    lv_obj_t *done = agents_capsule(w, "Done", ACT_RESULT_DONE, 233, 380, false, ORB_KIT_GREY);
    int top = 380 - ORB_KIT_SECONDARY_H / 2;
    if (retry && failed)
        top = agents_pair(done, agents_capsule(w, "Try again", ACT_RESULT_RETRY, 233, 380, false, ORB_KIT_GREY));
    lv_obj_t *area = agents_reading(w, 68, 112, 330, top - 12 - 112);
    lv_obj_t *symbol = lv_label_create(area);
    w->result_symbol = symbol; w->result_kind = (unsigned)copy->outcome + 1;
    orb_icon_apply(symbol, ORB_TEXT_TITLE2, ORB_TEXT_EMPHASIZED, 0);
    lv_label_set_text(symbol, copy->glyph);
    lv_obj_set_style_text_color(symbol, copy->outcome == ORB_RESULT_DONE ? ORB_GREEN :
                                copy->outcome == ORB_RESULT_FAILED ? ORB_RED : ORB_KIT_SECONDARY, 0);
    lv_obj_set_style_text_align(symbol, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(symbol, 330);
    lv_obj_set_event_bubble(symbol, true);
    lv_obj_update_layout(symbol);
    orb_kit_pop_in_arrival(symbol);
    if (copy->outcome == ORB_RESULT_DONE) orb_kit_check_label_arrival(symbol);
    int y = lv_obj_get_height(symbol) + 4;
    y = agents_orb_kit_paragraph(area, copy->title, ORB_TEXT_TITLE3, true, y, ORB_LABEL, true) + 4;
    w->wait_result = lv_obj_get_child(area, lv_obj_get_child_count(area) - 1);
    agents_wait(w, w->wait_result, copy->title, 3);
    if (decision && decision[0])
        y = agents_orb_kit_paragraph(area, decision, ORB_TEXT_CAPTION1, true, y, ORB_LABEL, true) + 4;
    if (copy->detail[0])
        y = agents_orb_kit_paragraph(area, copy->detail, ORB_TEXT_CAPTION1, false, y, ORB_LABEL, true) + 6;
    if (body && body[0]) agents_orb_kit_paragraph(area, body, ORB_TEXT_CAPTION1, false, y, ORB_KIT_SECONDARY, true);
    orb_kit_scroll_arc_attach(area);
    agents_reading_restore(w, area, surface, identity);
}


/* Delight §6: only real, named service waits breathe. "Waiting for activity"
 * is a passive missing-stream placeholder, including during Working; it has
 * no pending service operation and stays still. Slots survive live binding. */
static bool waiting(agents_state_t *w, const char *text, unsigned slot)
{
    unsigned kind = !strcmp(text, "Loading…") || !strcmp(text, "Loading") ? 1 :
        !strcmp(text, "Waiting for Dot’s reply") ? 2 :
        !strcmp(text, "Waiting for your Mac to sync") || !strcmp(text, "Syncing") ||
        !strcmp(text, ORB_KIT_SYNCING_CAPTION) ? 3 :
        !strcmp(text, "Sending…") || !strcmp(text, "Queued") ? 4 : 0;
    if (kind != w->waits[slot].kind) {
        w->waits[slot].kind = kind;
        w->waits[slot].started = lv_tick_get();
    }
    return kind && lv_tick_elaps(w->waits[slot].started) < 30000;
}

void agents_wait(agents_state_t *w, lv_obj_t *label, const char *text, unsigned slot)
{
    bool active = waiting(w, text, slot);
    if (label) orb_kit_breathe_since(label, active, w->waits[slot].started);
}

void agents_state_wait(agents_state_t *w, lv_obj_t *state, const char *text, unsigned slot)
{
    bool active = waiting(w, text, slot);
    if (state) orb_kit_state_set_waiting_since(state, active, w->waits[slot].started);
}
