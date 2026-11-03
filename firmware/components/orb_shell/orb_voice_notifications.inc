/* Shared shell chrome, reading anchors, arrival and outer receipt policy. */
static void workspace_chrome_clear(orb_workspace_t *w)
{
    if (w->heading) { lv_obj_delete(w->heading); w->heading = NULL; }
}

static void agents_reading_scrolled(lv_event_t *event)
{
    orb_workspace_t *w = lv_event_get_user_data(event);
    lv_obj_t *area = lv_event_get_current_target_obj(event);
    unsigned surface = (unsigned)(uintptr_t)lv_obj_get_user_data(area);
    orb_route_context_scroll(&w->places[surface], LV_MAX(0, lv_obj_get_scroll_y(area)));
}

static void notification_restore(void *context, lv_obj_t *area, const char *identity)
{
    orb_workspace_t *w = context;
    const unsigned surface = ORB_SHELL_NOTIFICATION_DETAIL;
    unsigned key = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)identity; *p; p++) key = (key ^ *p) * 16777619u;
    /* Keep the compatibility focus slot: ordinary departure overwrites this
     * reading identity with Apps focus, so re-entry still starts fresh. */
    w->restore_scroll = orb_route_context_reading(&w->places[surface], key);
    w->restore_place = true;
    lv_obj_set_user_data(area, (void *)(uintptr_t)surface);
    lv_obj_add_event_cb(area, agents_reading_scrolled, LV_EVENT_SCROLL, w);
}

/* HIG §8: the notification sound's visual pair; a labelled capsule that opens the card. */
static lv_obj_t *agents_arrival(orb_workspace_t *w)
{
    const orb_card_t *c = w->model->card_count ? &w->model->cards[w->model->card_count - 1] : NULL;
    return !c ? NULL : arrival_action(w, c->kind == ORB_CARD_APPROVAL || c->kind == ORB_CARD_CHOICE ?
        "Needs you" : "New update", ACT_CARD + (int)w->model->card_count - 1);
}

/* Try again clears only this view's local result, never a model command:
 * a card returns to its live review, a send to the hold-to-talk screen. */
static void agents_notice_dismiss(orb_workspace_t *w);

static bool agents_result_action(orb_workspace_t *w, int act)
{
    if (act == ACT_NOTICE_DISMISS) { agents_notice_dismiss(w); return true; }
    if (act != ACT_RESULT_RETRY && act != ACT_RESULT_DONE) return false;
    bool voice = w->surface == ORB_SHELL_VOICE;
    if (voice && w->model->voice.phase == ORB_VOICE_ERROR) voice_command(w, ORB_VOICE_CANCEL);
    if (act == ACT_RESULT_DONE) { back(w); return true; }
    if (voice) {
        orb_voice_flow_retry(w->voice_flow);
    } else {
        const orb_notification_record_t *r = orb_notification_history_find(w->notifications, w->card);
        const orb_card_t *c = selected_card(w);
        if (r && c && !c->answered) {
            orb_notification_history_retry(w->notifications, r->id);
        } else {
            /* The model's first-answer claim is final even after delivery
             * fails. Retry opens the session; it never replays a grant. */
            if (c && c->session[0]) session_open_token(w, c->session, NULL);
            else navigate(w, ORB_SHELL_SESSIONS, true);
            return true;
        }
    }
    render(w);
    return true;
}

/* Dismiss hides this one notification, as Clear all does for every one;
 * a live card and its receipt are untouched (clearing is presentation). */
static void agents_notice_dismiss(orb_workspace_t *w)
{
    orb_notification_history_dismiss(w->notifications, w->card);
    back(w);
}

