#include "orb_app_settings_battery.h"
#include "orb_app_settings.h"
#include "orb_page.h"
#include "settings_list_page.h"
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_effects.h"
#include "orb_kit_pages.h"
#include "orb_ui.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t scroll;
} battery_context_t;

typedef struct {
    orb_settings_battery_model_t model;
    lv_obj_t *heading, *list, *number, *health, *temperature, *status, *desk;
    bool projected;
} battery_state_t;

static const char *state_text(const orb_settings_battery_model_t *model)
{
    return model->present && model->desk_limit_active ? "Desk limit reached" :
        model->present && model->charging ? "Charging" :
        model->usb || model->external_usb ? "Connected to power" :
        model->present ? "On battery" : "Battery unavailable";
}

static void detail_text(const orb_settings_battery_model_t *model,
    char *health, size_t health_size, char *temperature, size_t temperature_size)
{
    /* Removal invalidates previously measured values, as in the old presenter. */
    if (model->present && model->health_valid && model->health_pct >= 0 && model->health_pct <= 100)
        snprintf(health, health_size, "%d%%", model->health_pct);
    else snprintf(health, health_size, "Unavailable");
    if (model->present && model->temperature_valid && isfinite(model->temperature_c)) {
        float value = model->fahrenheit ? model->temperature_c * 1.8f + 32 : model->temperature_c;
        snprintf(temperature, temperature_size, "%.0f°%s", (double)value, model->fahrenheit ? "F" : "C");
    } else snprintf(temperature, temperature_size, "Unavailable");
}

static void restore_checked(battery_state_t *state)
{
    if (state && state->desk)
        orb_kit_switch_set_checked(orb_kit_row_switch(state->desk), state->model.desk_charge_limit, false);
}

bool orb_app_settings_battery_toggle_desk_limit(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical)
{
    battery_state_t *state = orb_screen_state(instance);
    if (!state) return false;
    if (origin != ORB_INPUT_TOUCH) {
        restore_checked(state);
        return false;
    }
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = origin, .physical = physical};
    intent.value.setting.key = ORB_SETTINGS_KEY_DESK_LIMIT;
    intent.value.setting.value = !state->model.desk_charge_limit;
    bool emitted = orb_screen_emit(instance, &intent);
    if (!emitted) restore_checked(state);
    return emitted;
}

static void desk_changed(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_current_target_obj(event);
    if (lv_obj_get_style_opa(row, 0) == LV_OPA_TRANSP) return;
    lv_indev_t *input = lv_indev_active();
    if (input) {
        lv_point_t point;
        lv_indev_get_point(input, &point);
        if (!orb_round_list_hit_test(row, &point)) return;
    }
    /* The framework forwards a rejected synthetic contact to the shell's
     * existing hint policy, after its live opening/fixture/input guards. */
    (void)orb_app_settings_battery_toggle_desk_limit(instance, ORB_INPUT_TOUCH,
        input && !orb_input_is_synthetic(input));
}

static void build(orb_screen_instance_t *instance)
{
    battery_state_t *state = orb_screen_state(instance);
    const orb_settings_battery_model_t *model = &state->model;
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_clean(root);
    state->heading = orb_kit_rim_title_create(root, "Battery", ORB_KIT_SECONDARY);
    char level[24];
    bool valid = model->present && model->battery_pct >= 0 && model->battery_pct <= 100;
    if (valid) snprintf(level, sizeof(level), "%d%%", model->battery_pct);
    else snprintf(level, sizeof(level), "—");
    state->number = orb_kit_page_label(root, level, ORB_TEXT_LARGE_TITLE, true, 233, 82, 320, 1, ORB_LABEL);
    orb_text_apply(state->number, ORB_TEXT_LARGE_TITLE, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 0);
    orb_kit_value_set(state->number, level, "100%");
    state->status = orb_kit_page_label(root, state_text(model), ORB_TEXT_FOOTNOTE2, false,
        233, 165, 350, 1, ORB_KIT_SECONDARY);
    state->list = orb_kit_list_page(root,
        &(orb_kit_list_page_config_t){.top = 211, .height = 217, .pad_bottom = 96});
    char health[32], temperature[32];
    detail_text(model, health, sizeof(health), temperature, sizeof(temperature));
    state->health = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
        .title = "Health", .value = health, .glyph = ORB_ICON_BATTERY_HIGH, .accent = ORB_KIT_PLAY,
    });
    orb_kit_row_set_live_value(state->health, health, "Unavailable");
    lv_obj_set_clickable(state->health, false);
    state->desk = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
        .title = "Desk limit", .subtitle = model->battery_setting_error ? ORB_ICON_WARNING " Not saved" :
            model->desk_charge_limit ? "80% limit" : "Off",
        .glyph = ORB_ICON_BATTERY_CHARGING, .accent = ORB_KIT_PLAY,
        .show_switch = true, .checked = model->desk_charge_limit,
    });
    lv_obj_set_user_data(state->desk, (void *)(intptr_t)ORB_SETTINGS_KEY_DESK_LIMIT);
    lv_obj_add_event_cb(state->desk, desk_changed, LV_EVENT_VALUE_CHANGED, instance);
    state->temperature = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
        .title = "Temperature", .subtitle = temperature,
        .glyph = ORB_ICON_THERMOMETER, .accent = ORB_KIT_PLAY,
    });
    orb_kit_row_set_live_text(state->temperature, ORB_KIT_ROW_TEXT_SUBTITLE, temperature, "Unavailable");
    lv_obj_set_clickable(state->temperature, false);
    orb_scroll_edge_follow(state->list, 12, 16);
    orb_kit_list_apply(state->list);
    orb_kit_scroll_arc_attach(state->list);
    if (model->battery_setting_error) orb_settings_list_page_focus(state->list, state->desk);
    lv_obj_move_foreground(state->heading);
    orb_screen_page_bind(instance, state->list, state->heading);
}

static void refresh_values(battery_state_t *state)
{
    char level[24], health[32], temperature[32];
    bool valid = state->model.present && state->model.battery_pct >= 0 && state->model.battery_pct <= 100;
    if (valid) snprintf(level, sizeof(level), "%d%%", state->model.battery_pct);
    else snprintf(level, sizeof(level), "—");
    orb_kit_value_set_rolling(state->number, level, "100%");
    detail_text(&state->model, health, sizeof(health), temperature, sizeof(temperature));
    orb_kit_row_set_live_text_rolling(state->health, ORB_KIT_ROW_TEXT_VALUE, health, "Unavailable");
    orb_kit_row_set_live_text_rolling(state->temperature, ORB_KIT_ROW_TEXT_SUBTITLE, temperature, "Unavailable");
    const char *text = state_text(&state->model);
    if (strcmp(lv_label_get_text(state->status), text)) lv_label_set_text(state->status, text);
}

static int32_t maximum_scroll(battery_state_t *state)
{
    lv_obj_update_layout(state->list);
    return LV_MAX(0, lv_obj_get_scroll_y(state->list) + lv_obj_get_scroll_bottom(state->list));
}

static void set_scroll(battery_state_t *state, int32_t value)
{
    value = orb_screen_clamp_scroll(value, maximum_scroll(state));
    if (value == lv_obj_get_scroll_y(state->list)) return;
    if (!orb_round_list_scroll_to(state->list, value))
        lv_obj_scroll_to_y(state->list, value, LV_ANIM_OFF);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    lv_obj_set_clickable(orb_screen_root(instance), false);
    lv_obj_set_event_bubble(orb_screen_root(instance), true);
    battery_state_t *state = orb_screen_state(instance);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context,
        ORB_SETTINGS_BATTERY_SCREEN_ID, &projection) && projection.model &&
        projection.model->data && projection.model->size == sizeof(state->model)) {
        memcpy(&state->model, projection.model->data, sizeof(state->model));
        state->projected = true;
    }
    build(instance);
    return state->heading && state->list && state->number && state->desk;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    battery_state_t *state = orb_screen_state(instance);
    orb_settings_battery_model_t model = {0};
    if (update->model && update->model->data && update->model->size == sizeof(model))
        memcpy(&model, update->model->data, sizeof(model));
    bool rebuild = !state->projected || state->model.desk_charge_limit != model.desk_charge_limit ||
        state->model.battery_setting_error != model.battery_setting_error;
    state->model = model;
    state->projected = true;
    if (rebuild) {
        int32_t scroll = lv_obj_get_scroll_y(state->list);
        build(instance);
        if (scroll && !model.battery_setting_error) {
            /* Preserve the old full-render path for a desk setting/result.
             * Ordinary telemetry retains its original widgets and fling. */
            lv_obj_scroll_to_y(state->list, scroll, LV_ANIM_OFF);
            orb_round_list_update(state->list);
        }
    } else refresh_values(state);
}

static void destroy(orb_screen_instance_t *instance)
{
    (void)instance;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(battery_context_t)) return false;
    const battery_state_t *state = orb_screen_state(instance);
    battery_context_t value = {.scroll = lv_obj_get_scroll_y(state->list)};
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    battery_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.scroll) orb_page_restore(((battery_state_t *)orb_screen_state(instance))->list,
        &(orb_page_context_t){.scroll = value.scroll});
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
    set_scroll(orb_screen_state(instance), (int32_t)lrintf(value));
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)commit; (void)origin; (void)physical;
    set_scroll(orb_screen_state(instance), (int32_t)lrintf(value));
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    battery_state_t *state = orb_screen_state(instance);
    int32_t maximum = maximum_scroll(state);
    if (!maximum) return false;
    *binding = (orb_rim_binding_t){.subpage = 0, .property = 1, .kind = ORB_RIM_SCROLL,
        .minimum = 0, .maximum = maximum,
        .value = orb_screen_clamp_scroll(lv_obj_get_scroll_y(state->list), maximum),
        .detent = 72, .units_per_turn = 900,
        .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{
    (void)instance; (void)value;
}

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    (void)instance;
    *value = (orb_screen_chrome_t){.title = "Battery"};
}

static const orb_context_field_t s_context_fields[] = {
    {offsetof(battery_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SETTINGS_BATTERY_SCREEN_ID, .parent_id = ORB_SETTINGS_ROOT_SCREEN_ID, .ops = &s_ops,
    .state_bytes = sizeof(battery_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(battery_context_t),
    .context_version = 1, .context_fields = s_context_fields,
    .context_field_count = sizeof(s_context_fields) / sizeof(s_context_fields[0]),
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(battery_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(battery_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE | ORB_DOMAIN_MODEL, .privacy = ORB_PRIVACY_PUBLIC,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_settings_battery_descriptor(void)
{
    return &s_descriptor;
}

lv_obj_t *orb_app_settings_battery_scroll(const orb_screen_instance_t *instance)
{
    const battery_state_t *state = orb_screen_state(instance);
    return state ? state->list : NULL;
}

bool orb_app_settings_battery_desk_checked(const orb_screen_instance_t *instance)
{
    const battery_state_t *state = orb_screen_state(instance);
    return state && state->desk && orb_kit_switch_get_checked(orb_kit_row_switch(state->desk));
}

bool orb_app_settings_battery_desk_bounds(const orb_screen_instance_t *instance, lv_area_t *bounds)
{
    const battery_state_t *state = orb_screen_state(instance);
    if (!state || !state->desk || !bounds) return false;
    lv_obj_update_layout(orb_screen_root(instance));
    return orb_round_list_row_bounds(state->desk, bounds);
}
