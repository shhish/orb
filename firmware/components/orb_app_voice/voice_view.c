/* Voice review composition moved without changing widget order or geometry. */
#include "voice_internal.h"

static lv_obj_t *voice_action_base(voice_state_t *w, const char *text, int act,
    int x, int y, bool primary, bool glyph, lv_color_t colour)
{
    lv_obj_t *obj = glyph ? orb_kit_glyph_button_create(w->page, text, primary, colour) :
        primary ? orb_kit_button_primary_create(w->page, text, colour) :
                  orb_kit_button_secondary_create(w->page, text);
    lv_obj_update_layout(obj);
    lv_obj_set_pos(obj, x - lv_obj_get_width(obj) / 2, y - lv_obj_get_height(obj) / 2);
    lv_obj_set_user_data(obj, (void *)(intptr_t)act);
    lv_obj_set_event_bubble(obj, true);
    lv_obj_add_event_cb(obj, voice_clicked, LV_EVENT_CLICKED, w);
    return obj;
}

lv_obj_t *voice_plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_event_bubble(obj, true);
    return obj;
}

lv_obj_t *voice_label(lv_obj_t *parent, const char *text, orb_text_style_t style,
    bool bold, int x, int y, int width, int lines, lv_color_t colour)
{
    return orb_kit_page_label(parent, text, style, bold, x, y, width, lines, colour);
}

bool voice_ready(const voice_state_t *w) { return w->model->online && w->model->synced; }

lv_obj_t *voice_action(voice_state_t *w, const char *text, int action,
                                int x, int y, bool primary, bool glyph, lv_color_t colour)
{
    lv_obj_t *control = voice_action_base(w, text, action, x, y, primary, glyph, colour);
    if (w->materialise) orb_glass_materialise(control, true);
    return control;
}

lv_obj_t *voice_text(voice_state_t *w, const char *text, orb_text_style_t style,
                              bool bold, int y, int width, unsigned lines, bool secondary)
{
    lv_obj_t *obj = voice_label(w->page, text, style, bold, 233, y, width, lines,
                           secondary ? ORB_KIT_SECONDARY : ORB_LABEL);
    if (secondary) lv_obj_set_style_text_opa(obj, ORB_KIT_SECONDARY_OPA, 0);
    return obj;
}

static const orb_session_t *voice_target(voice_state_t *w, const char *target)
{
    for (size_t i = 0; i < w->model->session_count; i++)
        if (!strcmp(w->model->sessions[i].id, target)) return &w->model->sessions[i];
    if (w->has_selection && !strcmp(w->selection->id, target)) return w->selection;
    return NULL;
}

static const char *voice_recipient(voice_state_t *w, const char *target)
{
    const orb_session_t *s = voice_target(w, target);
    return s ? s->title : !strcmp(target, "time") ? "Time" :
        !target[0] || !strcmp(target, "dot") ? "Dot" : "Session ended";
}

lv_obj_t *voice_capsule(voice_state_t *w, const char *caption, int action,
                               int cx, int cy, bool primary, lv_color_t accent_colour)
{
    lv_obj_t *control = voice_action(w, caption, action, cx, cy, primary, false, accent_colour);
    /* HIG §§7/9: the kit handles contact retirement; availability still belongs
     * to the action boundary, including retained offline review transcripts. */
    if ((action >= ACT_OPTION && action < ACT_OPTION + ORB_MODEL_MAX_OPTIONS && !voice_ready(w)) ||
        ((action == ACT_VOICE_SEND || action == ACT_VOICE_HOLD) && (!voice_ready(w) || w->flow->voice_pending)) ||
        (action == ACT_REPLY && !voice_ready(w)))
        orb_kit_control_set_enabled(control, false);
    return control;
}

/* Review #27: two equal-status choices share the wider one's width. They sit
 * side by side (cancel role leading) when both fit the 6 o'clock chord, and
 * otherwise stack with the cancel role at 6 o'clock, as HIG §7 alerts put
 * Cancel last. Returns the pair's top edge for the reading viewport above. */
int voice_pair(lv_obj_t *cancel, lv_obj_t *other)
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


lv_obj_t *voice_secondary_text(voice_state_t *w, const char *text, int y, int width, int lines)
{
    return voice_text(w, text, ORB_TEXT_FOOTNOTE2, false, y, width, lines, true);
}

lv_obj_t *voice_reading(voice_state_t *w, int x, int y, int width, int height)
{
    lv_obj_t *area = orb_kit_detail_page(w->page, &(orb_kit_detail_page_config_t){.top = y, .height = height});
    w->list = area;
    lv_obj_set_pos(area, x, y); lv_obj_set_width(area, width);
    lv_obj_set_clickable(area, true);
    lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_OFF);
    return area;
}

int voice_paragraph(lv_obj_t *parent, const char *text, orb_text_style_t style,
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
void voice_result(voice_state_t *w, const orb_result_copy_t *copy, const char *decision,
                          const char *body, unsigned surface, const char *identity, bool retry)
{
    bool failed = copy->outcome == ORB_RESULT_FAILED || copy->outcome == ORB_RESULT_UNKNOWN;
    lv_obj_t *done = voice_capsule(w, "Done", ACT_RESULT_DONE, 233, 380, false, ORB_KIT_GREY);
    int top = 380 - ORB_KIT_SECONDARY_H / 2;
    if (retry && failed)
        top = voice_pair(done, voice_capsule(w, "Try again", ACT_RESULT_RETRY, 233, 380, false, ORB_KIT_GREY));
    lv_obj_t *area = voice_reading(w, 68, 112, 330, top - 12 - 112);
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
    y = voice_paragraph(area, copy->title, ORB_TEXT_TITLE3, true, y, ORB_LABEL, true) + 4;
    orb_kit_breathe_since(lv_obj_get_child(area, lv_obj_get_child_count(area) - 1),
        w->model->voice.phase == ORB_VOICE_SENDING, w->flow->wait_started);
    if (decision && decision[0])
        y = voice_paragraph(area, decision, ORB_TEXT_CAPTION1, true, y, ORB_LABEL, true) + 4;
    if (copy->detail[0]) {
        y = voice_paragraph(area, copy->detail, ORB_TEXT_CAPTION1, false, y, ORB_LABEL, true) + 6;
        orb_kit_breathe_since(lv_obj_get_child(area, lv_obj_get_child_count(area) - 1),
            copy->outcome == ORB_RESULT_SENT, w->flow->wait_started);
    }
    if (body && body[0]) voice_paragraph(area, body, ORB_TEXT_CAPTION1, false, y, ORB_KIT_SECONDARY, true);
    orb_kit_scroll_arc_attach(area);
    voice_reading_restore(w, area, surface, identity);
}


static void voice_bars(voice_state_t *w, bool hidden)
{
    for (unsigned i = 0; i < 9; i++) {
        lv_obj_t *bar = voice_plain(w->page); w->voice_bars[i] = bar;
        lv_obj_set_pos(bar, 152 + i * 19, 176); lv_obj_set_size(bar, 10, 12);
        lv_obj_set_style_radius(bar, 5, 0);
        lv_obj_set_style_bg_color(bar, w->capture_colour, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_hidden(bar, hidden);
    }
}

void voice_meter(voice_state_t *w)
{
    bool capture = w->model->voice.phase == ORB_VOICE_STARTING || w->model->voice.phase == ORB_VOICE_RECORDING;
    bool held_error = w->flow->mic_held &&
                      w->model->voice.phase == ORB_VOICE_ERROR;
    if (w->meter) lv_obj_set_hidden(w->meter, capture || held_error);
    /* A held touch keeps the workspace tree alive while phase changes arrive.
     * Both visual states are prepared while idle; this path only reveals the
     * recording controls and retires the idle instructions. */
    if (w->voice_talk) lv_obj_set_hidden(w->voice_talk, !(capture || held_error) || !w->flow->voice_touch);
    if (w->voice_name) lv_obj_set_hidden(w->voice_name, !(capture || held_error));
    if (w->meter && w->voice_status) {
        lv_obj_set_hidden(w->voice_status, !held_error);
        if (held_error) voice_set(w->voice_status, w->voice_status_text, sizeof(w->voice_status_text),
            !strcmp(w->model->voice.error_code, "no_speech") ? "Didn’t catch that" : "Not sent");
    }
    if (w->voice_quote) {
        lv_obj_set_hidden(w->voice_quote, !(capture || held_error));
        const char *transcript = held_error ? w->model->voice.error[0] ? w->model->voice.error :
            "Recording failed. Release to review." : w->model->voice.text[0] ? w->model->voice.text : "Listening…";
        if (strcmp(w->voice_source_text, transcript)) {
            snprintf(w->voice_source_text, sizeof(w->voice_source_text), "%s", transcript);
            /* The prepared two-line preview stays within its chord while a
             * held input prevents a full rebuild. The complete error appears
             * in the normal scrollable error page immediately after lift. */
            voice_fit(w->voice_quote, w->voice_preview_text, sizeof(w->voice_preview_text), transcript);
        }
    }
    unsigned level = LV_MIN(100, w->model->voice.level);
    static const unsigned s_weights[] = {18, 38, 62, 84, 54, 70, 40, 24, 14};
    for (unsigned i = 0; i < 9; i++) {
        lv_obj_t *bar = w->voice_bars[i];
        if (!bar) continue;
        lv_obj_set_hidden(bar, !capture);
        /* HIG §8: microphone activity is an actual amplitude, never a canned
         * animation; zero input stays quiet and Reduce Motion keeps it steady. */
        int height = 6 + (int)(s_weights[i] * level / 100);
        if (orb_ui_reduce_motion()) height = 6 + (int)(s_weights[i] * (level / 20) / 5);
        int y = 182 - height / 2;
        if (lv_obj_get_height(bar) != height) lv_obj_set_height(bar, height);
        if (lv_obj_get_y(bar) != y) lv_obj_set_y(bar, y);
    }
}

static void voice_phase(voice_state_t *w)
{
    const orb_voice_state_t *v = &w->model->voice;
    if (w->flow->voice_seen_phase == v->phase) return;
    bool same_delivery = w->flow->voice_seen_phase == ORB_VOICE_SENDING &&
        v->phase == ORB_VOICE_IDLE && w->model->last_message.status == ORB_SEND_QUEUED;
    w->flow->voice_seen_phase = v->phase;
    if (!same_delivery) w->flow->wait_started = lv_tick_get();
    if (v->phase == ORB_VOICE_STARTING || v->phase == ORB_VOICE_RECORDING) {
        w->flow->voice_sent_at = 0;
        w->flow->voice_retry = false; /* A new capture owns the next result. */
    }
    /* HIG §§8/9: transcripts and every receipt wait for explicit resolution.
     * Neither submission nor delivered-result dismissal runs on a countdown. */
}

void voice_render(voice_state_t *w)
{
    memset(w->voice_bars, 0, sizeof(w->voice_bars));
    voice_phase(w);
    const orb_voice_state_t *v = &w->model->voice;
    bool no_speech = v->phase == ORB_VOICE_ERROR && !strcmp(v->error_code, "no_speech");
    char target[ORB_MODEL_ID_MAX + 1] = {0}, card_id[ORB_MODEL_CARD_ID_MAX + 1] = {0};
    if (v->phase == ORB_VOICE_IDLE && w->flow->voice_sent_at && w->model->last_message.message.target[0])
        snprintf(target, sizeof(target), "%s", w->model->last_message.message.target);
    else if (v->phase == ORB_VOICE_IDLE) orb_model_get_voice_focus(target, card_id);
    else snprintf(target, sizeof(target), "%s", v->target);
    const orb_session_t *s = voice_target(w, target);
    bool local_time = !strcmp(target, "time");
    bool available = orb_app_voice_supported(&(orb_app_voice_model_t){.model = w->model, .selection = w->selection, .selection_external = w->selection_external}, target, v->phase == ORB_VOICE_IDLE ? card_id : v->card);
    lv_color_t colour = local_time ? ORB_KIT_TIME : s ? (s->agent == ORB_AGENT_CLAUDE ? ORB_KIT_CLAUDE : s->agent == ORB_AGENT_CODEX ? ORB_KIT_CODEX : ORB_KIT_DOT) : ORB_KIT_DOT;
    /* HIG §§3/4: identity curves within 120°. Long names never steal the
     * transcript's reading chord; the kit preserves the full semantic name. */
    w->heading = orb_kit_rim_title_create(w->page, voice_recipient(w, target), colour);
    bool capture = v->phase == ORB_VOICE_STARTING || v->phase == ORB_VOICE_RECORDING;
    bool receipt = v->phase == ORB_VOICE_IDLE && !w->flow->voice_retry && w->model->last_message.message.target[0] &&
                   !strcmp(w->model->last_message.message.target, target);
    if (capture) {
        voice_bars(w, false);
        voice_meter(w);
        lv_obj_t *area = voice_reading(w, 68, 224, 330, 80);
        voice_paragraph(area, v->text[0] ? v->text : v->phase == ORB_VOICE_STARTING ?
                  "Starting microphone…" : "Listening…", ORB_TEXT_CAPTION2, true, 0, ORB_LABEL, true);
        orb_kit_breathe_since(lv_obj_get_child(area, lv_obj_get_child_count(area) - 1),
            v->phase == ORB_VOICE_STARTING && !v->text[0], w->flow->wait_started);
        orb_kit_scroll_arc_attach(area);
        char identity[ORB_MODEL_ID_MAX + 12];
        snprintf(identity, sizeof(identity), "capture:%s", target);
        voice_reading_restore(w, area, ORB_SHELL_VOICE, identity);
        if (w->flow->voice_touch) {
            w->voice_talk = voice_action(w, ORB_ICON_STOP, ACT_VOICE_STOP, 233, AGENTS_TALK_Y, false, true, colour);
            w->voice_name = orb_kit_rim_caption_create(w->page, "Release to review");
        } else w->voice_name = orb_kit_rim_caption_create(w->page, "Release BOOT to review");
    } else if (v->phase == ORB_VOICE_CONFIRMING) {
        /* The decision controls remain separated by 22px at every text size.
         * HIG §§7/8: only explicit Send/Save commits; no countdown exists. */
        lv_obj_t *area = voice_reading(w, 68, 140, 330, 116);
        /* Review #13: Send needs a real transcript and a session that takes it; the reason leads. */
        const char *blocked = orb_voice_review_blocked(voice_ready(w), available, s != NULL, target, v->text[0]);
        int top = blocked ? voice_paragraph(area, blocked, ORB_TEXT_FOOTNOTE2, false, 0, ORB_LABEL, true) + 12 : 0;
        voice_paragraph(area, v->text[0] ? v->text : "No transcript", ORB_TEXT_CAPTION1,
                         true, top, ORB_LABEL, true);
        lv_obj_t *transcript = lv_obj_get_child(area, lv_obj_get_child_count(area) - 1);
        /* Shared full-text fitting follows Dynamic Type; an over-budget
         * transcript retains all words in this scrollable reading viewport. */
        orb_kit_hero_fit(transcript, v->text[0] ? v->text : "No transcript", 330, 3, NULL);
        lv_obj_update_layout(area);
        int hint_y = lv_obj_get_y(transcript) + lv_obj_get_height(transcript) + 16;
        if (!blocked) voice_paragraph(area, "Hold BOOT again to redo",
                                               ORB_TEXT_FOOTNOTE2, false, hint_y, ORB_LABEL, true);
        orb_kit_scroll_arc_attach(area);
        char identity[ORB_MODEL_ID_MAX + 12];
        snprintf(identity, sizeof(identity), "review:%s", target);
        voice_reading_restore(w, area, ORB_SHELL_VOICE, identity);
        voice_capsule(w, "Cancel", ACT_VOICE_CANCEL, 233, 300, false, colour);
        /* Only an actionable Send keeps the one tinted control (HIG §§2/8). */
        lv_obj_t *send = voice_capsule(w, local_time ? "Save" : "Send", ACT_VOICE_SEND,
                                       233, 380, !blocked && !w->flow->voice_pending, colour);
        if (blocked) orb_kit_control_set_enabled(send, false);
    } else if (!no_speech && (receipt || v->phase == ORB_VOICE_SENDING || v->phase == ORB_VOICE_ERROR)) {
        const orb_message_result_t *r = &w->model->last_message;
        bool send_result = receipt || (w->flow->voice_sent_at && r->message.stream && r->message.stream == v->stream &&
                                      !strcmp(r->message.target, target));
        bool uncertain = (send_result && !strcmp(r->error, "timeout")) ||
                         (!send_result && v->phase == ORB_VOICE_ERROR && !strcmp(v->error_code, "timeout"));
        bool error = !send_result && v->phase == ORB_VOICE_ERROR;
        /* Review #24: the shared delivery vocabulary (glossary §2.1). */
        orb_result_copy_t copy = orb_send_result(v->phase == ORB_VOICE_SENDING, local_time, uncertain, error,
            local_time && error ? ORB_SEND_FAILED : r->status,
            error ? v->error : r->reason[0] ? r->reason : NULL);
        char identity[ORB_MODEL_ID_MAX + 12];
        snprintf(identity, sizeof(identity), "result:%s", target);
        voice_result(w, &copy, NULL, v->text[0] ? v->text : r->message.text, ORB_SHELL_VOICE, identity,
                      v->phase != ORB_VOICE_SENDING);
    } else {
        w->meter = voice_plain(w->page); lv_obj_set_size(w->meter, 466, 466);
        /* Glossary §3.5: one instruction, and the disc is its touch twin, so
         * no rim caption repeats it under the disc (review #6). */
        lv_obj_t *instruction = voice_label(w->page, no_speech ? "Didn’t catch that" :
            v->phase == ORB_VOICE_TRANSCRIBING ? "Transcribing…" : "Hold BOOT to talk",
            ORB_TEXT_HEADLINE, true, 233, 140, 330, 2, ORB_LABEL);
        lv_obj_set_parent(instruction, w->meter);
        orb_kit_breathe_since(instruction, v->phase == ORB_VOICE_TRANSCRIBING, w->flow->wait_started);
        /* While transcribing there is nothing to hold; keep only Cancel (#14). */
        instruction = voice_secondary_text(w, !voice_ready(w) ? "Voice needs your Mac" : no_speech ?
            "Hold BOOT to talk" : w->notice[0] ? w->notice : "", 244, 330, 2);
        lv_obj_set_parent(instruction, w->meter);
        if (v->phase == ORB_VOICE_TRANSCRIBING)
            voice_capsule(w, "Cancel", ACT_VOICE_CANCEL, 233, 380, false, colour);
        else if (available) {
            lv_obj_t *hold = voice_action(w, ORB_ICON_MIC, ACT_VOICE_HOLD, 233, AGENTS_TALK_Y, false, true, colour);
            lv_obj_set_parent(hold, w->meter);
            lv_obj_add_event_cb(hold, voice_hold, LV_EVENT_LONG_PRESSED, w);
            lv_obj_add_event_cb(hold, voice_hold, LV_EVENT_RELEASED, w);
            lv_obj_add_event_cb(hold, voice_hold, LV_EVENT_PRESS_LOST, w);
            if (!voice_ready(w) || w->flow->voice_pending) orb_kit_control_set_enabled(hold, false);
            w->voice_talk = voice_action(w, ORB_ICON_STOP, ACT_VOICE_STOP, 233, AGENTS_TALK_Y, false, true, colour);
            lv_obj_set_hidden(w->voice_talk, true);
            w->voice_name = orb_kit_rim_caption_create(w->page, "Release to review");
            lv_obj_set_hidden(w->voice_name, true);
        }
        w->voice_status = voice_label(w->page, "Not sent", ORB_TEXT_HEADLINE, true, 233, 140, 330, 2, ORB_LABEL);
        w->voice_status_text[0] = '\0';
        lv_obj_set_hidden(w->voice_status, true);
        voice_bars(w, true);
        w->voice_quote = voice_label(w->page, "", ORB_TEXT_CAPTION2, false, 233, 224, 320, 2, ORB_LABEL);
        lv_label_set_long_mode(w->voice_quote, LV_LABEL_LONG_MODE_WRAP);
        w->voice_preview_text[0] = w->voice_source_text[0] = '\0';
        lv_label_set_text_static(w->voice_quote, w->voice_preview_text);
        lv_obj_set_hidden(w->voice_quote, true);
    }
}





