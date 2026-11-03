#include "agents_internal.h"

uint64_t agents_idle_hash(const agents_state_t *w)
{
    if (!w->idle) return 0;
    uint64_t hash = 1469598103934665603ULL;
    const unsigned char *bytes = (const unsigned char *)w->idle;
    for (size_t i = 0; i < sizeof(*w->idle); i++) hash = (hash ^ bytes[i]) * 1099511628211ULL;
    return hash;
}

void agents_session_detail(agents_state_t *w, const orb_session_t *s, char *detail, size_t capacity)
{
    const char *state = !agents_ready(w) ? "Last known" : s->reason[0] ? s->reason : agents_state_text(s->state);
    if (s->agent == ORB_AGENT_DOT) snprintf(detail, capacity, "%s", state);
    else snprintf(detail, capacity, "%s · %s", agents_provider(s->agent), state);
}

void agents_sessions(agents_state_t *w)
{
    agents_chrome_clear(w);
    w->heading = orb_kit_rim_title_create(w->page, "Sessions", ORB_KIT_SECONDARY);
    w->agent_membership = agents_membership(w);
    memset(w->session_list_rows, 0, sizeof(w->session_list_rows));
    memset(w->session_list_names, 0, sizeof(w->session_list_names));
    memset(w->session_list_details, 0, sizeof(w->session_list_details));
    unsigned indices[ORB_MODEL_MAX_SESSIONS];
    unsigned count = agents_order(w->model, indices);
    if (!count && !w->model->idle_count) {
        /* Review #15/#28: the whole hint, in the shared empty template, with
         * the list's only other destination as its one action. */
        agents_status(w, ORB_ICON_TERMINAL, agents_ready(w) ? "No sessions" : agents_connection_status(w),
                      "Start a session on your Mac", true);
        agents_action(w, "Usage", ORB_SHELL_USAGE, 233, 380, false, false, ORB_KIT_GREY);
        return;
    }
    w->list = agents_plain(w->page);
    lv_obj_set_size(w->list, 466, 340); lv_obj_set_pos(w->list, 0, 88);
    lv_obj_set_scrollable(w->list, true); lv_obj_set_clickable(w->list, true);
    lv_obj_set_scroll_dir(w->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(w->list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_momentum(w->list, true);
    /* Review #33: every list's first row rests just below the top fade. */
    lv_obj_set_style_pad_top(w->list, ORB_KIT_LIST_INSET, 0);
    w->curved = true;
    for (unsigned position = 0; position < count; position++) {
        unsigned i = indices[position];
        const orb_session_t *s = &w->model->sessions[i];
        char detail[128]; agents_session_detail(w, s, detail, sizeof(detail));
        lv_obj_t *r = agents_row(w, s->title, detail, s->agent,
            s->agent == ORB_AGENT_DOT ? ORB_SHELL_DOT : ACT_SESSION + i);
        w->session_list_rows[i] = r;
        snprintf(w->session_list_names[i], sizeof(w->session_list_names[i]), "%s", s->title);
        snprintf(w->session_list_details[i], sizeof(w->session_list_details[i]), "%s", detail);
    }
    if (w->idle) {

        w->agent_idle_hash = agents_idle_hash(w);
        for (unsigned i = 0; i < w->idle->count; i++) {
            const orb_session_t *s = &w->idle->sessions[i];
            if (!w->model->prefs.sources[s->agent]) continue;
            char detail[100]; snprintf(detail, sizeof(detail), "%s · %s", agents_provider(s->agent), agents_state_text(s->state));
            agents_row(w, s->title, detail, s->agent, ACT_IDLE_SESSION + i);
        }
    }
    if (w->model->idle_count && (!w->idle || w->idle->pending || w->idle->error[0] || w->idle->cursor[0])) {
        /* Review #33: a neutral destination row, not a provider identity. */
        const char *detail = !w->idle ? "" : w->idle->pending ? "Loading…" :
            w->idle->error[0] ? "Couldn’t load · Try again" : "Show more";
        orb_kit_row_config_t idle = {.title = "Idle sessions", .subtitle = detail,
            .glyph = ORB_ICON_CLOCK, .accent = ORB_KIT_GREY};
        lv_obj_t *row = orb_kit_row_create(w->list, &idle);
        w->wait_row = row; w->wait_row_role = ORB_KIT_ROW_TEXT_SUBTITLE;
        bool pending = w->idle && w->idle->pending;
        agents_wait(w, NULL, pending ? "Loading…" : "", 0);
        orb_kit_row_set_waiting_since(row, ORB_KIT_ROW_TEXT_SUBTITLE, pending, w->waits[0].started);
        lv_obj_set_user_data(row, (void *)(intptr_t)(w->idle ? ACT_IDLE_MORE : ORB_SHELL_IDLE_SESSIONS));
        lv_obj_add_event_cb(row, agents_clicked, LV_EVENT_CLICKED, w);
    }
    /* Usage remains a named destination in the visible content hierarchy;
     * there is no hidden centre-drag navigation owner. */
    orb_kit_row_config_t usage = {.title = "Usage", .subtitle = "Claude & Codex limits",
        .glyph = ORB_ICON_HOURGLASS, .accent = ORB_KIT_GREY};
    lv_obj_t *usage_row = orb_kit_row_create(w->list, &usage);
    lv_obj_set_user_data(usage_row, (void *)(intptr_t)ORB_SHELL_USAGE);
    lv_obj_add_event_cb(usage_row, agents_clicked, LV_EVENT_CLICKED, w);
    lv_obj_set_style_pad_bottom(w->list, 160, 0);
    orb_kit_list_apply(w->list);
    if (ORB_UI_SOFT_EDGES) orb_scroll_edge_follow(w->list, 38, 38);
    orb_kit_scroll_arc_attach(w->list);
    agents_reading_restore(w, w->list, ORB_SHELL_SESSIONS, "sessions");
}

void agents_stream_update(agents_state_t *w)
{
    const orb_session_t *s = agents_selected_session(w);
    if (!s || !w->list || !w->stream_rows[0]) return;
    bool in_focus = false;
    for (unsigned i = 0; i < w->model->session_count; i++)
        if (!strcmp(w->model->sessions[i].id, s->id)) in_focus = true;
    bool talk = agents_talk_available(w, s);
    if (w->agent_talk) orb_kit_control_set_enabled(w->agent_talk, talk);
    if (w->agent_caption) lv_obj_set_hidden(w->agent_caption, !talk);
    const orb_agent_stream_t *stream = agents_stream(w, s->id);
    agents_line(w->agent_status, w->agent_status_text, sizeof(w->agent_status_text),
               !in_focus ? "Outside focus" : !w->model->online ? ORB_KIT_OFFLINE_CAPTION :
               !agents_ready(w) ? ORB_KIT_SYNCING_CAPTION : stream && stream->stale ? "Last known" :
               !s->can_message ? ORB_KIT_READ_ONLY_CAPTION : agents_state_text(s->state));
    agents_wait(w, w->agent_status, w->agent_status_text, 1);
    bool bottom = lv_obj_get_scroll_bottom(w->list) < 4;
    int top = lv_obj_get_scroll_y(w->list);
    uint64_t revision = w->stream_log->revision;
    if (!orb_model_get_stream_log(s->id, w->stream_log)) memset(w->stream_log, 0, sizeof(*w->stream_log));
    if (w->agent_empty) lv_obj_set_hidden(w->agent_empty, w->stream_log->count != 0);
    agents_wait(w, w->agent_empty, agents_ready(w) && !w->stream_log->count ? "Waiting for activity" : "", 3);
    if (revision == w->stream_log->revision) return;
    int y = 0, width = lv_obj_get_width(w->list);
    for (unsigned i = 0; i < ORB_MODEL_STREAM_ROWS; i++) {
        lv_obj_t *r = w->stream_rows[i], *t = w->stream_text[i];
        if (i >= w->stream_log->count) { lv_obj_set_hidden(r, true); continue; }
        const orb_stream_row_t *entry = &w->stream_log->rows[i];
        lv_obj_set_hidden(r, false);
        char value[ORB_MODEL_TEXT_MAX + 32];
        bool step = entry->kind == ORB_STREAM_STEP, user = entry->kind == ORB_STREAM_USER;
        snprintf(value, sizeof(value), "%s", entry->text);
        lv_obj_set_hidden(w->stream_icons[i], !step);
        lv_obj_set_width(r, user ? width - 28 : width);
        lv_obj_set_x(r, user ? 18 : 0);
        int icon_gap = step ? agents_step_gap() : 0;
        lv_obj_set_width(t, user ? width - 60 : step ? width - icon_gap : width - 10);
        lv_obj_set_style_bg_opa(r, user ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        /* Review #31: one component binds step rows in both views. */
        if (step) agents_step_bind(w->stream_icons[i], t, w->stream_buffers[i],
            sizeof(w->stream_buffers[i]), s->agent, entry->state, value, width - icon_gap, true);
        else {
            lv_obj_set_style_text_color(t, ORB_KIT_SECONDARY, 0);
            lv_obj_set_style_text_opa(t, ORB_KIT_SECONDARY_OPA, 0);
            orb_text_apply(t, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 0);
            lv_obj_set_height(t, LV_SIZE_CONTENT);
            lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
            agents_set(t, w->stream_buffers[i], sizeof(w->stream_buffers[i]), value);
        }
        lv_obj_update_layout(t);
        int h = lv_obj_get_height(t) + (user ? 20 : 0);
        lv_obj_set_height(r, h); lv_obj_set_y(r, y);
        lv_obj_set_pos(t, user ? 16 : icon_gap, user ? 10 : 0);
        y += h + (step ? 12 : 14);
    }
    lv_obj_update_layout(w->list);
    if (bottom) lv_obj_scroll_to_y(w->list, LV_COORD_MAX, LV_ANIM_OFF);
    else lv_obj_scroll_to_y(w->list, top, LV_ANIM_OFF);
}

void agents_session(agents_state_t *w)
{
    agents_chrome_clear(w);
    const orb_session_t *s = agents_selected_session(w);
    if (!s) {
        w->heading = orb_kit_rim_title_create(w->page, "Sessions", ORB_KIT_SECONDARY);
        agents_status(w, ORB_ICON_TERMINAL, "Session unavailable", "Find it in Sessions on your Mac", true);
        agents_action(w, "Sessions", ORB_SHELL_SESSIONS, 233, 380, false, false, ORB_KIT_GREY);
        return;
    }
    w->heading = orb_kit_rim_title_create(w->page, s->title, agents_colour(s->agent));
    /* HIG §3: the longest120° title occupies the upper shoulders. Reading
     * starts below that ink at every type size; the full stream still scrolls. */
    /* The separate status line clears the mic even at AX3. Keep the full
     * stream in its own faded viewport above that line (HIG §§3/4/8). */
    lv_obj_t *area = agents_prose(w, 140, 118);
    lv_obj_set_pos(area, 60, 140); lv_obj_set_width(area, 346);
    lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_OFF);
    memset(w->stream_buffers, 0, sizeof(w->stream_buffers));
    for (unsigned i = 0; i < ORB_MODEL_STREAM_ROWS; i++) {
        lv_obj_t *r = agents_plain(area); w->stream_rows[i] = r;
        lv_obj_set_style_radius(r, 20, 0); lv_obj_set_style_bg_color(r, ORB_GRAY6, 0);
        lv_obj_t *t = lv_label_create(r); w->stream_text[i] = t;
        lv_obj_set_style_text_opa(t, ORB_KIT_SECONDARY_OPA, 0);
        orb_text_apply(t, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 0);
        lv_label_set_text_static(t, w->stream_buffers[i]);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_height(t, LV_SIZE_CONTENT);
        lv_obj_set_event_bubble(r, true); lv_obj_set_event_bubble(t, true);
        w->stream_icons[i] = lv_label_create(r);
        orb_icon_apply(w->stream_icons[i], ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED, 0);
        lv_obj_set_pos(w->stream_icons[i], 0, 0);
        lv_obj_set_hidden(w->stream_icons[i], true);
        lv_obj_set_hidden(r, true);
    }
    w->agent_status_text[0] = '\0';
    w->agent_status = agents_text(w, "", ORB_TEXT_FOOTNOTE2, false, 266, 320, 1, true);
    w->agent_empty = lv_label_create(area);
    orb_text_apply(w->agent_empty, ORB_TEXT_BODY, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(w->agent_empty, "Waiting for activity");
    lv_obj_set_width(w->agent_empty, 336);
    lv_obj_set_style_text_color(w->agent_empty, ORB_KIT_SECONDARY, 0);
    lv_obj_set_style_text_opa(w->agent_empty, ORB_KIT_SECONDARY_OPA, 0);
    w->stream_log->revision = UINT64_MAX;
    agents_stream_update(w);
    w->agent_talk = agents_action(w, ORB_ICON_MIC, ACT_REPLY, 233, AGENTS_TALK_Y, false, true, ORB_KIT_GREY);
    bool talk = agents_talk_available(w, s);
    if (!talk) orb_kit_control_set_enabled(w->agent_talk, false);
    /* Review #6/#31: the same talk pair as the Agents glance. */
    if (talk) w->agent_caption = orb_kit_rim_caption_create(w->page, "Hold BOOT to talk");
    orb_kit_scroll_arc_attach(area);
    agents_reading_restore(w, area, ORB_SHELL_SESSION, s->id);

}

