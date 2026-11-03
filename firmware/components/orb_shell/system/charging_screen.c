/* HIG §§1/3/4/8: power telemetry is ordinary content, with the existing
 * inset ring and measured labels. This descriptor owns no power service. */
#include "charging_screen.h"
#include "../orb_place_view.h"
#include "orb_shell.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_page.h"
#include "orb_ui.h"

#include <string.h>

typedef struct {
    orb_charging_model_t model;
    orb_charging_view_t view;
    lv_obj_t *title;
    bool initial_update;
} charging_state_t;

orb_charging_model_t orb_charging_project(const orb_shell_snapshot_t *status, bool contrast)
{
    return (orb_charging_model_t){.battery_pct = status ? status->battery_pct : 0,
        .usb = status && status->usb, .charging = status && status->charging, .contrast = contrast};
}

static lv_color_t colour(orb_delight_color_t accent, bool contrast)
{
    if (accent == ORB_DELIGHT_GREEN) return contrast ? ORB_GREEN_CONTRAST : ORB_GREEN;
    if (accent == ORB_DELIGHT_RED) return contrast ? ORB_POWER_LOW_CONTRAST : ORB_POWER_LOW;
    return contrast ? ORB_GRAY_CONTRAST : ORB_GRAY;
}

static void apply(charging_state_t *state)
{
    const orb_charging_model_t *model = &state->model;
    orb_charging_copy_t value = orb_charging_copy(model->battery_pct, model->usb, model->charging);
    orb_charging_view_update(&state->view, model->battery_pct, value,
        colour(value.accent, model->contrast));
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    charging_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    /* The inert wrapper keeps shell gesture ownership and adds no ink. */
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context,
        ORB_SHELL_CHARGING, &projection) && projection.model && projection.model->data &&
        projection.model->size == sizeof(state->model))
        memcpy(&state->model, projection.model->data, sizeof(state->model));
    state->view.ring = orb_kit_ring_create(root, 213, 12,
        colour(ORB_DELIGHT_GRAY, state->model.contrast));
    /* The title remains neutral even when the charging state changes colour. */
    state->title = orb_kit_rim_title_inset_create(root, "Battery", ORB_LABEL);
    state->view.icon = orb_kit_page_label(root, ORB_ICON_LIGHTNING_FILL,
        ORB_TEXT_TITLE3, true, 233, 143, 96, 1, ORB_LABEL);
    state->view.number = orb_kit_page_label(root, "—",
        ORB_TEXT_LARGE_TITLE, true, 233, 201, 280, 1, ORB_LABEL);
    /* The PMU supplies no completion estimate; preserve the original caption. */
    state->view.caption = orb_kit_page_label(root, "",
        ORB_TEXT_FOOTNOTE2, false, 233, 288, 330, 1, ORB_KIT_SECONDARY);
    if (!state->view.ring || !state->title || !state->view.icon ||
        !state->view.number || !state->view.caption) return false;
    apply(state);
    state->initial_update = true;
    orb_screen_page_bind(instance, NULL, state->title);
    return true;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    charging_state_t *state = orb_screen_state(instance);
    orb_charging_model_t model = {0};
    if (update->model && update->model->data && update->model->size == sizeof(model))
        memcpy(&model, update->model->data, sizeof(model));
    bool initial_same = state->initial_update && state->model.battery_pct == model.battery_pct &&
        state->model.usb == model.usb && state->model.charging == model.charging &&
        state->model.contrast == model.contrast;
    state->initial_update = false;
    state->model = model;
    /* Staging repeats creation's projection. Its extra fit would move the
     * first paint; subsequent telemetry keeps the original update cadence. */
    if (!initial_same) apply(state);
}

static void destroy(orb_screen_instance_t *instance)
{
    (void)instance;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    (void)instance;
    return capacity >= sizeof(*context);
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    (void)instance; (void)context;
    return true;
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    (void)instance;
    return ORB_BACK_LEAVE_SCREEN;
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    (void)instance; (void)binding;
    return false;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    (void)instance; (void)value;
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Battery", .inset_ring = true};
}

static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SHELL_CHARGING, .parent_id = ORB_SHELL_FACE, .ops = &s_ops,
    .state_bytes = sizeof(charging_state_t), .context_bytes = sizeof(orb_screen_context_t),
    .context_version = 1,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(charging_state_t),
        .context_bytes = sizeof(orb_screen_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE | ORB_DOMAIN_TIME, .privacy = ORB_PRIVACY_PUBLIC,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_charging_descriptor(void)
{
    return &s_descriptor;
}

static void rim_finish(orb_screen_instance_t *instance, bool commit)
{
    (void)instance; (void)commit;
}

orb_app_host_t *orb_charging_host_create(bool preview)
{
    return orb_app_host_create(preview, &(orb_app_host_config_t){
        .descriptor = orb_charging_descriptor, .name = "Charging", .forget_on_detach = true,
        .model_bytes = sizeof(orb_charging_model_t), .rim_finish = rim_finish});
}

bool orb_charging_host_attach(orb_app_host_t *host, lv_obj_t *page, const orb_charging_model_t *model)
{
    if (!model || !orb_app_host_set_model(host, model, sizeof(*model)) || !orb_app_host_attach(host, page)) return false;
    /* Shell chrome stays outside the moving body during an opening. The
     * framework tracks this title and releases it with the descriptor. */
    lv_obj_t *title = orb_screen_page_title(orb_app_host_current(host));
    if (title) lv_obj_set_parent(title, page);
    return true;
}

bool orb_charging_host_update(orb_app_host_t *host, const orb_charging_model_t *model)
{
    if (!model || !orb_app_host_current(host) || !orb_app_host_set_model(host, model, sizeof(*model))) return false;
    orb_app_host_poll(host);
    return true;
}
