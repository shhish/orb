/* Agents presentation lifetime. Models are borrowed only during callbacks;
 * the shell's refcounted generation remains the authoritative projection. */
#include "agents_internal.h"

#include <stddef.h>

typedef struct {
    uint32_t identity, page;
    int32_t scroll;
    agents_wait_t waits[4];
    uint32_t result_kind;
} agents_context_t;

static agents_state_t *state_of(const orb_screen_instance_t *instance)
{
    return orb_screen_alive(instance) ? orb_screen_state(instance) : NULL;
}

static bool direct_input_allowed(agents_state_t *state)
{
    orb_screen_host_t *host = orb_screen_host(state->instance);
    /* The workspace suppresses tap callbacks while the crown owns contact.
     * Its direct rotary path still updates the already admitted live page. */
    return host && !host->config.preview && !state->fixture &&
        orb_screen_visibility_get(state->instance) == ORB_VISIBILITY_ACTIVE;
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    agents_state_t *state = orb_screen_state(instance);
    state->instance = instance;
    state->surface = orb_screen_descriptor(instance)->id;
    state->page = orb_screen_root(instance);
    lv_obj_set_clickable(state->page, false);
    lv_obj_set_event_bubble(state->page, true);
    if (!agents_projection_current(state)) return false;
    /* Usage's initial value is route state, not a live telemetry reset. */
    state->page_index = state->projected_page;
    bool ok = true;
    if (state->surface == ORB_APP_AGENTS_SESSION || state->surface == ORB_APP_AGENTS_DOT) {
        state->stream_log = orb_screen_alloc(instance, sizeof(*state->stream_log));
        ok = state->stream_log != NULL;
    }
    if (ok) switch (state->surface) {
    case ORB_APP_AGENTS_SCREEN: agents(state); break;
    case ORB_APP_AGENTS_SESSIONS: agents_sessions(state); break;
    case ORB_APP_AGENTS_IDLE: agents_idle(state); break;
    case ORB_APP_AGENTS_SESSION: agents_session(state); break;
    case ORB_APP_AGENTS_USAGE: agents_usage(state); break;
    case ORB_APP_AGENTS_DOT: agents_dot(state); break;
    case ORB_APP_AGENTS_CARD: ok = agents_approval(state, agents_selected_card(state)); break;
    default: ok = false; break;
    }
    orb_screen_page_bind(instance, state->list, state->heading);
    agents_projection_end(state);
    return ok;
}

bool agents_refresh(agents_state_t *w)
{
    if ((w->surface == ORB_SHELL_AGENTS || w->surface == ORB_SHELL_SESSIONS) &&
        w->agent_membership != agents_membership(w)) return false;
    if (w->surface == ORB_SHELL_SESSIONS) {
        if (w->idle && (agents_idle_hash(w) != w->agent_idle_hash)) return false;
        for (unsigned i = 0; i < w->model->session_count; i++) {
            const orb_session_t *s = &w->model->sessions[i];
            lv_obj_t *r = w->session_list_rows[i];
            if (!r) continue;
            char detail[128];
            const char *value = !agents_ready(w) ? "Last known" : s->reason[0] ? s->reason : agents_state_text(s->state);
            if (s->agent == ORB_AGENT_DOT) snprintf(detail, sizeof(detail), "%s", value);
            else snprintf(detail, sizeof(detail), "%s · %s", agents_provider(s->agent), value);
            if (strcmp(w->session_list_names[i], s->title) || strcmp(w->session_list_details[i], detail)) {
                snprintf(w->session_list_names[i], sizeof(w->session_list_names[i]), "%s", s->title);
                snprintf(w->session_list_details[i], sizeof(w->session_list_details[i]), "%s", detail);
                orb_kit_row_set_text(r, ORB_KIT_ROW_TEXT_TITLE, s->title);
                orb_kit_row_set_text(r, ORB_KIT_ROW_TEXT_SUBTITLE, detail);
                orb_kit_row_set_text_role(r, ORB_KIT_ROW_TEXT_TITLE, ORB_TEXT_REGULAR);
            }
        }
        orb_round_list_update(w->list);
        return true;
    }
    if (w->surface == ORB_SHELL_DOT && w->dot_answer) { agents_dot_update(w); return true; }
    if (w->surface == ORB_SHELL_AGENTS && w->agent_name) { agents_focus_update(w); return true; }
    if (w->surface == ORB_SHELL_SESSION && w->stream_rows[0]) { agents_stream_update(w); return true; }
    if (w->surface == ORB_SHELL_USAGE && w->usage_rings[0]) { agents_usage_update(w); return true; }
    return false;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    agents_state_t *state = orb_screen_state(instance);
    state->refreshable = agents_projection_begin(state, update) && agents_refresh(state);
    agents_projection_end(state);
}

static void destroy(orb_screen_instance_t *instance)
{
    /* Framework root and tracked stream payload share its once-only cleanup. */
    agents_projection_end(orb_screen_state(instance));
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(agents_context_t)) return false;
    const agents_state_t *state = orb_screen_state(instance);
    agents_context_t value = {.identity = state->reading_identity,
        .page = state->page_index, .scroll = state->reading_scroll, .result_kind = state->result_kind};
    memcpy(value.waits, state->waits, sizeof(value.waits));
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    agents_state_t *state = orb_screen_state(instance);
    agents_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.page > 1 || value.scroll < 0 || value.result_kind > ORB_RESULT_UNKNOWN + 1) return false;
    state->result_changed = state->outcome_live && state->result_symbol &&
        value.identity == state->reading_identity && value.result_kind && value.result_kind != state->result_kind;
    for (unsigned i = 0; i < 4; i++) {
        if (value.waits[i].kind > 4) return false;
        /* A rebuild carries the same wait budget; a newly named wait starts
         * its own budget instead of inheriting an unrelated old one. */
        if (value.waits[i].kind == state->waits[i].kind)
            state->waits[i].started = value.waits[i].started;
    }
    if (state->wait_row) orb_kit_row_set_waiting_since(state->wait_row, state->wait_row_role,
        state->waits[0].kind != 0, state->waits[0].started);
    if (state->agent_empty && state->surface != ORB_APP_AGENTS_SESSION)
        orb_kit_state_set_waiting_since(state->agent_empty,
            state->waits[0].kind != 0, state->waits[0].started);
    if (state->agent_status) orb_kit_breathe_since(state->agent_status,
        state->waits[1].kind != 0, state->waits[1].started);
    if (state->surface == ORB_APP_AGENTS_SESSION && state->agent_empty)
        orb_kit_breathe_since(state->agent_empty, state->waits[3].kind != 0, state->waits[3].started);
    if (state->surface == ORB_APP_AGENTS_SCREEN && state->agent_steps[0])
        orb_kit_breathe_since(state->agent_steps[0], state->waits[3].kind != 0, state->waits[3].started);
    if (state->wait_result) orb_kit_breathe_since(state->wait_result,
        state->waits[3].kind != 0, state->waits[3].started);
    if (state->dot_answer) orb_kit_breathe_since(state->dot_answer,
        state->waits[2].kind != 0, state->waits[2].started);
    if (state->surface == ORB_APP_AGENTS_USAGE) {
        if (!agents_projection_current(state)) return false;
        state->page_index = value.page;
        agents_usage_page(state);
        agents_projection_end(state);
    }
    if (state->list) {
        state->reading_scroll = value.identity == state->reading_identity ? value.scroll : 0;
        state->restore_pending = true;
    }
    return true;
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    (void)instance;
    return ORB_BACK_LEAVE_SCREEN;
}

static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)origin; (void)physical;
}

static void rim_preview(orb_screen_instance_t *instance, float value)
{
    agents_state_t *state = orb_screen_state(instance);
    if (state->surface == ORB_APP_AGENTS_SCREEN)
        orb_app_agents_focus_change(instance, (int)lrintf(value) - (int)state->focus_index);
    else if (state->surface == ORB_APP_AGENTS_USAGE)
        orb_app_agents_usage_change(instance, (unsigned)lrintf(value));
    else if (state->list) {
        int top = (int)lrintf(value);
        if (!orb_round_list_scroll_to(state->list, top)) lv_obj_scroll_to_y(state->list, top, LV_ANIM_OFF);
    }
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)value; (void)commit; (void)origin; (void)physical;
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    agents_state_t *state = orb_screen_state(instance);
    if (!agents_projection_current(state)) return false;
    bool focus = state->surface == ORB_APP_AGENTS_SCREEN;
    lv_obj_t *list = focus ? NULL : state->list;
    lv_obj_update_layout(state->page);
    float value = list ? lv_obj_get_scroll_y(list) : focus ? state->focus_index : state->page_index;
    float maximum = list ? LV_MAX(0, value + lv_obj_get_scroll_bottom(list)) :
        focus ? LV_MAX(0, (int)state->model->session_count - 1) : state->surface == ORB_APP_AGENTS_USAGE ? 1 : 0;
    *binding = (orb_rim_binding_t){.subpage = state->surface, .property = 1,
        .kind = list ? ORB_RIM_SCROLL : ORB_RIM_FOCUS, .minimum = 0, .maximum = maximum, .value = value,
        .detent = list ? 72 : 1, .units_per_turn = list ? 900 : 8,
        .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    agents_projection_end(state);
    return maximum > 0;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    agents_state_t *state = orb_screen_state(instance);
    if (value != ORB_VISIBILITY_ACTIVE || !state->result_changed) return;
    state->result_changed = false;
    /* A new authoritative result in a retained receipt is a live outcome,
     * independent of navigation. Back/cover restores never replay it. */
    orb_kit_arrival_complete(state->result_symbol, false);
    orb_kit_pop_in(state->result_symbol);
    if (state->result_kind == ORB_RESULT_DONE + 1) orb_kit_check_label_draw_on(state->result_symbol);
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    const agents_state_t *state = orb_screen_state(instance);
    *value = (orb_screen_chrome_t){.title = state->heading ? orb_kit_rim_text_get(state->heading) : ""};
}

static const orb_context_field_t s_fields[] = {
    {offsetof(agents_context_t, identity), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(agents_context_t, page), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(agents_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(agents_context_t, result_kind), sizeof(uint32_t), ORB_CONTEXT_U32},
#define WAIT_FIELD(i, field) {offsetof(agents_context_t, waits[i].field), sizeof(uint32_t), ORB_CONTEXT_U32}
    WAIT_FIELD(0, kind), WAIT_FIELD(0, started), WAIT_FIELD(1, kind), WAIT_FIELD(1, started),
    WAIT_FIELD(2, kind), WAIT_FIELD(2, started), WAIT_FIELD(3, kind), WAIT_FIELD(3, started),
#undef WAIT_FIELD
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
#define DESCRIPTOR(route) { \
    .id = route, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops, \
    .state_bytes = sizeof(agents_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(agents_context_t), \
    .context_version = 3, .context_fields = s_fields, .context_field_count = 12, \
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(agents_state_t), \
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(agents_context_t), \
        .payload_bytes = sizeof(orb_stream_log_t), .declared = true}, \
    .subscriptions = ORB_DOMAIN_MODEL | ORB_DOMAIN_DEVICE, .privacy = ORB_PRIVACY_PRIVATE, \
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN, \
}
static const orb_screen_descriptor_t s_descriptors[] = {
    DESCRIPTOR(ORB_APP_AGENTS_SCREEN), DESCRIPTOR(ORB_APP_AGENTS_SESSION),
    DESCRIPTOR(ORB_APP_AGENTS_CARD), DESCRIPTOR(ORB_APP_AGENTS_DOT),
    DESCRIPTOR(ORB_APP_AGENTS_IDLE), DESCRIPTOR(ORB_APP_AGENTS_USAGE), DESCRIPTOR(ORB_APP_AGENTS_SESSIONS),
};

const orb_screen_descriptor_t *orb_app_agents_descriptor(orb_screen_id_t id)
{
    for (unsigned i = 0; i < sizeof(s_descriptors) / sizeof(s_descriptors[0]); i++)
        if (s_descriptors[i].id == id) return &s_descriptors[i];
    return NULL;
}

bool orb_app_agents_screen(orb_screen_id_t id) { return orb_app_agents_descriptor(id) != NULL; }

bool orb_app_agents_refreshable(const orb_screen_instance_t *instance)
{
    agents_state_t *state = state_of(instance);
    return state && state->refreshable;
}

const char *orb_app_agents_session(const orb_screen_instance_t *instance)
{
    agents_state_t *state = state_of(instance);
    return state ? state->session : "";
}

unsigned orb_app_agents_focus_index(const orb_screen_instance_t *instance)
{
    agents_state_t *state = state_of(instance);
    return state ? state->focus_index : 0;
}

unsigned orb_app_agents_usage_page(const orb_screen_instance_t *instance)
{
    agents_state_t *state = state_of(instance);
    return state ? state->page_index : 0;
}

bool orb_app_agents_curved(const orb_screen_instance_t *instance)
{
    agents_state_t *state = state_of(instance);
    return state && state->curved;
}

bool orb_app_agents_restore_scroll(orb_screen_instance_t *instance, int *scroll)
{
    agents_state_t *state = state_of(instance);
    if (!state || !state->restore_pending || !scroll) return false;
    *scroll = state->reading_scroll;
    state->restore_pending = false;
    return true;
}

bool orb_app_agents_session_token(const orb_screen_instance_t *instance,
    unsigned index, orb_launch_token_t *token)
{
    agents_state_t *state = state_of(instance);
    if (!state || !token || index >= ORB_MODEL_MAX_SESSIONS || !state->session_list_rows[index]) return false;
    agents_token(state->session_list_rows[index], true, token);
    token->identity = ACT_SESSION + index;
    return true;
}

bool orb_app_agents_focus_change(orb_screen_instance_t *instance, int delta)
{
    agents_state_t *state = state_of(instance);
    if (!state || !agents_projection_current(state)) return false;
    bool applied = direct_input_allowed(state) && agents_focus_change(state, delta);
    agents_projection_end(state);
    return applied;
}

void orb_app_agents_select_focus(orb_screen_instance_t *instance, unsigned index)
{
    agents_state_t *state = state_of(instance);
    if (!state || !agents_projection_current(state)) return;
    if (direct_input_allowed(state) && index < state->model->session_count) {
        snprintf(state->session, sizeof(state->session), "%s", state->model->sessions[index].id);
        state->focus_index = index;
        state->has_selection = false;
        agents_refresh(state);
    }
    agents_projection_end(state);
}

bool orb_app_agents_usage_change(orb_screen_instance_t *instance, unsigned page)
{
    agents_state_t *state = state_of(instance);
    if (!state || state->surface != ORB_APP_AGENTS_USAGE || !agents_projection_current(state)) return false;
    page = LV_MIN(page, 1u);
    bool changed = direct_input_allowed(state) && state->page_index != page;
    if (changed) { state->page_index = page; agents_usage_page(state); }
    agents_projection_end(state);
    return changed;
}
