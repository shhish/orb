/* Existing reading/list compositions, with screen-owned value anchors. */
#include "agents_internal.h"

lv_obj_t *agents_plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_event_bubble(obj, true);
    return obj;
}

lv_obj_t *agents_label(lv_obj_t *parent, const char *value, orb_text_style_t style,
    bool bold, int x, int y, int width, int lines, lv_color_t color)
{
    return orb_kit_page_label(parent, value, style, bold, x, y, width, lines, color);
}

lv_obj_t *agents_prose(agents_state_t *w, int y, int h)
{
    lv_obj_t *area = orb_kit_detail_page(w->page, &(orb_kit_detail_page_config_t){
        .top = y, .height = h, .system = false, .arc = false,
    });
    w->list = area;
    w->curved = false;
    return area;
}

bool agents_ready(const agents_state_t *w) { return w->model->online && w->model->synced; }

const char *agents_connection_status(const agents_state_t *w)
{
    if (!w->net_connected) return "Wi-Fi offline";
    if (!w->model->online || !w->link_online) return "Mac offline";
    return agents_ready(w) ? "Connected" : "Syncing";
}

const orb_session_t *agents_selected_session(agents_state_t *w)
{
    for (size_t i = 0; i < w->model->session_count; i++)
        if (!strcmp(w->session, w->model->sessions[i].id)) return &w->model->sessions[i];
    if (w->has_selection && !strcmp(w->session, w->selection->id)) return w->selection;
    return NULL;
}

const orb_card_t *agents_selected_card(agents_state_t *w)
{
    for (size_t i = 0; i < w->model->card_count; i++)
        if (!strcmp(w->card, w->model->cards[i].id)) return &w->model->cards[i];
    return NULL;
}

bool agents_voice_supported(agents_state_t *w, const char *target, const char *card_id)
{
    if (card_id && card_id[0]) {
        for (size_t i = 0; i < w->model->card_count; i++)
            if (!strcmp(card_id, w->model->cards[i].id))
                return w->model->cards[i].reply && !w->model->cards[i].answered;
        return false;
    }
    for (size_t i = 0; i < w->model->session_count; i++)
        if (!strcmp(target, w->model->sessions[i].id))
            return w->model->prefs.sources[w->model->sessions[i].agent] && w->model->sessions[i].can_message;
    return w->selection_external && w->has_selection && !strcmp(target, w->selection->id) &&
        w->model->prefs.sources[w->selection->agent] && w->selection->can_message;
}

lv_obj_t *agents_kit_action(agents_state_t *w, const char *text, int action,
    int x, int y, bool primary, bool glyph, lv_color_t colour)
{
    lv_obj_t *obj = glyph ? orb_kit_glyph_button_create(w->page, text, primary, colour) :
        primary ? orb_kit_button_primary_create(w->page, text, colour) :
                  orb_kit_button_secondary_create(w->page, text);
    lv_obj_update_layout(obj);
    lv_obj_set_pos(obj, x - lv_obj_get_width(obj) / 2, y - lv_obj_get_height(obj) / 2);
    lv_obj_set_user_data(obj, (void *)(intptr_t)action);
    lv_obj_set_event_bubble(obj, true);
    lv_obj_add_event_cb(obj, agents_clicked, LV_EVENT_CLICKED, w);
    return obj;
}

static void reading_scrolled(lv_event_t *event)
{
    agents_state_t *w = lv_event_get_user_data(event);
    w->reading_scroll = LV_MAX(0, lv_obj_get_scroll_y(lv_event_get_current_target_obj(event)));
}

void agents_reading_restore(agents_state_t *w, lv_obj_t *area, unsigned surface, const char *identity)
{
    uint32_t key = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)identity; *p; p++) key = (key ^ *p) * 16777619u;
    w->reading_identity = key;
    w->reading_scroll = 0;
    w->restore_pending = true;
    lv_obj_set_user_data(area, (void *)(uintptr_t)surface);
    lv_obj_add_event_cb(area, reading_scrolled, LV_EVENT_SCROLL, w);
}

static lv_obj_t *idle_more(agents_state_t *w, const char *title, const char *detail, const char *glyph, int action)
{
    lv_obj_t *row = orb_kit_row_create(w->list, &(orb_kit_row_config_t){
        .title = title, .subtitle = detail, .glyph = glyph, .accent = ORB_KIT_GREY});
    lv_obj_set_user_data(row, (void *)(intptr_t)action);
    lv_obj_set_event_bubble(row, true);
    if (action) lv_obj_add_event_cb(row, agents_clicked, LV_EVENT_CLICKED, w);
    else lv_obj_set_clickable(row, false);
    return row;
}

static void idle_footer(agents_state_t *w)
{
    bool pending = w->idle && w->idle->pending;
    orb_idle_more_copy_t copy = orb_idle_more_copy(w->idle, w->model->online, agents_ready(w), pending,
        w->idle && w->idle->error[0]);
    lv_obj_t *more = idle_more(w, copy.title, copy.detail, copy.glyph, ACT_IDLE_MORE);
    w->wait_row = more; w->wait_row_role = ORB_KIT_ROW_TEXT_TITLE;
    agents_wait(w, NULL, copy.title, 0);
    orb_kit_row_set_waiting_since(more, ORB_KIT_ROW_TEXT_TITLE,
        !strcmp(copy.title, "Loading…"), w->waits[0].started);
    if (pending || !agents_ready(w) || !w->idle) lv_obj_add_state(more, LV_STATE_DISABLED);
}

void agents_idle(agents_state_t *w)
{
    agents_chrome_clear(w);
    w->heading = orb_kit_rim_title_create(w->page, "Idle sessions", ORB_KIT_SECONDARY);
    w->list = orb_kit_list_page(w->page, &(orb_kit_list_page_config_t){.top = 88, .height = 340});
    lv_obj_set_style_pad_top(w->list, ORB_KIT_LIST_INSET, 0);
    w->curved = true;
    unsigned count = 0;
    if (w->idle) for (unsigned i = 0; i < w->idle->count; i++) {
        const orb_session_t *session = &w->idle->sessions[i];
        if (!w->model->prefs.sources[session->agent]) continue;
        char detail[128];
        snprintf(detail, sizeof(detail), "%s · %s", agents_ready(w) ? session->project : "Last known",
            agents_state_text(session->state));
        agents_row(w, session->title, detail, session->agent, ACT_IDLE_SESSION + i);
        count++;
    }
    if (!w->idle || w->idle->cursor[0] || w->idle->error[0] || w->idle->pending) idle_footer(w);
    else if (!count) idle_more(w, "No idle sessions", "", ORB_ICON_TERMINAL, 0);
    lv_obj_set_style_pad_bottom(w->list, 120, 0);
    orb_kit_list_apply(w->list);
    orb_kit_scroll_arc_attach(w->list);
    agents_reading_restore(w, w->list, ORB_SHELL_IDLE_SESSIONS, "idle-sessions");
}
