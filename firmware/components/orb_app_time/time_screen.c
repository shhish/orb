/* Time on the screen framework. The existing orb_time_app presenter keeps its
 * pages, editors, physical-input guards and service snapshot; this adapter
 * owns its lifetime and a versioned value context. Service deadlines run in
 * orb_time independently of this screen's lifetime. */
#include "orb_app_time.h"
#include "time_command.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    orb_time_view_t *view;
    orb_app_time_model_t model;
    uint64_t poll_revision;
    /* The page last recorded for cache invalidation: the restored page (or
     * Timer), as the workspace's remembered page was before migration. */
    orb_time_page_t observed_page;
    /* The staging projection follows creation's own service snapshot; the
     * first poll stays on the shell's next refresh. */
    bool baselined, restored;
} time_state_t;

/* Pointer-free record of orb_time_view_context_t with explicit widths. */
typedef struct {
    uint32_t page;
    int32_t scroll[4], sheet_scroll[4], detail_scroll;
    uint32_t fields[4], sheet, field, reminder_id;
    bool reminder_detail;
} time_context_t;

#define TIME_CONTEXT_BYTES (sizeof(orb_screen_context_t) + sizeof(time_context_t))
#define U32(member) {offsetof(time_context_t, member), sizeof(uint32_t), ORB_CONTEXT_U32}
#define I32(member) {offsetof(time_context_t, member), sizeof(int32_t), ORB_CONTEXT_I32}
#define FOUR(macro, member) macro(member[0]), macro(member[1]), macro(member[2]), macro(member[3])

_Static_assert(sizeof(((orb_time_view_context_t *)0)->scroll) == 4 * sizeof(int), "four Time pages");

static const orb_context_field_t s_context_fields[] = {
    U32(page), FOUR(I32, scroll), FOUR(I32, sheet_scroll), I32(detail_scroll),
    FOUR(U32, fields), U32(sheet), U32(field), U32(reminder_id),
    {offsetof(time_context_t, reminder_detail), sizeof(bool), ORB_CONTEXT_BOOL},
};

static time_state_t *state_of(const orb_screen_instance_t *instance)
{
    return orb_screen_alive(instance) ? orb_screen_state(instance) : NULL;
}

static orb_time_view_t *view_of(const orb_screen_instance_t *instance)
{
    time_state_t *state = state_of(instance);
    return state ? state->view : NULL;
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    time_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    /* A style-free wrapper adds neither ink nor a semantic control. Time's
     * own root keeps every contact; its events still reach the shell. */
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (!host->config.project || !host->config.project(host->config.context, ORB_APP_TIME_SCREEN_ID, &projection) ||
        !projection.model || !projection.model->data || projection.model->size != sizeof(state->model)) return false;
    memcpy(&state->model, projection.model->data, sizeof(state->model));
    state->view = orb_time_view_create(root, &(orb_time_commands_t){time_command_send, instance});
    if (!state->view) return false;
    orb_time_view_set_system_input(state->view, state->model.system_input);
    orb_time_view_set_contrast(state->view, state->model.contrast);
    return true;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    time_state_t *state = orb_screen_state(instance);
    if (!state->view) return;
    if (update->model && update->model->data && update->model->size == sizeof(state->model))
        memcpy(&state->model, update->model->data, sizeof(state->model));
    /* The host's time revision is the shell's existing Time poll clock. */
    if (state->baselined && update->revisions.time != state->poll_revision) orb_time_view_poll(state->view);
    state->poll_revision = update->revisions.time;
    state->baselined = true;
}

static void destroy(orb_screen_instance_t *instance)
{
    /* The view is a descendant of the framework root; deleting that root
     * runs Time's own deletion path. No service call happens here. */
    time_state_t *state = orb_screen_state(instance);
    state->view = NULL;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    const time_state_t *state = orb_screen_state(instance);
    if (!state->view || capacity < TIME_CONTEXT_BYTES) return false;
    orb_time_view_context_t view;
    orb_time_view_save(state->view, &view);
    time_context_t value;
    memset(&value, 0, sizeof(value));
    value.page = view.page;
    for (unsigned i = 0; i < 4; i++) {
        value.scroll[i] = view.scroll[i];
        value.sheet_scroll[i] = view.sheet_scroll[i];
        value.fields[i] = view.fields[i];
    }
    value.detail_scroll = view.detail_scroll;
    value.sheet = view.sheet;
    value.field = view.field;
    value.reminder_id = view.reminder_id;
    value.reminder_detail = view.reminder_detail;
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    time_state_t *state = orb_screen_state(instance);
    if (!state->view) return false;
    time_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    orb_time_view_context_t view = {.page = (orb_time_page_t)value.page, .detail_scroll = value.detail_scroll,
        .sheet = value.sheet, .field = value.field, .reminder_id = value.reminder_id,
        .reminder_detail = value.reminder_detail};
    for (unsigned i = 0; i < 4; i++) {
        view.scroll[i] = value.scroll[i];
        view.sheet_scroll[i] = value.sheet_scroll[i];
        view.fields[i] = value.fields[i];
    }
    /* Remembered page first, then the editor/detail state. The presenter
     * refuses a context whose sheet it cannot present, and any restore while
     * an alarm rings or a reminder is due: navigation never Stops, Snoozes
     * or Saves. */
    orb_time_view_select(state->view, view.page);
    orb_time_view_restore(state->view, &view);
    state->restored = true;
    state->observed_page = view.page;
    return true;
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    time_state_t *state = orb_screen_state(instance);
    return orb_time_view_back(state->view) ? ORB_BACK_CONSUMED : ORB_BACK_LEAVE_SCREEN;
}

static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)origin; (void)physical;
}

static void rim_preview(orb_screen_instance_t *instance, float value)
{
    orb_time_view_rotate(view_of(instance), value);
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)value; (void)commit; (void)origin; (void)physical;
    orb_time_view_rotary_finish(view_of(instance));
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    orb_time_rotary_t owner;
    if (!orb_time_view_rotary(view_of(instance), &owner)) return false;
    /* Editors and lists change values or scroll only; Start/Save/Send stay
     * explicit physical activations. */
    *binding = (orb_rim_binding_t){.subpage = orb_time_view_page(view_of(instance)) + 1, .property = 1,
        .kind = owner.detent == 72 ? ORB_RIM_SCROLL : ORB_RIM_VALUE,
        .minimum = owner.minimum, .maximum = owner.maximum, .value = owner.value,
        .detent = owner.detent, .units_per_turn = owner.units_per_turn,
        .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    /* Deadlines are service state; the presenter's settle timer pauses on its
     * own when its root is hidden. */
    (void)instance; (void)value;
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Time"};
}

static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_APP_TIME_SCREEN_ID, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops,
    .state_bytes = sizeof(time_state_t), .context_bytes = TIME_CONTEXT_BYTES,
    .context_version = 1, .context_fields = s_context_fields,
    .context_field_count = sizeof(s_context_fields) / sizeof(s_context_fields[0]),
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(time_state_t),
        .context_bytes = TIME_CONTEXT_BYTES, .declared = true},
    .subscriptions = ORB_DOMAIN_TIME, .privacy = ORB_PRIVACY_PRIVATE,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_time_descriptor(void)
{
    return &s_descriptor;
}

void orb_app_time_enter(orb_screen_instance_t *instance, bool alarm_ringing, bool reminder_due)
{
    time_state_t *state = state_of(instance);
    if (!state || !state->view) return;
    if (!state->restored) orb_time_view_select(state->view, ORB_TIME_PAGE_TIMER);
    if (alarm_ringing) orb_time_view_select(state->view, ORB_TIME_PAGE_ALARM);
    if (reminder_due && !alarm_ringing) orb_time_view_select(state->view, ORB_TIME_PAGE_REMINDER);
}

orb_time_page_t orb_app_time_page(const orb_screen_instance_t *instance)
{
    return orb_time_view_page(view_of(instance));
}

bool orb_app_time_page_changed(orb_screen_instance_t *instance)
{
    time_state_t *state = state_of(instance);
    if (!state || !state->view) return false;
    orb_time_page_t page = orb_time_view_page(state->view);
    bool changed = page != state->observed_page;
    state->observed_page = page;
    return changed;
}

bool orb_app_time_local_back(orb_screen_instance_t *instance)
{
    orb_time_view_t *view = view_of(instance);
    if (!view) return false;
    orb_time_view_context_t context = {0};
    orb_time_view_save(view, &context);
    return context.sheet || context.reminder_detail;
}

lv_obj_t *orb_app_time_scroll(orb_screen_instance_t *instance)
{
    return orb_time_view_scroll(view_of(instance));
}

bool orb_app_time_rotary(orb_screen_instance_t *instance, orb_time_rotary_t *owner)
{
    return orb_time_view_rotary(view_of(instance), owner);
}

void orb_app_time_rotate(orb_screen_instance_t *instance, float value)
{
    orb_time_view_rotate(view_of(instance), value);
}

void orb_app_time_rotary_finish(orb_screen_instance_t *instance, bool commit)
{
    (void)commit;
    orb_time_view_rotary_finish(view_of(instance));
}
