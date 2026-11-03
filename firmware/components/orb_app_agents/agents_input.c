/* Value-only input bridge. The compatibility host retains outer route policy;
 * every callback reacquires its immutable projection before interpreting IDs. */
#include "agents_internal.h"

bool agents_projection_begin(agents_state_t *state, const orb_screen_update_t *update)
{
    if (!update || !update->model || !update->model->data ||
        update->model->size != sizeof(orb_app_agents_model_t)) return false;
    const orb_app_agents_model_t *model = update->model->data;
    if (!model->model) return false;
    state->projected_page = model->page;
    state->model = model->model;
    state->idle = model->idle;
    state->selection = model->selection;
    state->receipt = model->receipt;
    state->has_selection = model->selection != NULL;
    state->selection_external = model->selection_external;
    state->fixture = model->fixture;
    state->materialise = model->materialise;
    state->net_connected = model->net_connected;
    state->link_online = model->link_online;
    state->interactive = model->interactive; state->outcome_live = model->outcome_live;
    snprintf(state->session, sizeof(state->session), "%s", model->session);
    snprintf(state->card, sizeof(state->card), "%s", model->card);
    return true;
}

bool agents_projection_current(agents_state_t *state)
{
    orb_screen_host_t *host = orb_screen_host(state->instance);
    orb_screen_update_t update = {0};
    return host && host->config.project &&
        host->config.project(host->config.context, state->surface, &update) &&
        agents_projection_begin(state, &update);
}

void agents_projection_end(agents_state_t *state)
{
    state->model = NULL;
    state->idle = NULL;
    state->selection = NULL;
    state->receipt = NULL;
}

bool agents_input_allowed(agents_state_t *state)
{
    orb_screen_host_t *host = orb_screen_host(state->instance);
    return host && !host->config.preview && !state->fixture && state->interactive &&
        orb_screen_visibility_get(state->instance) == ORB_VISIBILITY_ACTIVE;
}

void agents_token(lv_obj_t *row, bool session, orb_launch_token_t *token)
{
    *token = (orb_launch_token_t){.valid = true, .rect = {197, 197, 72, 72}};
    if (!row) return;
    if (session) {
        orb_kit_press_cancel(row);
        lv_obj_remove_state(row, LV_STATE_PRESSED);
    }
    lv_area_t bounds;
    lv_obj_get_coords(row, &bounds);
    if (!session) {
        token->rect = (orb_nav_token_t){bounds.x1, bounds.y1,
            lv_area_get_width(&bounds), lv_area_get_height(&bounds)};
        lv_obj_remove_state(row, LV_STATE_PRESSED);
        return;
    }
    token->rect = (orb_nav_token_t){(bounds.x1 + bounds.x2 + 1) / 2 - 36,
        (bounds.y1 + bounds.y2 + 1) / 2 - 36, 72, 72};
    if (lv_obj_get_child_count(row) &&
        orb_round_list_child_bounds(row, lv_obj_get_child(row, 0), &bounds))
        token->rect = (orb_nav_token_t){bounds.x1, bounds.y1,
            lv_area_get_width(&bounds), lv_area_get_height(&bounds)};
    for (unsigned i = 0; i < lv_obj_get_child_count(row); i++) {
        lv_obj_t *disc = lv_obj_get_child(row, i);
        if (lv_obj_get_width(disc) != ORB_KIT_DISC_ROW || lv_obj_get_height(disc) != ORB_KIT_DISC_ROW) continue;
        if (orb_round_list_child_bounds(row, disc, &bounds))
            token->rect = (orb_nav_token_t){bounds.x1, bounds.y1,
                lv_area_get_width(&bounds), lv_area_get_height(&bounds)};
        break;
    }
}

bool orb_app_agents_allow_synthetic(const orb_intent_t *intent)
{
    if (!intent) return false;
    if (intent->kind == ORB_INTENT_NAVIGATE) return true;
    if (intent->kind != ORB_INTENT_APP_COMMAND) return false;
    unsigned action = intent->value.command.action;
    return (action >= ACT_SESSION && action < ACT_SESSION + ORB_MODEL_MAX_SESSIONS) ||
        (action >= ACT_IDLE_SESSION && action < ACT_IDLE_SESSION + ORB_MODEL_MAX_IDLE) ||
        (action >= ACT_FOCUS_SELECT && action < ACT_FOCUS_SELECT + ORB_MODEL_MAX_SESSIONS) ||
        action == ACT_REPLY || action == ACT_IDLE_MORE ||
        (action >= ACT_FOCUS_NEXT && action <= ACT_RESULT_DONE);
}

static bool activate(agents_state_t *state, int action, orb_input_origin_t origin,
    bool physical, const orb_launch_token_t *launch)
{
    if (!agents_input_allowed(state) ||
        (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON)) {
        agents_projection_end(state);
        return false;
    }
    orb_intent_t intent = {.kind = action < 100 ? ORB_INTENT_NAVIGATE :
        action >= ACT_OPTION && action < ACT_OPTION + ORB_MODEL_MAX_OPTIONS ?
        ORB_INTENT_APPROVE : ORB_INTENT_APP_COMMAND, .origin = origin, .physical = physical};
    if (action < 100) {
        intent.value.navigate.screen = action;
        if (launch) intent.value.navigate.launch = *launch;
    } else {
        intent.value.command.action = action;
        if (launch) intent.launch = *launch;
    }
    if (action == ACT_FOCUS_NEXT || action == ACT_FOCUS_PREVIOUS) {
        if (!agents_focus_change(state, action == ACT_FOCUS_NEXT ? 1 : -1)) {
            agents_projection_end(state);
            return false;
        }
    } else if (action >= ACT_FOCUS_SELECT && action < ACT_FOCUS_SELECT + ORB_MODEL_MAX_SESSIONS) {
        unsigned index = action - ACT_FOCUS_SELECT;
        if (index >= state->model->session_count) {
            agents_projection_end(state);
            return false;
        }
        snprintf(state->session, sizeof(state->session), "%s", state->model->sessions[index].id);
        state->focus_index = index;
        state->has_selection = false;
        agents_refresh(state);
    }
    orb_screen_instance_t *instance = state->instance;
    /* Compatibility acceptance can complete a pending Back and destroy the
     * sender. End the borrowed callback scope before entering that boundary. */
    agents_projection_end(state);
    return orb_screen_emit(instance, &intent);
}

bool orb_app_agents_activate(orb_screen_instance_t *instance, int action,
    orb_input_origin_t origin, bool physical, const orb_launch_token_t *launch)
{
    agents_state_t *state = orb_screen_state(instance);
    if (!state || !agents_projection_current(state)) return false;
    return activate(state, action, origin, physical, launch);
}

void agents_clicked(lv_event_t *event)
{
    agents_state_t *state = lv_event_get_user_data(event);
    if (!agents_projection_current(state)) return;
    lv_obj_t *target = lv_event_get_target_obj(event);
    bool allowed = agents_input_allowed(state);
    if (allowed && state->curved && state->list && lv_obj_get_parent(target) == state->list) {
        allowed = lv_obj_get_style_opa(target, 0) != LV_OPA_TRANSP;
        lv_indev_t *input = lv_indev_active();
        if (allowed && input) {
            lv_point_t point;
            lv_indev_get_point(input, &point);
            allowed = orb_round_list_hit_test(target, &point);
        }
    }
    if (allowed) {
        int action = (int)(intptr_t)lv_obj_get_user_data(target);
        bool session = (action >= ACT_SESSION && action < ACT_SESSION + ORB_MODEL_MAX_SESSIONS) ||
            (action >= ACT_IDLE_SESSION && action < ACT_IDLE_SESSION + ORB_MODEL_MAX_IDLE);
        orb_launch_token_t token = {0};
        if (session || action < 100) agents_token(target, session, &token);
        token.identity = (uint64_t)(unsigned)action;
        (void)activate(state, action, ORB_INPUT_TOUCH, !orb_input_is_synthetic(lv_indev_active()), &token);
        return;
    }
    agents_projection_end(state);
}
