/* Home on the screen framework. The existing orb_home presenter keeps its own
 * nested hierarchy, physical-input guards and store snapshot; this adapter
 * owns its lifetime, value context, presentation intents and rim binding. */
#include "orb_app_home.h"
#include "home_context.h"
#include "home_command.h"
#include "orb_home_view.h"
#include "orb_input.h"
#include "orb_kit_motion.h"

#include <math.h>
#include <string.h>

typedef struct {
    orb_home_view_t *view;
    orb_app_home_model_t model;
    uint64_t poll_revision;
    /* The staging projection follows creation's own store copy. Applying only
     * its environment keeps the first poll on the shell's next refresh. */
    bool baselined, fixture, always_on, fresh;
    /* A local return issued while the framework runs Back is a button action. */
    bool backing;
} home_state_t;

static home_state_t *state_of(const orb_screen_instance_t *instance)
{
    return orb_screen_alive(instance) ? orb_screen_state(instance) : NULL;
}

static orb_home_view_t *view_of(const orb_screen_instance_t *instance)
{
    home_state_t *state = state_of(instance);
    return state ? state->view : NULL;
}

/* Home owns its local hierarchy; the host owns iris presentation. The
 * prepare phase precedes the local tree change so the source is captured. */
static void transition(void *context, const lv_area_t *token, bool reverse, bool before)
{
    orb_screen_instance_t *instance = context;
    home_state_t *state = state_of(instance);
    if (!state || !token) return;
    orb_intent_t intent = {.kind = ORB_INTENT_NAVIGATE,
        .origin = state->backing ? ORB_INPUT_BUTTON : ORB_INPUT_TOUCH,
        .physical = !orb_input_active_is_synthetic()};
    intent.value.navigate.screen = ORB_APP_HOME_SCREEN_ID;
    intent.value.navigate.launch = (orb_launch_token_t){.valid = true, .rect = {token->x1, token->y1,
        token->x2 - token->x1 + 1, token->y2 - token->y1 + 1}};
    intent.value.navigate.reverse = reverse;
    intent.value.navigate.prepare = before;
    (void)orb_screen_emit(instance, &intent);
}

static bool projection(orb_screen_instance_t *instance, orb_app_home_model_t *model)
{
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t update = {0};
    if (!host->config.project || !host->config.project(host->config.context, ORB_APP_HOME_SCREEN_ID, &update) ||
        !update.model || !update.model->data || update.model->size != sizeof(*model)) return false;
    memcpy(model, update.model->data, sizeof(*model));
    return true;
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    home_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    /* A style-free wrapper adds neither ink nor a semantic control. Home's
     * own root keeps every contact; its events still reach the shell. */
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    if (!projection(instance, &state->model)) return false;
    state->fixture = state->model.fixture != NULL;
    state->view = state->fixture ? orb_home_view_create_fixture(root, state->model.fixture) :
        orb_home_view_create(root, &(orb_home_commands_t){home_command_send, instance});
    state->model.fixture = NULL;
    if (!state->view) return false;
    orb_home_view_system_navigation(state->view, state->model.system_navigation);
    orb_home_view_hour(state->view, state->model.hour);
    orb_home_view_set_contrast(state->view, state->model.contrast);
    if (!state->fixture) orb_home_view_set_transition(state->view, transition, instance);
    state->fresh = true;
    return true;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    home_state_t *state = orb_screen_state(instance);
    if (!state->view) return;
    if (update->environment) {
        state->always_on = update->environment->always_on;
        orb_kit_motion_set_active(orb_screen_root(instance),
            orb_screen_visibility_get(instance) == ORB_VISIBILITY_ACTIVE && !state->always_on);
    }
    if (update->model && update->model->data && update->model->size == sizeof(state->model)) {
        memcpy(&state->model, update->model->data, sizeof(state->model));
        state->model.fixture = NULL;
        orb_home_view_hour(state->view, state->model.hour);
        orb_home_view_set_contrast(state->view, state->model.contrast);
    }
    /* The view retains its own revision-checked store snapshot and failure /
     * scene-receipt timers. The host's time revision is that poll clock. */
    if (state->baselined && update->revisions.time != state->poll_revision) orb_home_view_poll(state->view);
    state->poll_revision = update->revisions.time;
    state->baselined = true;
}

static void destroy(orb_screen_instance_t *instance)
{
    /* The view is a descendant of the framework root; deleting that root
     * runs Home's own deletion path, exactly as page deletion did. */
    home_state_t *state = orb_screen_state(instance);
    state->view = NULL;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    const home_state_t *state = orb_screen_state(instance);
    if (!state->view || capacity < HOME_CONTEXT_BYTES) return false;
    orb_home_view_state_t view;
    orb_home_view_save(state->view, &view);
    home_context_t value;
    home_context_encode(&view, &value);
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    home_state_t *state = orb_screen_state(instance);
    if (!state->view) return false;
    state->fresh = false;
    home_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    orb_home_view_state_t view;
    home_context_decode(&value, &view);
    orb_kit_motion_finish(orb_home_view_root(state->view));
    orb_home_view_restore(state->view, &view);
    orb_kit_arrival_complete(orb_home_view_root(state->view), false);
    orb_kit_motion_finish(orb_home_view_root(state->view));
    return true;
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    home_state_t *state = orb_screen_state(instance);
    state->backing = true;
    bool consumed = orb_home_view_back(state->view);
    state->backing = false;
    return consumed ? ORB_BACK_CONSUMED : ORB_BACK_LEAVE_SCREEN;
}

static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)origin; (void)physical;
}

static void rim_preview(orb_screen_instance_t *instance, float value)
{
    /* Synthetic rim input cannot preview a value its lift could write (HIG §1). */
    if (!orb_screen_host(instance)->lease.physical && orb_home_view_dial_writes(view_of(instance))) return;
    orb_home_view_rotate(view_of(instance), value);
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)value; (void)origin; (void)physical;
    /* A committed lift queues the one write. Cancellation is the host's
     * contact-suspension call, so a refused or unchanged step cancels nothing
     * the presenter did not preview. */
    if (commit) orb_home_view_rotary_finish(view_of(instance), true);
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    float value, step;
    orb_home_view_t *view = view_of(instance);
    if (!orb_home_view_dial(view, &value, &step)) return false;
    /* Accessory dials preview one value until lift; native lists keep their
     * ordinary scroll owner instead of taking a framework value lease. */
    *binding = (orb_rim_binding_t){.subpage = 2, .property = 1,
        .kind = ORB_RIM_VALUE,
        .minimum = 0, .maximum = 1, .value = fminf(1, fmaxf(0, value)), .detent = step, .units_per_turn = 1,
        .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    home_state_t *state = state_of(instance);
    if (state) orb_kit_motion_set_active(orb_screen_root(instance),
        value == ORB_VISIBILITY_ACTIVE && !state->always_on);
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Home"};
}

static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_APP_HOME_SCREEN_ID, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops,
    .state_bytes = sizeof(home_state_t), .context_bytes = HOME_CONTEXT_BYTES,
    .context_version = 2, .context_fields = home_context_fields,
    .context_field_count = HOME_CONTEXT_FIELD_COUNT,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(home_state_t),
        .context_bytes = HOME_CONTEXT_BYTES, .declared = true},
    .subscriptions = ORB_DOMAIN_HOME | ORB_DOMAIN_TIME, .privacy = ORB_PRIVACY_PRIVATE,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_HOME,
};

const orb_screen_descriptor_t *orb_app_home_descriptor(void)
{
    return &s_descriptor;
}

lv_obj_t *orb_app_home_scroll(orb_screen_instance_t *instance)
{
    return orb_home_view_scroll(view_of(instance));
}

bool orb_app_home_can_back(const orb_screen_instance_t *instance)
{
    return orb_home_view_can_back(view_of(instance));
}

bool orb_app_home_dial(orb_screen_instance_t *instance, float *value, float *step)
{
    return orb_home_view_dial(view_of(instance), value, step);
}

bool orb_app_home_dial_writes(const orb_screen_instance_t *instance)
{
    return orb_home_view_dial_writes(view_of(instance));
}

void orb_app_home_rotate(orb_screen_instance_t *instance, float value)
{
    orb_home_view_rotate(view_of(instance), value);
}

void orb_app_home_rotary(orb_screen_instance_t *instance, float delta)
{
    orb_home_view_rotary(view_of(instance), delta);
}

void orb_app_home_rotary_finish(orb_screen_instance_t *instance, bool commit)
{
    orb_home_view_rotary_finish(view_of(instance), commit);
}

void orb_app_home_enter(orb_screen_instance_t *instance)
{
    orb_home_view_t *view = view_of(instance);
    if (!view) return;
    orb_home_view_state_t context;
    orb_home_view_save(view, &context);
    context.surface = ORB_HOME_ROOMS;
    context.offset = context.detail_page = 0;
    context.rooms_scroll = context.scroll = 0;
    context.sections = context.sensors = context.scene_page = false;
    context.room[0] = context.selected[0] = 0;
    /* Publish before a destination snapshot freezes this fresh entry. Room
     * scroll slots remain available when that room is opened again. */
    orb_home_view_restore(view, &context);
    if (orb_kit_motion_allowed(orb_screen_root(instance))) orb_home_view_arrival_prepare(view);
}

void orb_app_home_show_scenes(orb_screen_instance_t *instance)
{
    orb_home_view_t *view = view_of(instance);
    if (!view) return;
    orb_home_view_state_t context;
    orb_home_view_save(view, &context);
    context.scene_page = true;
    context.scroll = context.scene_scroll;
    orb_home_view_restore(view, &context);
}

bool orb_app_home_moving(const orb_screen_instance_t *instance)
{
    return orb_home_view_moving(view_of(instance));
}

void orb_app_home_arrival_snapshot(orb_screen_instance_t *instance)
{
    orb_home_view_arrival_snapshot(view_of(instance));
}

void orb_app_home_arrival_prepare(orb_screen_instance_t *instance)
{
    home_state_t *state = state_of(instance);
    if (!state || !state->fresh) return;
    state->fresh = false;
    /* Staging temporarily covers every descriptor. Arm only after commit;
     * saved contexts and suspended snapshot hosts never replay arrivals. */
    if (orb_kit_motion_allowed(orb_screen_root(instance))) orb_home_view_arrival_prepare(state->view);
}
