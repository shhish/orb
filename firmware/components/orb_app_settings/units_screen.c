#include "orb_app_settings_units.h"
#include "settings_list_page.h"
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_page.h"
#include "orb_kit.h"
#include "orb_kit_effects.h"
#include "orb_ui.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct { int32_t scroll; uint32_t wait_started; bool saving; } units_context_t;
typedef struct {
    orb_settings_units_model_t model;
    orb_settings_list_page_t page;
    lv_obj_t *choices[3], *status;
    int32_t status_top;
    char offline_text[64];
    bool temperature_route, saving;
    uint32_t wait_started;
} units_state_t;

static bool is_temperature(const orb_screen_instance_t *instance)
{
    const orb_screen_descriptor_t *descriptor = orb_screen_descriptor(instance);
    return descriptor && descriptor->id == ORB_SETTINGS_TEMPERATURE_SCREEN_ID;
}

bool orb_app_settings_temperature_enabled(const orb_screen_instance_t *instance)
{
    const units_state_t *state = orb_screen_state(instance);
    return state && is_temperature(instance) && state->model.ready && !state->model.prefs_pending;
}

orb_settings_temperature_selection_t orb_app_settings_temperature_selection(const orb_screen_instance_t *instance)
{
    const units_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.temperature_owner) return ORB_SETTINGS_TEMPERATURE_AUTOMATIC;
    return state->model.temperature == ORB_TEMPERATURE_F ?
        ORB_SETTINGS_TEMPERATURE_F : ORB_SETTINGS_TEMPERATURE_C;
}

bool orb_app_settings_units_open_temperature(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical, const orb_launch_token_t *launch)
{
    const orb_screen_descriptor_t *descriptor = orb_screen_descriptor(instance);
    if (!descriptor || descriptor->id != ORB_SETTINGS_UNITS_SCREEN_ID) return false;
    orb_intent_t intent = {.kind = ORB_INTENT_NAVIGATE, .origin = origin, .physical = physical};
    intent.value.navigate.screen = ORB_SETTINGS_TEMPERATURE_SCREEN_ID;
    if (launch) intent.value.navigate.launch = *launch;
    return orb_screen_emit(instance, &intent);
}

bool orb_app_settings_temperature_select(orb_screen_instance_t *instance,
    orb_settings_temperature_selection_t selection, orb_input_origin_t origin, bool physical)
{
    if (!orb_app_settings_temperature_enabled(instance) ||
        (unsigned)selection > ORB_SETTINGS_TEMPERATURE_C ||
        (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON)) return false;
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = origin, .physical = physical};
    intent.value.setting.key = ORB_SETTINGS_SETTING_TEMPERATURE;
    intent.value.setting.value = selection;
    return orb_screen_emit(instance, &intent);
}

static void row_clicked(lv_event_t *event)
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
    if (is_temperature(instance)) {
        orb_settings_temperature_selection_t selection = (intptr_t)lv_obj_get_user_data(row) - 277;
        (void)orb_app_settings_temperature_select(instance, selection, ORB_INPUT_TOUCH,
            input && !orb_input_is_synthetic(input));
    } else {
        lv_area_t box;
        lv_obj_get_coords(row, &box);
        orb_launch_token_t launch = {.identity = ORB_SETTINGS_TEMPERATURE_SCREEN_ID, .valid = true,
            .rect = {box.x1, box.y1, box.x2 - box.x1 + 1, box.y2 - box.y1 + 1}};
        /* The legacy route handler removes the resting row's pressed state
         * before scheduling navigation. Its old feedback cancellation was a
         * no-op for kit rows; retain the kit's own release behavior. */
        lv_obj_remove_state(row, LV_STATE_PRESSED);
        (void)orb_app_settings_units_open_temperature(instance, ORB_INPUT_TOUCH,
            input && !orb_input_is_synthetic(input), &launch);
    }
}

static lv_obj_t *row_create(orb_screen_instance_t *instance, unsigned slot,
    const char *title, const char *detail, const char *icon, lv_color_t accent)
{
    units_state_t *state = orb_screen_state(instance);
    lv_obj_t *row = orb_kit_row_create(state->page.list, &(orb_kit_row_config_t){
        .title = title, .subtitle = detail,
        .value = state->temperature_route ? "" : NULL,
        .glyph = icon && icon[0] ? icon : ORB_ICON_INFO, .accent = accent,
    });
    lv_obj_set_user_data(row, (void *)(intptr_t)(state->temperature_route ? 277 + slot : 20));
    lv_obj_set_event_bubble(row, true);
    lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, instance);
    orb_kit_row_wrap_title(row, ORB_TEXT_REGULAR);
    if (state->temperature_route) {
        state->choices[slot] = row;
        orb_kit_row_set_live_value(row, (state->model.known || state->model.ready) &&
            slot == (unsigned)orb_app_settings_temperature_selection(instance) ? ORB_ICON_CHECK : "", ORB_ICON_CHECK);
        /* Reserve the bounded model notice once, then use the ordinary
         * subtitle lane. Pending/results never grow buffers or rebuild rows. */
        char reservation[sizeof(state->model.notice)];
        memset(reservation, 'W', sizeof(reservation) - 1);
        reservation[sizeof(reservation) - 1] = '\0';
        orb_kit_row_set_live_text(row, ORB_KIT_ROW_TEXT_SUBTITLE, detail, reservation);
        orb_kit_row_set_live_text(row, ORB_KIT_ROW_TEXT_SUBTITLE, detail, "Mac preference");
    }
    return row;
}

static void temperature_values(orb_screen_instance_t *instance)
{
    units_state_t *state = orb_screen_state(instance);
    const orb_settings_units_model_t *model = &state->model;
    if (model->prefs_pending && !state->saving) state->wait_started = lv_tick_get();
    state->saving = model->prefs_pending;
    const char *feedback = model->notice[0] ? model->notice : model->prefs_pending ? "Saving…" :
        model->prefs_error[0] ? ORB_ICON_WARNING " Not saved" : NULL;
    for (unsigned i = 0; i < 3; i++) {
        orb_kit_row_set_live_value(state->choices[i], (model->known || model->ready) &&
            i == (unsigned)orb_app_settings_temperature_selection(instance) ? ORB_ICON_CHECK : "", NULL);
        orb_kit_row_set_live_text(state->choices[i], ORB_KIT_ROW_TEXT_SUBTITLE,
            feedback ? feedback : i == 0 ? "Mac preference" : "", NULL);
        orb_kit_row_set_waiting_since(state->choices[i], ORB_KIT_ROW_TEXT_SUBTITLE,
            state->saving && !model->notice[0], state->wait_started);
        orb_kit_row_set_enabled(state->choices[i], orb_app_settings_temperature_enabled(instance));
    }
    orb_kit_value_set(state->status, model->known ? state->offline_text : "Mac offline", NULL);
    lv_obj_set_hidden(state->status, model->ready);
    int32_t top = model->ready ? 88 : state->status_top;
    lv_obj_set_y(state->page.list, top);
    lv_obj_set_height(state->page.list, 448 - top);
}

static void build(orb_screen_instance_t *instance)
{
    units_state_t *state = orb_screen_state(instance);
    orb_settings_list_page_begin(orb_screen_root(instance), state->temperature_route ? "Temperature" : "Units", &state->page);
    lv_obj_t *selected_row = NULL;
    if (!state->temperature_route) {
        selected_row = row_create(instance, 0, "Temperature", state->model.temperature == ORB_TEMPERATURE_F ? "°F" : "°C",
            ORB_ICON_THERMOMETER, ORB_KIT_GREY);
    } else {
        static const char *s_names[] = {"Automatic", "°F", "°C"};
        /* Unknown offline: no checkmark claims a unit the hub never confirmed. */
        unsigned selected = state->model.known || state->model.ready ? orb_app_settings_temperature_selection(instance) : 3;
        for (unsigned i = 0; i < 3; i++) {
            /* HIG §§1/5/7: the existing radio list has one neutral checkmark. */
            lv_obj_t *row = row_create(instance, i, s_names[i], i == 0 ? "Mac preference" : "",
                ORB_ICON_THERMOMETER, ORB_KIT_GREY);
            if (i == selected) selected_row = row;
        }
        state->status = orb_settings_list_page_status(orb_screen_root(instance), state->page.list, ORB_KIT_OFFLINE_CAPTION);
        state->status_top = lv_obj_get_y(state->page.list);
        snprintf(state->offline_text, sizeof(state->offline_text), "%s", lv_label_get_text(state->status));
        orb_kit_value_set(state->status, state->offline_text, ORB_KIT_OFFLINE_CAPTION);
        temperature_values(instance);
    }
    orb_settings_list_page_end(&state->page);
    if (selected_row) orb_settings_list_page_focus(state->page.list, selected_row);
    orb_screen_page_bind(instance, state->page.list, state->page.heading);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    units_state_t *state = orb_screen_state(instance);
    state->temperature_route = is_temperature(instance);
    /* Seed the exact immutable projection before preparing rows. Router
     * update() sees that same value and need not rebuild or emit scroll events. */
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    const orb_screen_descriptor_t *descriptor = orb_screen_descriptor(instance);
    if (host->config.project && host->config.project(host->config.context,
        descriptor->id, &projection) && projection.model && projection.model->data &&
        projection.model->size == sizeof(state->model))
        memcpy(&state->model, projection.model->data, sizeof(state->model));
    build(instance);
    return state->page.heading && state->page.list;
}

static bool model_equal(const orb_settings_units_model_t *a, const orb_settings_units_model_t *b)
{
    return a->temperature == b->temperature && a->temperature_owner == b->temperature_owner &&
        a->prefs_pending == b->prefs_pending && a->ready == b->ready && a->known == b->known &&
        !strcmp(a->prefs_error, b->prefs_error) && !strcmp(a->notice, b->notice);
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    units_state_t *state = orb_screen_state(instance);
    orb_settings_units_model_t model = {0};
    if (update->model && update->model->data && update->model->size == sizeof(model))
        memcpy(&model, update->model->data, sizeof(model));
    if (model_equal(&state->model, &model)) return;
    int32_t scroll = lv_obj_get_scroll_y(state->page.list);
    if (state->temperature_route) {
        /* HIG §§1/7: the fixed radio rows retain the person's reading place. */
        state->model = model;
        temperature_values(instance);
        orb_settings_list_page_scroll(&state->page, scroll);
        return;
    }
    state->model = model;
    /* Rebuild only for real preference/result changes: the selected glyph and
     * disabled radio state belong to this composition, not kit child topology. */
    build(instance);
    orb_settings_list_page_scroll(&state->page, scroll);
}

static void destroy(orb_screen_instance_t *instance)
{
    (void)instance;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(units_context_t)) return false;
    const units_state_t *state = orb_screen_state(instance);
    units_context_t value = {0};
    value.scroll = lv_obj_get_scroll_y(state->page.list);
    value.wait_started = state->wait_started; value.saving = state->saving;
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    units_state_t *state = orb_screen_state(instance);
    units_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.saving && state->saving) state->wait_started = value.wait_started;
    if (state->temperature_route) temperature_values(instance);
    if (value.scroll || lv_obj_get_scroll_y(state->page.list))
        orb_settings_list_page_scroll(&state->page, value.scroll);
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
    units_state_t *state = orb_screen_state(instance);
    orb_settings_list_page_scroll(&state->page, (int32_t)lrintf(value));
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)commit; (void)origin; (void)physical;
    rim_preview(instance, value);
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    units_state_t *state = orb_screen_state(instance);
    int32_t maximum = orb_settings_list_page_maximum(&state->page);
    if (!maximum) return false;
    *binding = (orb_rim_binding_t){.property = 1, .kind = ORB_RIM_SCROLL,
        .minimum = 0, .maximum = maximum,
        .value = orb_screen_clamp_scroll(lv_obj_get_scroll_y(state->page.list), maximum),
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
    *value = (orb_screen_chrome_t){.title = is_temperature(instance) ? "Temperature" : "Units"};
}

static const orb_context_field_t s_context_fields[] = {
    {offsetof(units_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(units_context_t, wait_started), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(units_context_t, saving), sizeof(bool), ORB_CONTEXT_BOOL},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
#define UNITS_DESCRIPTOR(ID, PARENT) { \
    .id = ID, .parent_id = PARENT, .ops = &s_ops, .state_bytes = sizeof(units_state_t), \
    .context_bytes = sizeof(orb_screen_context_t) + sizeof(units_context_t), .context_version = 2, \
    .context_fields = s_context_fields, .context_field_count = 3, \
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(units_state_t), \
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(units_context_t), .declared = true}, \
    .subscriptions = ORB_DOMAIN_MODEL | ORB_DOMAIN_DEVICE, .privacy = ORB_PRIVACY_PUBLIC, \
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN, \
}
static const orb_screen_descriptor_t s_units = UNITS_DESCRIPTOR(ORB_SETTINGS_UNITS_SCREEN_ID, 8);
static const orb_screen_descriptor_t s_temperature = UNITS_DESCRIPTOR(ORB_SETTINGS_TEMPERATURE_SCREEN_ID, ORB_SETTINGS_UNITS_SCREEN_ID);
#undef UNITS_DESCRIPTOR

const orb_screen_descriptor_t *orb_app_settings_units_descriptor(void)
{
    return &s_units;
}

const orb_screen_descriptor_t *orb_app_settings_temperature_descriptor(void)
{
    return &s_temperature;
}

lv_obj_t *orb_app_settings_units_scroll(const orb_screen_instance_t *instance)
{
    const units_state_t *state = orb_screen_state(instance);
    return state ? state->page.list : NULL;
}
