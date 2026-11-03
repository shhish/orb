/* Compatibility boundary for the migrated Agents descriptors. The workspace
 * keeps outer navigation/voice policy; immutable generations are borrowed only
 * for the synchronous projection callback, including read-only previews. */
static bool agents_project(void *context, orb_screen_id_t screen, orb_app_agents_model_t *model)
{
    orb_workspace_t *w = context;
    *model = (orb_app_agents_model_t){.model = w->model, .idle = w->idle,
        .selection = w->has_selection ? &w->selection : NULL, .selection_external = w->selection_external,
        .page = w->page_index, .fixture = w->fixture,
        .materialise = w->navigation_pending && !w->nav_applying,
        .net_connected = w->status.net == ORB_SHELL_NET_CONNECTED,
        .link_online = w->status.link == ORB_SHELL_LINK_ONLINE,
        .interactive = !w->fixture && !w->suppress_click && !w->opening,
        .outcome_live = !w->opening && !w->open.active && !w->nav_applying && !w->nav_pending && !w->nav_presenting};
    snprintf(model->session, sizeof(model->session), "%s", w->session);
    snprintf(model->card, sizeof(model->card), "%s", w->card);
    model->receipt = screen == ORB_SHELL_CARD ?
        orb_notification_history_receipt(w->notifications, w->card) : NULL;
    return true;
}

static void agents_focus_sync(orb_workspace_t *w)
{
    const char *session = orb_app_agents_session(agents_screen(w));
    if (session) snprintf(w->session, sizeof(w->session), "%s", session);
}

static void agents_synthetic_refused(void *context, const orb_intent_t *intent)
{
    orb_workspace_t *w = context;
    if (intent->kind == ORB_INTENT_APPROVE && !w->fixture && !w->suppress_click && !w->opening)
        (void)orb_input_require_physical(w->root);
}

static bool agents_accept(void *context, const orb_intent_t *intent)
{
    orb_workspace_t *w = context;
    if (w->fixture || w->suppress_click || w->opening) return false;
    agents_focus_sync(w);
    if (intent->kind == ORB_INTENT_NAVIGATE) {
        clicked(w, intent->value.navigate.screen, NULL, &intent->value.navigate.launch);
        return true;
    }
    int action = (int)intent->value.command.action;
    /* Focus has already moved in the descriptor. Synchronize the existing
     * voice target once; crown movement uses the same policy below. */
    if (action == ACT_FOCUS_NEXT || action == ACT_FOCUS_PREVIOUS ||
        (action >= ACT_FOCUS_SELECT && action < ACT_FOCUS_SELECT + ORB_MODEL_MAX_SESSIONS)) {
        w->has_selection = false;
        voice_focus(w);
        return true;
    }
    clicked(w, action, NULL, &intent->launch);
    return true;
}

static orb_agents_host_t *agents_host_create(orb_workspace_t *w, bool preview)
{
    return orb_agents_host_create(preview, &(orb_agents_host_config_t){.context = w,
        .project = agents_project, .accept = agents_accept, .synthetic_refused = agents_synthetic_refused});
}

static bool agents_context_copy(orb_workspace_t *w, orb_agents_host_t **destination,
    orb_agents_host_t *source, bool preview)
{
    if (!source && !*destination) return true;
    if (!*destination) *destination = agents_host_create(w, preview);
    return *destination && orb_agents_host_clone_contexts(*destination, source);
}

/* Dot's existing automatic discovery stays at the model-owner boundary. It
 * occurs at the original construction/update point and never in a preview. */
static void agents_dot_discover(orb_workspace_t *w)
{
    if (w->fixture || !w->idle) return;
    bool discovered = false;
    for (unsigned i = 0; i < w->model->session_count; i++)
        if (w->model->sessions[i].agent == ORB_AGENT_DOT) discovered = true;
    if (discovered) return;
    orb_model_idle_snapshot(w->idle);
    for (unsigned i = 0; i < w->idle->count; i++) if (w->idle->sessions[i].agent == ORB_AGENT_DOT) {
        w->selection = w->idle->sessions[i];
        w->has_selection = w->selection_external = discovered = true;
        break;
    }
    if (!discovered && ready(w) && !w->idle->pending && !w->idle->error[0] && w->idle->cursor[0])
        orb_model_idle_more();
}

static void agents_adapter(orb_workspace_t *w)
{
    workspace_chrome_clear(w);
    if (w->surface == ORB_SHELL_DOT) agents_dot_discover(w);
    if (!w->fixture && w->idle && (w->surface == ORB_SHELL_SESSIONS || w->surface == ORB_SHELL_IDLE_SESSIONS))
        orb_model_idle_snapshot(w->idle);
    if (!w->agents_host) w->agents_host = agents_host_create(w, w->fixture);
    if (!orb_agents_host_attach(w->agents_host, w->page, w->surface)) return;
    orb_screen_instance_t *screen = agents_screen(w);
    w->list = orb_screen_page_viewport(screen);
    w->heading = orb_screen_page_title(screen);
    if (w->heading) lv_obj_set_parent(w->heading, w->page);
    w->curved = orb_app_agents_curved(screen);
    w->restore_place = orb_app_agents_restore_scroll(screen, &w->restore_scroll);
    if (w->surface == ORB_SHELL_IDLE_SESSIONS && w->list)
        lv_obj_add_event_cb(w->list, scroll_position, LV_EVENT_SCROLL, w);
    if (w->surface == ORB_SHELL_USAGE) w->page_index = orb_app_agents_usage_page(screen);
    agents_focus_sync(w);
    if (w->surface == ORB_SHELL_AGENTS || w->surface == ORB_SHELL_SESSION) voice_focus(w);
}

static bool agents_refresh(orb_workspace_t *w)
{
    if (!agents_screen(w) || orb_screen_descriptor(agents_screen(w))->id != w->surface) return false;
    if (w->surface == ORB_SHELL_DOT) agents_dot_discover(w);
    bool retained = orb_agents_host_refresh(w->agents_host);
    agents_focus_sync(w);
    return retained;
}

static void agents_focus_change(orb_workspace_t *w, int delta)
{
    if (w->surface != ORB_SHELL_AGENTS || !delta) return;
    if (!orb_app_agents_focus_change(agents_screen(w), delta)) return;
    agents_focus_sync(w);
    w->has_selection = false;
    voice_focus(w);
}

static bool agents_bench_restore(orb_workspace_t *w)
{
    if (agents_screen(w)) { w->list = NULL; w->heading = NULL; }
    if (!agents_context_copy(w, &w->agents_host, w->agents_bench, w->fixture)) return false;
    orb_agents_host_destroy(w->agents_bench);
    w->agents_bench = NULL;
    return orb_app_host_restore(&w->play_host, &w->play_bench) &&
        orb_settings_host_restore(&w->settings, &w->settings_bench) &&
        orb_app_host_restore(&w->home_host, &w->home_bench) &&
        orb_app_host_restore(&w->time_host, &w->time_bench);
}
