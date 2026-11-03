/* HIG §§3–7: retain the round launcher, flat identity discs and the existing
 * focus spring. The shell owns refresh opportunities and launch policy. */
#include "apps_screen.h"
#include "orb_shell.h"
#include "orb_feedback.h"
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_effects.h"
#include "orb_memory.h"
#include "orb_ui.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    int32_t focused, previous;
} apps_context_t;

typedef struct {
    uint16_t version, size;
    apps_context_t value;
} apps_record_t;

typedef struct {
    apps_context_t context;
    bool time_available;
    bool (*active)(void *);
    void *owner;
} apps_model_t;

typedef struct {
    apps_model_t model;
    lv_obj_t *caption, *discs[7];
    unsigned built;
    uint32_t arrival_generation;
    bool arrival_final;
} apps_state_t;

struct orb_apps_view {
    orb_app_host_t *host;
    apps_record_t saved;
    bool preview;
    bool (*active)(void *);
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *);
    void *context;
};

static const struct { const char *name, *icon; int action, x, y; } s_items[] = {
    {"Agents", ORB_ICON_PROMPT, ORB_SHELL_AGENTS, 233, 133},
    {"Dot", ORB_ICON_CHAT, ORB_SHELL_DOT, 320, 183},
    {"Home", ORB_ICON_HOUSE, ORB_SHELL_HOME, 146, 183},
    {"Time", ORB_ICON_TIMER, ORB_APPS_TIME, 233, 233},
    {"Play", ORB_ICON_GAME_CONTROLLER, ORB_SHELL_PLAY, 320, 283},
    {"Updates", ORB_ICON_UPLOAD, ORB_SHELL_UPDATES, 146, 283},
    {"Settings", ORB_ICON_GEAR, ORB_SHELL_SETTINGS, 233, 333},
};

static const char *name(int action)
{
    for (unsigned i = 0; i < 7; i++) if (s_items[i].action == action) return s_items[i].name;
    return "Agents";
}

static lv_color_t colour(unsigned index)
{
    switch (index) {
    case 0: return ORB_KIT_CLAUDE;
    case 1: return ORB_KIT_DOT;
    case 2: return ORB_KIT_HOME;
    case 3: return ORB_KIT_TIME;
    case 4: return ORB_KIT_PLAY;
    case 5: return ORB_KIT_CODEX;
    default: return ORB_KIT_GREY;
    }
}

static bool interactive(orb_screen_instance_t *instance)
{
    if (!orb_screen_alive(instance) || orb_screen_host(instance)->config.preview ||
        orb_screen_visibility_get(instance) != ORB_VISIBILITY_ACTIVE) return false;
    const apps_state_t *state = orb_screen_state(instance);
    /* The shell can change route before its queued page deletion. A late
     * release must not mutate the departed launcher's remembered focus. */
    return !state->model.active || state->model.active(state->model.owner);
}

static void clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target_obj(event);
    lv_area_t bounds; lv_obj_get_coords(target, &bounds);
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = lv_indev_active() && !orb_input_is_synthetic(lv_indev_active()),
        .launch = {.valid = true, .rect = {bounds.x1, bounds.y1,
            bounds.x2 - bounds.x1 + 1, bounds.y2 - bounds.y1 + 1}},
        .value.command = {.target = ORB_SHELL_APPS,
            .action = (uint32_t)(uintptr_t)lv_obj_get_user_data(target)}};
    /* The accepted command can destroy the sender synchronously. */
    (void)orb_screen_emit(instance, &intent);
}

static void touch(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    if (!interactive(instance)) return;
    apps_state_t *state = orb_screen_state(instance);
    if (!state->caption) return;
    lv_obj_t *target = lv_event_get_current_target_obj(event);
    apps_context_t *context = &state->model.context;
    int previous = context->focused;
    bool cancel = false;
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        context->previous = context->focused;
        context->focused = (int)(intptr_t)lv_obj_get_user_data(target);
    } else {
        cancel = lv_event_get_code(event) == LV_EVENT_PRESS_LOST;
        lv_indev_t *input = lv_indev_active();
        if (input && lv_event_get_code(event) == LV_EVENT_RELEASED) {
            lv_point_t point; lv_indev_get_point(input, &point);
            lv_area_t bounds; lv_obj_get_coords(target, &bounds);
            cancel = point.x < bounds.x1 || point.x > bounds.x2 || point.y < bounds.y1 || point.y > bounds.y2;
        }
        if (cancel) context->focused = context->previous;
    }
    orb_kit_rim_text_set(state->caption, name(context->focused), NULL);
    for (unsigned i = 0; i < state->built; i++)
        orb_kit_disc_set_focus(state->discs[i], s_items[i].action == context->focused);
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = lv_indev_active() && !orb_input_is_synthetic(lv_indev_active()),
        .value.command = {.target = ORB_SHELL_APPS, .action = cancel ? ORB_APPS_CANCEL : ORB_APPS_FOCUS,
            .value = previous, .extra = context->focused}};
    (void)orb_screen_emit(instance, &intent);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    apps_state_t *state = orb_screen_state(instance);
    state->arrival_generation = orb_kit_motion_interrupt_generation();
    state->model.context.focused = ORB_SHELL_HOME;
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context, ORB_SHELL_APPS, &projection) &&
        projection.model && projection.model->data && projection.model->size == sizeof(state->model))
        memcpy(&state->model, projection.model->data, sizeof(state->model));
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    return true;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *projection)
{
    /* Input owns focus, and only an explicit shell build opportunity creates
     * discs. Staging/equal revisions must not add another progressive slice. */
    (void)instance; (void)projection;
}

static void destroy(orb_screen_instance_t *instance) { (void)instance; }

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(apps_record_t)) return false;
    const apps_state_t *state = orb_screen_state(instance);
    memcpy(context->payload, &state->model.context, sizeof(apps_context_t));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    apps_state_t *state = orb_screen_state(instance);
    memcpy(&state->model.context, context->payload, sizeof(apps_context_t));
    return true;
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{ (void)instance; return ORB_BACK_LEAVE_SCREEN; }
static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{ (void)instance; (void)binding; return false; }
static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{ (void)instance; (void)value; }
static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{ (void)instance; *value = (orb_screen_chrome_t){.title = "Apps"}; }

static const orb_context_field_t s_fields[] = {
    {offsetof(apps_context_t, focused), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(apps_context_t, previous), sizeof(int32_t), ORB_CONTEXT_I32},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SHELL_APPS, .parent_id = ORB_SHELL_FACE, .ops = &s_ops,
    .state_bytes = sizeof(apps_state_t), .context_bytes = sizeof(apps_record_t), .context_version = 1,
    .context_fields = s_fields, .context_field_count = 2,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(apps_state_t),
        .context_bytes = sizeof(apps_record_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE, .privacy = ORB_PRIVACY_PRIVATE,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_apps_descriptor(void) { return &s_descriptor; }

bool orb_apps_screen_build(orb_screen_instance_t *instance, bool progressive, bool time_available)
{
    if (!orb_screen_alive(instance)) return true;
    apps_state_t *state = orb_screen_state(instance);
    state->model.time_available = time_available;
    unsigned first = state->built;
    unsigned last = progressive ? LV_MIN(first + 1, 7u) : 7u;
    bool valid = false;
    for (unsigned i = 0; i < 7; i++) valid |= s_items[i].action == state->model.context.focused;
    if (!valid) state->model.context.focused = ORB_SHELL_HOME;
    for (unsigned i = first; i < last; i++) {
        lv_obj_t *disc = orb_kit_disc_create(orb_screen_root(instance), ORB_KIT_DISC_STANDARD,
            ORB_KIT_ACCENT_FILL, colour(i), s_items[i].icon);
        if (!disc) return false;
        state->discs[i] = disc;
        lv_obj_set_pos(disc, s_items[i].x - 40, s_items[i].y - 40);
        lv_obj_set_user_data(disc, (void *)(intptr_t)s_items[i].action);
        lv_obj_set_event_bubble(disc, true);
        lv_obj_add_event_cb(disc, clicked, LV_EVENT_CLICKED, instance);
        lv_obj_add_event_cb(disc, touch, LV_EVENT_PRESSED, instance);
        lv_obj_add_event_cb(disc, touch, LV_EVENT_PRESS_LOST, instance);
        lv_obj_add_event_cb(disc, touch, LV_EVENT_RELEASED, instance);
        orb_kit_disc_set_focus(disc, s_items[i].action == state->model.context.focused);
        if (s_items[i].action == ORB_APPS_TIME && !state->model.time_available) lv_obj_add_state(disc, LV_STATE_DISABLED);
        if (!state->arrival_final && state->arrival_generation == orb_kit_motion_interrupt_generation())
            orb_kit_pop_in_arrival(disc);
        state->built++;
    }
    if (last < 7) return false;
    if (!state->caption) {
        state->caption = orb_kit_rim_caption_create(orb_screen_root(instance), name(state->model.context.focused));
        orb_kit_rim_caption_set_style(state->caption, ORB_TEXT_CAPTION2,
            ORB_TEXT_EMPHASIZED, ORB_LABEL, LV_OPA_COVER);
    }
    return true;
}

bool orb_apps_arrival_prepare(orb_screen_instance_t *instance, bool fresh)
{
    if (!orb_screen_alive(instance)) return false;
    apps_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    /* Progressive construction or a warm retained plane can finish a token's
     * constructor fence early. Arm the complete catalogue at snapshot
     * admission, using the same pose again at the actual transition fence. */
    orb_kit_arrival_complete(root, false);
    if (!fresh) { state->arrival_final = true; orb_kit_motion_finish(root); return false; }
    if (state->arrival_final || state->arrival_generation != orb_kit_motion_interrupt_generation()) return false;
    for (unsigned i = 0; i < state->built; i++) orb_kit_pop_in_arrival(state->discs[i]);
    return !orb_ui_reduce_motion() && orb_kit_motion_settling(root);
}

static void capture(orb_apps_view_t *view)
{
    orb_screen_instance_t *screen = orb_apps_view_current(view);
    if (screen) (void)orb_screen_save(screen, (orb_screen_context_t *)&view->saved, sizeof(view->saved));
}

static bool command(void *context, orb_screen_instance_t *screen, const orb_intent_t *intent)
{
    orb_apps_view_t *view = context;
    return view->command && view->command(view->context, screen, intent);
}

static bool allow_synthetic(const orb_intent_t *intent)
{
    return intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_SHELL_APPS;
}

static void rim_finish(orb_screen_instance_t *instance, bool commit) { (void)instance; (void)commit; }

orb_apps_view_t *orb_apps_view_create(bool preview,
    bool (*accept)(void *, orb_screen_instance_t *, const orb_intent_t *),
    bool (*active)(void *), void *context)
{
    orb_apps_view_t *view = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(*view));
    if (view) {
        view->preview = preview; view->command = accept; view->active = active; view->context = context;
        view->saved = (apps_record_t){.version = 1, .size = sizeof(apps_record_t),
            .value.focused = ORB_SHELL_HOME};
    }
    return view;
}

size_t orb_apps_view_size(void) { return sizeof(orb_apps_view_t); }
orb_screen_instance_t *orb_apps_view_current(orb_apps_view_t *view)
{ return view ? orb_app_host_current(view->host) : NULL; }

void orb_apps_view_destroy(orb_apps_view_t *view)
{
    if (!view) return;
    orb_app_host_destroy(view->host);
    orb_memory_free(view);
}

void orb_apps_view_detach(orb_apps_view_t *view, lv_obj_t *page)
{
    if (!view) return;
    capture(view);
    orb_app_host_detach_page(view->host, page);
}

bool orb_apps_view_build(orb_apps_view_t *view, lv_obj_t *page, bool progressive, bool time_available)
{
    if (!view) return true;
    if (!view->host) view->host = orb_app_host_create(view->preview, &(orb_app_host_config_t){
        .descriptor = orb_apps_descriptor, .name = "Apps", .model_bytes = sizeof(apps_model_t),
        .forget_on_detach = true, .command = command, .context = view,
        .allow_synthetic = allow_synthetic, .rim_finish = rim_finish});
    if (!orb_apps_view_current(view)) {
        apps_model_t model = {.context = view->saved.value, .time_available = time_available,
            .active = view->active, .owner = view->context};
        if (!orb_app_host_set_model(view->host, &model, sizeof(model)) || !orb_app_host_attach(view->host, page)) return true;
    }
    return orb_apps_screen_build(orb_apps_view_current(view), progressive, time_available);
}

lv_obj_t *orb_apps_view_caption(orb_apps_view_t *view)
{
    orb_screen_instance_t *screen = orb_apps_view_current(view);
    apps_state_t *state = screen ? orb_screen_state(screen) : NULL;
    return state ? state->caption : NULL;
}

int orb_apps_view_focus(orb_apps_view_t *view)
{
    if (!view) return ORB_SHELL_HOME;
    capture(view);
    return view->saved.value.focused;
}

void orb_apps_view_select(orb_apps_view_t *view, int focused)
{
    if (!view) return;
    capture(view);
    view->saved.value.focused = focused;
    orb_screen_instance_t *screen = orb_apps_view_current(view);
    if (screen) ((apps_state_t *)orb_screen_state(screen))->model.context.focused = focused;
}

bool orb_apps_view_copy(orb_apps_view_t *destination, orb_apps_view_t *source)
{
    if (!destination || !source) return false;
    capture(source);
    destination->saved = source->saved;
    orb_screen_instance_t *screen = orb_apps_view_current(destination);
    return !screen || orb_screen_restore(screen, (orb_screen_context_t *)&destination->saved, sizeof(destination->saved));
}

void orb_apps_prepare_launch(orb_screen_instance_t *instance, unsigned action)
{
    if (!orb_screen_alive(instance)) return;
    apps_state_t *state = orb_screen_state(instance);
    for (unsigned i = 0; i < state->built; i++) if ((unsigned)s_items[i].action == action) {
        lv_obj_t *disc = state->discs[i];
        lv_obj_remove_state(disc, LV_STATE_PRESSED);
        orb_feedback_cancel(disc);
        orb_kit_press_cancel(disc);
        break;
    }
}
