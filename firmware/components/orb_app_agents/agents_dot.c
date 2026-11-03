#include "agents_internal.h"

bool agents_dot_values(agents_state_t *w, const char **question, const char **answer)
{
    *question = NULL; *answer = NULL;
    /* Dot's own bounded ring survives a later send to another session. Keep
     * the returned text in this instance's pre-sized stream copy, never a
     * temporary stack snapshot or the global most-recent send receipt. */
    if (!w->fixture && orb_model_get_stream_log("dot", w->stream_log))
        for (size_t i = w->stream_log->count; i > 0; i--)
            if (w->stream_log->rows[i - 1].kind == ORB_STREAM_USER) {
                *question = w->stream_log->rows[i - 1].text; break;
            }
    if (!*question && w->model->last_message.accepted && !strcmp(w->model->last_message.message.target, "dot"))
        *question = w->model->last_message.message.text;
    for (size_t i = 0; i < w->model->stream_count; i++)
        if (!strcmp(w->model->streams[i].session, "dot")) *answer = w->model->streams[i].assistant.text;
    if (!*answer || !(*answer)[0]) for (size_t i = w->model->card_count; i > 0; i--) {
        const orb_card_t *c = &w->model->cards[i - 1];
        if (!strcmp(c->session, "dot") && c->kind == ORB_CARD_MESSAGE) { *answer = c->body; break; }
    }
    const orb_session_t *dot = agents_target(w, "dot");
    if ((!*answer || !(*answer)[0]) && dot && dot->summary[0]) *answer = dot->summary;
    bool content = (*question && (*question)[0]) || (*answer && (*answer)[0]);
    if (!*answer || !(*answer)[0]) {
        *answer = dot && dot->summary[0] ? dot->summary : !w->model->online ?
            "Mac offline" : !agents_ready(w) ? "Waiting for your Mac to sync" : "Waiting for Dot’s reply";
    }
    if (!*question) *question = "";
    return content;
}

void agents_dot_update(agents_state_t *w)
{
    if (!w->dot_answer || !w->dot_question || !w->dot_bubble || !w->list) return;
    /* Dot can leave the eight-session focus set after its recent-state grace.
     * Discover its actual grant through ordinary idle pages, rather than
     * inventing availability or publishing an extra out-of-focus stream. */
    bool discovered = false;
    for (unsigned i = 0; i < w->model->session_count; i++)
        if (w->model->sessions[i].agent == ORB_AGENT_DOT) discovered = true;
    if (!discovered && w->idle)
        for (unsigned i = 0; i < w->idle->count; i++)
            if (w->idle->sessions[i].agent == ORB_AGENT_DOT) { discovered = true; break; }
    bool checking = !discovered && w->idle && w->idle->pending && agents_ready(w);
    const char *question, *answer;
    bool content = agents_dot_values(w, &question, &answer);
    bool listening = agents_voice_supported(w, "dot", "");
    const char *empty_title = !w->model->online ? "Mac offline" : !agents_ready(w) ? "Loading…" :
        listening ? "Ask Dot" : checking ? "Loading…" : "Dot isn’t connected";
    const char *empty_detail /* #14: what is happening, not a promise */ = !w->model->online ?
        "Connect your Mac to reach Dot" : !agents_ready(w) ? "Waiting for your Mac to sync" : listening ?
        "Hold BOOT to talk" : checking ? "Checking your Mac for Dot" : "Connect Dot on your Mac to talk";
    /* Review #5/#28: the shared flow template, so AX3 wraps instead of overlapping. */
    if (!content && (!w->agent_empty || strcmp(lv_label_get_text(lv_obj_get_child(w->agent_empty, 1)), empty_title))) {
        if (w->agent_empty) lv_obj_delete(w->agent_empty);
        w->agent_empty = agents_status(w, ORB_ICON_CHAT, empty_title, empty_detail, true);
    }
    if (w->agent_empty) lv_obj_set_hidden(w->agent_empty, content);
    agents_state_wait(w, w->agent_empty, content ? "" : empty_title, 0);
    lv_obj_set_hidden(w->list, !content);
    if (w->agent_status) lv_obj_set_hidden(w->agent_status, !content);
    bool was_bottom = lv_obj_get_scroll_bottom(w->list) <= 2;
    bool changed = agents_set(w->dot_question, w->dot_question_text, sizeof(w->dot_question_text), question);
    changed |= agents_set(w->dot_answer, w->dot_answer_text, sizeof(w->dot_answer_text), answer);
    agents_wait(w, w->dot_answer, content ? answer : "", 2);
    bool hidden = !question[0];
    if (lv_obj_is_hidden(w->dot_bubble) != hidden) {
        lv_obj_set_hidden(w->dot_bubble, hidden); changed = true;
    }
    const orb_agent_stream_t *stream = agents_stream(w, "dot");
    const char *status = !agents_ready(w) ? orb_agents_unavailable(w->model->online, false) :
        !listening ? checking ? "Loading…" : "Dot isn’t connected" :
        stream && stream->stale ? "Last known" : "";
    if (w->agent_status)
        changed |= agents_set(w->agent_status, w->agent_status_text, sizeof(w->agent_status_text), status);
    agents_wait(w, w->agent_status, content ? status : "", 1);
    int height = 176;
    if (lv_obj_get_height(w->list) != height) { lv_obj_set_height(w->list, height); changed = true; }
    if (w->agent_talk) {
        lv_obj_set_hidden(w->agent_talk, !content && (!listening || !agents_ready(w)));
        orb_kit_control_set_enabled(w->agent_talk, agents_ready(w) && agents_voice_supported(w, "dot", ""));
    }
    /* The talk caption pairs with an enabled mic only (review #12). */
    if (w->agent_name) lv_obj_set_hidden(w->agent_name, !content || !listening || !agents_ready(w));
    if (!changed) return;
    lv_obj_update_layout(w->dot_question);
    int bubble_height = lv_obj_get_height(w->dot_question) + 20;
    lv_obj_set_height(w->dot_bubble, bubble_height);
    lv_obj_set_y(w->dot_answer, hidden ? 0 : bubble_height + 14);
    lv_obj_update_layout(w->list);
    if (was_bottom) lv_obj_scroll_to_y(w->list, LV_COORD_MAX, LV_ANIM_OFF);
}

void agents_dot(agents_state_t *w)
{
    agents_chrome_clear(w);
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    w->heading = orb_kit_rim_title_create(w->page, "Dot", ORB_KIT_DOT);
    w->dot_question_text[0] = w->dot_answer_text[0] = w->agent_status_text[0] = '\0';
    lv_obj_t *area = agents_reading(w, 73, 92, 320, 176);
    w->dot_bubble = agents_plain(area);
    lv_obj_set_size(w->dot_bubble, ax ? 250 : 270, 60); lv_obj_set_pos(w->dot_bubble, ax ? 70 : 40, 0);
    lv_obj_set_style_bg_color(w->dot_bubble, ORB_GRAY6, 0);
    lv_obj_set_style_bg_opa(w->dot_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(w->dot_bubble, 20, 0);
    w->dot_question = lv_label_create(w->dot_bubble);
    orb_text_apply(w->dot_question, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(w->dot_question, ORB_KIT_SECONDARY, 0);
    lv_obj_set_style_text_opa(w->dot_question, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_set_width(w->dot_question, ax ? 218 : 238); lv_obj_set_pos(w->dot_question, 16, 10);
    lv_label_set_long_mode(w->dot_question, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text_static(w->dot_question, w->dot_question_text);
    w->dot_answer = lv_label_create(area);
    orb_text_apply(w->dot_answer, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, 0);
    lv_obj_set_style_text_color(w->dot_answer, ORB_LABEL, 0);
    lv_obj_set_style_text_align(w->dot_answer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(w->dot_answer, 320); lv_obj_set_pos(w->dot_answer, 0, 0);
    lv_label_set_long_mode(w->dot_answer, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text_static(w->dot_answer, w->dot_answer_text);
    w->agent_status = agents_secondary_text(w, "", 276, 330, 1);
    lv_label_set_text_static(w->agent_status, w->agent_status_text);
    w->agent_talk = agents_action(w, ORB_ICON_MIC, ACT_REPLY, 233, AGENTS_TALK_Y, false, true, ORB_KIT_DOT);
    w->agent_name = orb_kit_rim_caption_create(w->page, "Hold BOOT to talk");
    orb_kit_scroll_arc_attach(area);
    agents_dot_update(w);
    agents_reading_restore(w, area, ORB_SHELL_DOT, "dot");
}

