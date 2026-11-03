/* Hosting only: the existing miniature presenter keeps its artwork, input
 * timing and cues. Context contains selection, never a launch request. */
#include "orb_app_play.h"
#include "orb_ui.h"
#include "orb_input.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    orb_game_play_view_t *view;
    orb_app_play_model_t model;
    unsigned observed;
} play_state_t;

static play_state_t *state_of(const orb_screen_instance_t *instance)
{
    return orb_screen_alive(instance) ? orb_screen_state(instance) : NULL;
}

static bool input_allowed(void *context)
{
    orb_screen_instance_t *instance = context;
    play_state_t *state = state_of(instance);
    orb_screen_host_t *host = orb_screen_host(instance);
    return state && host && !host->config.preview &&
        orb_screen_visibility_get(instance) == ORB_VISIBILITY_ACTIVE &&
        (!state->model.input_allowed || state->model.input_allowed(state->model.context));
}

static void launch(orb_game_kind_t kind, void *context)
{
    orb_screen_instance_t *instance = context;
    if (!input_allowed(instance)) return;
    /* Launch used to record selection before scheduling the shell redraw, so
     * a peek immediately followed by Play does not invalidate that cache. */
    play_state_t *state = orb_screen_state(instance);
    state->observed = kind;
    /* The legacy picker accepts synthetic safe launch activation. Its host
     * explicitly retains that policy; previews remain denied by the framework. */
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = !orb_input_is_synthetic(lv_indev_active()),
        .value.command = {.target = ORB_APP_PLAY_SCREEN_ID, .action = ORB_APP_PLAY_LAUNCH, .value = kind}};
    (void)orb_screen_emit(instance, &intent);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    play_state_t *state = orb_screen_state(instance);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (!host->config.project || !host->config.project(host->config.context, ORB_APP_PLAY_SCREEN_ID, &projection) ||
        !projection.model || !projection.model->data || projection.model->size != sizeof(state->model)) return false;
    memcpy(&state->model, projection.model->data, sizeof(state->model));
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    const char *lines[ORB_GAME_COUNT];
    for (unsigned i = 0; i < ORB_GAME_COUNT; i++) lines[i] = state->model.progress[i];
    state->view = orb_game_play_view_create(root, launch, instance, lines);
    if (!state->view) return false;
    orb_game_play_view_set_system_input(state->view, state->model.system_input);
    orb_game_play_view_set_input_guard(state->view, input_allowed);
    orb_game_play_view_select(state->view, ORB_GAME_MARBLE);
    return true;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    /* Progress is refreshed on the shell's existing full-render triggers. A
     * subscription update must not reset a locally selected miniature. */
    (void)instance; (void)update;
}

static void destroy(orb_screen_instance_t *instance)
{
    /* The presenter's descendant DELETE callback releases its own resources. */
    play_state_t *state = orb_screen_state(instance);
    state->view = NULL;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(uint32_t)) return false;
    uint32_t selected = orb_app_play_selected(instance);
    memcpy(context->payload, &selected, sizeof(selected));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    uint32_t selected;
    memcpy(&selected, context->payload, sizeof(selected));
    play_state_t *state = orb_screen_state(instance);
    if (selected >= ORB_GAME_COUNT || !state->view) return false;
    orb_game_play_view_select(state->view, selected);
    state->observed = selected;
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
    orb_app_play_select(instance, (int)value - (int)orb_app_play_selected(instance));
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)value; (void)commit; (void)origin; (void)physical;
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    /* The compatibility shell retains its wrapping/re-anchor trace policy. */
    if (!orb_app_play_picker(instance)) return false;
    *binding = (orb_rim_binding_t){.subpage = 1, .property = 1, .kind = ORB_RIM_FOCUS,
        .minimum = 0, .maximum = ORB_GAME_COUNT - 1, .value = orb_app_play_selected(instance),
        .detent = 1, .units_per_turn = 8, .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    (void)instance; (void)value;
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Play"};
}

static const orb_context_field_t s_context_fields[] = {{0, sizeof(uint32_t), ORB_CONTEXT_U32}};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_APP_PLAY_SCREEN_ID, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops,
    .state_bytes = sizeof(play_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(uint32_t),
    .context_version = 1, .context_fields = s_context_fields, .context_field_count = 1,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(play_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(uint32_t), .declared = true},
    .subscriptions = ORB_DOMAIN_TIME, .privacy = ORB_PRIVACY_PRIVATE,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_play_descriptor(void) { return &s_descriptor; }

bool orb_app_play_picker(const orb_screen_instance_t *instance)
{
    play_state_t *state = state_of(instance);
    return state && state->view;
}

orb_game_kind_t orb_app_play_selected(const orb_screen_instance_t *instance)
{
    play_state_t *state = state_of(instance);
    return state ? orb_game_play_view_selected(state->view) : ORB_GAME_MARBLE;
}

void orb_app_play_select(orb_screen_instance_t *instance, int delta)
{
    play_state_t *state = state_of(instance);
    if (!state || !state->view) return;
    int selected = orb_game_play_view_selected(state->view);
    orb_game_play_view_select(state->view, (selected + ORB_GAME_COUNT + delta % ORB_GAME_COUNT) % ORB_GAME_COUNT);
}

bool orb_app_play_selection_changed(orb_screen_instance_t *instance)
{
    play_state_t *state = state_of(instance);
    if (!state || !state->view) return false;
    unsigned selected = orb_game_play_view_selected(state->view);
    bool changed = selected != state->observed;
    state->observed = selected;
    return changed;
}

void orb_app_play_rotary_finish(orb_screen_instance_t *instance, bool commit)
{
    (void)instance; (void)commit;
}
