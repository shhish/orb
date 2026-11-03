#include "agents_internal.h"

const orb_session_t *agents_focused(agents_state_t *w)
{
    const orb_session_t *s;
    for (unsigned i = 0; i < w->model->session_count; i++) {
        s = &w->model->sessions[i];
        if (!strcmp(s->id, w->session) && w->model->prefs.sources[s->agent]) {
            w->focus_index = i; return s;
        }
    }
    unsigned indices[ORB_MODEL_MAX_SESSIONS];
    if (agents_order(w->model, indices)) {
        s = &w->model->sessions[indices[0]];
        snprintf(w->session, sizeof(w->session), "%s", s->id);
        w->focus_index = indices[0];
        return s;
    }
    return NULL;
}

bool agents_focus_change(agents_state_t *w, int delta)
{
    if (w->surface != ORB_SHELL_AGENTS || !delta) return false;
    const orb_session_t *s = agents_focused(w);
    unsigned count = w->model->session_count;
    if (!s || !count) return false;
    int index = (int)(s - w->model->sessions);
    /* Rotary callbacks can coalesce several detents into one absolute value.
     * Honour their full delta before stepping past any disabled source. */
    index = (int)(((int64_t)index + delta) % count + count) % (int)count;
    for (unsigned i = 0; i < count; i++) {
        if (!w->model->prefs.sources[w->model->sessions[index].agent]) {
            index = (index + (delta > 0 ? 1 : -1) + (int)count) % (int)count;
            continue;
        }
        snprintf(w->session, sizeof(w->session), "%s", w->model->sessions[index].id);
        w->focus_index = (unsigned)index;
        w->has_selection = false;

        agents_refresh(w);
        return true;
    }
    return false;
}

const orb_agent_stream_t *agents_stream(agents_state_t *w, const char *id)
{
    for (unsigned i = 0; i < w->model->stream_count; i++)
        if (!strcmp(w->model->streams[i].session, id)) return &w->model->streams[i];
    return NULL;
}

uint64_t agents_membership(const agents_state_t *w)
{
    uint64_t hash = 1469598103934665603ULL;
    unsigned indices[ORB_MODEL_MAX_SESSIONS];
    unsigned count = agents_order(w->model, indices);
    for (unsigned i = 0; i < count; i++) {
        const orb_session_t *s = &w->model->sessions[indices[i]];
        for (const char *p = s->id; *p; p++) hash = (hash ^ (unsigned char)*p) * 1099511628211ULL;
        hash = (hash ^ s->agent ^ ((unsigned)s->state << 16) ^ (indices[i] << 8)) * 1099511628211ULL;
    }
    return hash ^ w->model->session_count ^ w->model->idle_count;
}

void agents_steps_update(agents_state_t *w, const orb_session_t *s, const orb_agent_stream_t *stream)
{
    unsigned rows = agents_step_rows(), first = stream && stream->steps_count > rows ? stream->steps_count - rows : 0;
    int column = 390, widths[3] = {0}, gap = agents_step_gap();
    for (unsigned i = 0; i < rows; i++) {
        int y = lv_obj_get_y(w->agent_steps[i]);
        int h = lv_obj_get_style_text_font(w->agent_steps[i], 0)->line_height;
        column = LV_MIN(column, orb_chord_width(y, y + h - 1, ORB_RIM_MARGIN_TEXT + 24));
    }
    for (unsigned i = 0; i < rows; i++) {
        const orb_agent_step_t *step = stream && first + i < stream->steps_count ? &stream->steps[first + i] : NULL;
        char added[20] = "", removed[20] = "";
        if (step && step->has_added) agents_count(added, sizeof(added), "+", step->added);
        if (step && step->has_removed) agents_count(removed, sizeof(removed), "−", step->removed);
        orb_kit_value_set_rolling(w->agent_added[i], added, "+888M");
        orb_kit_value_set_rolling(w->agent_removed[i], removed, "−888M");
        int extras = (added[0] ? agents_ink(w->agent_added[i], added) + 8 : 0) +
            (removed[0] ? agents_ink(w->agent_removed[i], removed) + 8 : 0);
        agents_step_bind(w->agent_step_icons[i], w->agent_steps[i], w->agent_step_text[i],
                          sizeof(w->agent_step_text[i]), s->agent, step ? step->state : ORB_STEP_DONE,
                          step ? step->label : i == 0 ? "Waiting for activity" : "",
                          column - gap - extras, step != NULL);
        snprintf(w->agent_step_icon_text[i], sizeof(w->agent_step_icon_text[i]), "%s",
                 step ? agents_step_glyph(step->state) : "");
        widths[i] = (step ? gap : 0) + agents_ink(w->agent_steps[i], w->agent_step_text[i]) + extras;
    }
    agents_wait(w, w->agent_steps[0], agents_ready(w) && (!stream || !stream->steps_count) ?
        "Waiting for activity" : "", 3);
    int block = 0;
    for (unsigned i = 0; i < rows; i++) block = LV_MAX(block, widths[i]);
    int left = 233 - LV_MIN(block, column) / 2;
    for (unsigned i = 0; i < rows; i++) {
        bool glyph = w->agent_step_icon_text[i][0];
        /* An unstarted stream's lone placeholder centres like other empty text. */
        int x = glyph ? left : 233 - widths[i] / 2;
        lv_obj_set_x(w->agent_step_icons[i], x);
        if (glyph) x += gap;
        lv_obj_set_x(w->agent_steps[i], x);
        x += agents_ink(w->agent_steps[i], w->agent_step_text[i]) + 8;
        lv_obj_set_x(w->agent_added[i], x);
        if (lv_label_get_text(w->agent_added[i])[0])
            x += agents_ink(w->agent_added[i], lv_label_get_text(w->agent_added[i])) + 8;
        lv_obj_set_x(w->agent_removed[i], x);
    }
}

void agents_focus_update(agents_state_t *w)
{
    const orb_session_t *s = agents_focused(w);
    if (!s || !w->agent_name) return;
    const orb_agent_stream_t *stream = agents_stream(w, s->id);
    /* The rim setter rebuilds cached ink, so call it only on a real content
     * change, never for the polling/scroll cadence (HIG §§4/8). */
    if ((uintptr_t)lv_obj_get_user_data(w->agent_name) != (unsigned)s->agent + 1) {
        lv_obj_delete(w->agent_name);
        w->agent_name = orb_kit_rim_title_create(w->page, s->title, agents_colour(s->agent));
        lv_obj_set_user_data(w->agent_name, (void *)(uintptr_t)((unsigned)s->agent + 1));
    } else if (strcmp(w->agent_name_text, s->title)) {
        orb_kit_rim_text_set(w->agent_name, s->title, NULL);
    }
    snprintf(w->agent_name_text, sizeof(w->agent_name_text), "%s", s->title);
    /* Polish review #12/#32: the shared unavailable words, then read-only,
     * then the session's own state in words (the old unlabelled orange arc). */
    const char *status = !w->model->online ? ORB_KIT_OFFLINE_CAPTION : !agents_ready(w) ? ORB_KIT_SYNCING_CAPTION :
        stream && stream->stale ? "Last known" : !s->can_message ? ORB_KIT_READ_ONLY_CAPTION : agents_state_text(s->state);
    agents_line(w->agent_status, w->agent_status_text, sizeof(w->agent_status_text), status);
    agents_wait(w, w->agent_status, status, 1);
    lv_obj_update_layout(w->page);
    agents_steps_update(w, s, stream);
    const char *sentence = stream && stream->assistant.text[0] ? stream->assistant.text : "";
    if (strcmp(w->agent_quote_source, sentence)) {
        snprintf(w->agent_quote_source, sizeof(w->agent_quote_source), "%s", sentence);
        char quote[sizeof(w->agent_quote_text)];
        snprintf(quote, sizeof(quote), sentence[0] ? "“%s”" : "%s", sentence[0] ? sentence : "No reply yet");
        agents_fit(w->agent_quote, w->agent_quote_text, sizeof(w->agent_quote_text), quote, true);
    }
    bool talk = agents_talk_available(w, s);
    orb_kit_control_set_enabled(w->agent_talk, talk);
    if (w->agent_caption && lv_obj_is_hidden(w->agent_caption) == talk) lv_obj_set_hidden(w->agent_caption, !talk);
}

/* Review #32: Sessions, talk and the focused session's full stream as one
 * symmetric row of discs on the talk line, the 80 px mic in the middle. */
void agents_disc(agents_state_t *w, const char *glyph, int action, int x)
{
    lv_obj_t *disc = orb_kit_disc_create(w->page, ORB_KIT_DISC_KIND_MINI,
        ORB_KIT_NEUTRAL_GLASS, ORB_KIT_GREY, glyph);
    lv_obj_set_pos(disc, x - ORB_KIT_DISC_MINI / 2, AGENTS_TALK_Y - ORB_KIT_DISC_MINI / 2);
    lv_obj_set_user_data(disc, (void *)(intptr_t)action);
    lv_obj_set_event_bubble(disc, true);
    lv_obj_add_event_cb(disc, agents_clicked, LV_EVENT_CLICKED, w);
    if (w->materialise) orb_glass_materialise(disc, true);
}

void agents(agents_state_t *w)
{
    agents_chrome_clear(w);
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    w->agent_membership = agents_membership(w);
    const orb_session_t *s = agents_focused(w);
    if (!s) {
        /* Glossary §§2.3/4.1: the destination is Sessions. */
        w->heading = orb_kit_rim_title_create(w->page, "Sessions", ORB_KIT_SECONDARY);
        agents_status(w, ORB_ICON_TERMINAL, agents_ready(w) ? "No sessions" : agents_connection_status(w),
                      "Start a session on your Mac", true);
        /* The list would hold only these two rows, so offer the useful one. */
        if (w->model->idle_count) agents_action(w, "Idle sessions", ORB_SHELL_IDLE_SESSIONS, 233, 380, false, false, ORB_KIT_GREY);
        else agents_action(w, "Usage", ORB_SHELL_USAGE, 233, 380, false, false, ORB_KIT_GREY);
        return;
    }
    w->agent_status_text[0] = w->agent_name_text[0] = w->agent_quote_text[0] = '\0';
    w->agent_quote_source[0] = 1; w->agent_quote_source[1] = '\0';
    memset(w->agent_step_text, 0, sizeof(w->agent_step_text));
    memset(w->agent_step_icon_text, 0, sizeof(w->agent_step_icon_text));
    w->agent_name = orb_kit_rim_title_create(w->page, s->title, agents_colour(s->agent));
    lv_obj_set_user_data(w->agent_name, (void *)(uintptr_t)((unsigned)s->agent + 1));
    unsigned rows = agents_step_rows();
    for (unsigned i = 0; i < 3; i++) {
        int y = (ax ? 140 : 108) + i * (ax ? 42 : 34);
        w->agent_step_icons[i] = agents_text(w, "", ORB_TEXT_CAPTION2, true, y, 28, 1, false);
        w->agent_steps[i] = agents_text(w, "", ORB_TEXT_CAPTION2, false, y, 290, 1, true);
        lv_obj_set_clickable(w->agent_steps[i], false);
        w->agent_added[i] = agents_text(w, "", ORB_TEXT_FOOTNOTE2, true, y, 100, 1, false);
        w->agent_removed[i] = agents_text(w, "", ORB_TEXT_FOOTNOTE2, true, y, 100, 1, false);
        lv_obj_set_style_text_color(w->agent_added[i], ORB_GREEN, 0);
        lv_obj_set_style_text_color(w->agent_removed[i], ORB_RED, 0);
        lv_obj_set_width(w->agent_added[i], LV_SIZE_CONTENT);
        lv_obj_set_width(w->agent_removed[i], LV_SIZE_CONTENT);
        if (i >= rows) {
            lv_obj_set_hidden(w->agent_step_icons[i], true); lv_obj_set_hidden(w->agent_steps[i], true);
            lv_obj_set_hidden(w->agent_added[i], true); lv_obj_set_hidden(w->agent_removed[i], true);
        }
    }
    /* Steps, then the state line, then the latest reply: one order at every
     * text size (review #31), all above the 312 px top of the talk discs. */
    int status_y = (ax ? 140 : 108) + (int)rows * (ax ? 42 : 34) + 2;
    w->agent_status = agents_text(w, "", ORB_TEXT_FOOTNOTE2, false, status_y, 326, 1, true);
    lv_obj_update_layout(w->agent_status);
    int quote_y = status_y + lv_obj_get_height(w->agent_status) + 6;
    w->agent_quote = agents_text(w, "", ORB_TEXT_CAPTION1, false, quote_y, ax ? 300 : 314, ax ? 1 : 2, true);
    lv_label_set_long_mode(w->agent_quote, LV_LABEL_LONG_MODE_WRAP);
    w->agent_talk = agents_action(w, ORB_ICON_MIC, ACT_REPLY, 233, AGENTS_TALK_Y, false, true, ORB_KIT_GREY);
    /* Gesture model v3 reserves centre contacts for content, so the list and
     * the full stream are visible, press-responsive controls, not swipes. */
    agents_disc(w, ORB_ICON_LIST, ORB_SHELL_SESSIONS, 233 - 100);
    agents_disc(w, ORB_ICON_ARROW_UP_RIGHT, ACT_FOCUS_OPEN, 233 + 100);
    w->agent_caption = orb_kit_rim_caption_create(w->page, "Hold BOOT to talk");
    agents_focus_update(w);

}

