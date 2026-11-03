/* Review lifetime and copied input; shell owns the outer route and BOOT. */
#include "voice_internal.h"
#include "orb_page.h"
#include "orb_kit_motion.h"
#include <stddef.h>

typedef struct { uint32_t identity; int32_t scroll; uint32_t result_kind; } voice_context_t;

bool voice_projection_current(voice_state_t *state)
{
    orb_screen_host_t *host = orb_screen_host(state->instance);
    orb_screen_update_t update = {0};
    if (!host || !host->config.project ||
        !host->config.project(host->config.context, ORB_APP_VOICE_SCREEN, &update) ||
        !update.model || update.model->size != sizeof(orb_app_voice_model_t)) return false;
    const orb_app_voice_model_t *model = update.model->data;
    if (!model || !model->model || !model->flow) return false;
    state->model = model->model; state->selection = model->selection; state->flow = model->flow;
    state->has_selection = model->selection != NULL; state->selection_external = model->selection_external;
    state->notice = model->notice; state->fixture = model->fixture; state->materialise = model->materialise;
    state->interactive = model->interactive; state->touch_allowed = model->touch_allowed;
    state->outcome_live = model->outcome_live;
    state->capture_colour = model->capture_colour;
    return true;
}

void voice_projection_end(voice_state_t *state)
{
    state->model = NULL; state->selection = NULL; state->flow = NULL; state->notice = NULL;
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    voice_state_t *state = orb_screen_state(instance);
    state->instance = instance; state->page = orb_screen_root(instance);
    lv_obj_set_clickable(state->page, false); lv_obj_set_event_bubble(state->page, true);
    if (!voice_projection_current(state)) return false;
    voice_render(state);
    orb_screen_page_bind(instance, state->list, state->heading);
    voice_projection_end(state);
    return true;
}

void orb_app_voice_meter(orb_screen_instance_t *instance)
{
    if (!orb_screen_alive(instance)) return;
    voice_state_t *state = orb_screen_state(instance);
    if (voice_projection_current(state)) voice_meter(state);
    voice_projection_end(state);
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    /* The shell invokes the retained meter at its original poll boundary.
     * Stage updates must not fit hidden idle text or advance phase state. */
    (void)instance; (void)update;
}

static void destroy(orb_screen_instance_t *instance) { voice_projection_end(orb_screen_state(instance)); }
static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(voice_context_t)) return false;
    const voice_state_t *state = orb_screen_state(instance);
    voice_context_t value = {state->reading_identity, state->reading_scroll, state->result_kind};
    memcpy(context->payload, &value, sizeof(value));
    return true;
}
static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    voice_state_t *state = orb_screen_state(instance);
    voice_context_t value; memcpy(&value, context->payload, sizeof(value));
    if (value.scroll < 0 || value.result_kind > ORB_RESULT_UNKNOWN + 1) return false;
    state->result_changed = state->outcome_live && state->result_symbol &&
        value.identity == state->reading_identity && value.result_kind && value.result_kind != state->result_kind;
    /* Pages without a reader leave the last reading anchor intact, exactly
     * like the former per-route slot across idle/no-speech lifetimes. */
    if (!state->list) state->reading_identity = value.identity;
    state->reading_scroll = state->reading_identity == value.identity ? value.scroll : 0;
    return true;
}
static orb_back_result_t back(orb_screen_instance_t *instance) { (void)instance; return ORB_BACK_LEAVE_SCREEN; }
static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{ (void)instance; (void)binding; return false; }
static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    voice_state_t *state = orb_screen_state(instance);
    if (value != ORB_VISIBILITY_ACTIVE || !state->result_changed) return;
    state->result_changed = false;
    orb_kit_arrival_complete(state->result_symbol, false);
    orb_kit_pop_in(state->result_symbol);
    if (state->result_kind == ORB_RESULT_DONE + 1) orb_kit_check_label_draw_on(state->result_symbol);
}
static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    const voice_state_t *state = orb_screen_state(instance);
    *value = (orb_screen_chrome_t){.title = state->heading ? orb_kit_rim_text_get(state->heading) : ""};
}
static const orb_context_field_t s_fields[] = {
    {offsetof(voice_context_t, identity), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(voice_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(voice_context_t, result_kind), sizeof(uint32_t), ORB_CONTEXT_U32},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
const orb_screen_descriptor_t *orb_app_voice_descriptor(void)
{
    static const orb_screen_descriptor_t s_descriptor = {
        .id = ORB_APP_VOICE_SCREEN, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops,
        .state_bytes = sizeof(voice_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(voice_context_t),
        .context_version = 2, .context_fields = s_fields, .context_field_count = 3,
        .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(voice_state_t),
            .context_bytes = sizeof(orb_screen_context_t) + sizeof(voice_context_t), .declared = true},
        .subscriptions = ORB_DOMAIN_MODEL | ORB_DOMAIN_DEVICE, .privacy = ORB_PRIVACY_PRIVATE,
        .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
    };
    return &s_descriptor;
}

static void scrolled(lv_event_t *event)
{
    voice_state_t *state = lv_event_get_user_data(event);
    state->reading_scroll = LV_MAX(0, lv_obj_get_scroll_y(lv_event_get_current_target_obj(event)));
}
void voice_reading_restore(voice_state_t *state, lv_obj_t *area, unsigned surface, const char *identity)
{
    uint32_t key = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)identity; *p; p++) key = (key ^ *p) * 16777619u;
    state->reading_identity = key; state->restore_pending = true;
    lv_obj_set_user_data(area, (void *)(uintptr_t)surface);
    lv_obj_add_event_cb(area, scrolled, LV_EVENT_SCROLL, state);
}
bool orb_app_voice_restore_scroll(orb_screen_instance_t *instance, int *scroll)
{
    if (!orb_screen_alive(instance) || !scroll) return false;
    voice_state_t *state = orb_screen_state(instance);
    if (!state->restore_pending) return false;
    *scroll = state->reading_scroll; state->restore_pending = false;
    return true;
}

bool orb_app_voice_allow_synthetic(const orb_intent_t *intent)
{
    if (!intent || intent->kind != ORB_INTENT_APP_COMMAND) return false;
    unsigned action = intent->value.command.action;
    return action == ACT_VOICE_STOP || action == ACT_VOICE_CANCEL || action == ACT_VOICE_HOLD ||
        action == ACT_RESULT_RETRY || action == ACT_RESULT_DONE ||
        action == ORB_APP_VOICE_TOUCH_START || action == ORB_APP_VOICE_TOUCH_RELEASE;
}

bool orb_app_voice_activate(orb_screen_instance_t *instance, unsigned action, orb_input_origin_t origin, bool physical)
{
    if (!orb_screen_alive(instance)) return false;
    voice_state_t *state = orb_screen_state(instance);
    if (!voice_projection_current(state)) return false;
    orb_screen_host_t *host = orb_screen_host(instance);
    bool touch = action == ORB_APP_VOICE_TOUCH_START || action == ORB_APP_VOICE_TOUCH_RELEASE;
    bool allowed = !host->config.preview && !state->fixture &&
        orb_screen_visibility_get(instance) == ORB_VISIBILITY_ACTIVE &&
        (touch ? (action == ORB_APP_VOICE_TOUCH_RELEASE || state->touch_allowed) : state->interactive);
    voice_projection_end(state);
    if (!allowed || (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON)) return false;
    orb_intent_t intent = {.kind = action == ACT_VOICE_SEND ? ORB_INTENT_SEND : ORB_INTENT_APP_COMMAND,
        .origin = origin, .physical = physical};
    intent.value.command.action = action;
    /* Done may synchronously leave and destroy the sender. No state after emit. */
    return orb_screen_emit(instance, &intent);
}
void voice_clicked(lv_event_t *event)
{
    voice_state_t *state = lv_event_get_user_data(event);
    orb_app_voice_activate(state->instance, (unsigned)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event)),
        ORB_INPUT_TOUCH, !orb_input_is_synthetic(lv_indev_active()));
}
void voice_hold(lv_event_t *event)
{
    voice_state_t *state = lv_event_get_user_data(event);
    lv_obj_t *control = lv_event_get_current_target_obj(event);
    bool begin = lv_event_get_code(event) == LV_EVENT_LONG_PRESSED;
    if (begin && (lv_obj_has_state(control, LV_STATE_DISABLED) || orb_kit_press_is_cancelled(control))) return;
    orb_app_voice_activate(state->instance, begin ? ORB_APP_VOICE_TOUCH_START : ORB_APP_VOICE_TOUCH_RELEASE,
        ORB_INPUT_TOUCH, !orb_input_is_synthetic(lv_indev_active()));
}
