#include "orb_app_settings.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_ui.h"
#include "orb_page.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint32_t page;
    int32_t scroll;
} about_context_t;

typedef struct {
    orb_settings_about_model_t model;
    uint32_t page;
    lv_obj_t *heading, *list, *values[3];
} about_state_t;

static int32_t maximum_scroll(about_state_t *state)
{
    lv_obj_update_layout(state->list);
    return LV_MAX(0, lv_obj_get_scroll_y(state->list) + lv_obj_get_scroll_bottom(state->list));
}

static void set_scroll(about_state_t *state, int32_t value)
{
    value = orb_screen_clamp_scroll(value, maximum_scroll(state));
    if (!orb_round_list_scroll_to(state->list, value))
        lv_obj_scroll_to_y(state->list, value, LV_ANIM_OFF);
}

bool orb_app_settings_about_open_licences(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical, const orb_launch_token_t *launch)
{
    about_state_t *state = orb_screen_state(instance);
    if (!state || state->page != ORB_SETTINGS_ABOUT_PAGE) return false;
    orb_intent_t intent = {.kind = ORB_INTENT_NAVIGATE, .origin = origin, .physical = physical};
    intent.value.navigate.screen = ORB_SETTINGS_ABOUT_SCREEN_ID;
    intent.value.navigate.args.page = ORB_SETTINGS_LICENCES_PAGE;
    if (launch) intent.value.navigate.launch = *launch;
    return orb_screen_emit(instance, &intent);
}

static void licences_clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_target_obj(event);
    if (lv_obj_get_style_opa(row, 0) == LV_OPA_TRANSP) return;
    lv_indev_t *input = lv_indev_active();
    if (input) {
        lv_point_t point;
        lv_indev_get_point(input, &point);
        if (!orb_round_list_hit_test(row, &point)) return;
    }
    lv_area_t box;
    lv_obj_get_coords(row, &box);
    orb_launch_token_t launch = {.identity = ORB_SETTINGS_LICENCES_PAGE, .valid = true,
        .rect = {box.x1, box.y1, box.x2 - box.x1 + 1, box.y2 - box.y1 + 1}};
    (void)orb_app_settings_about_open_licences(instance, ORB_INPUT_TOUCH, input != NULL, &launch);
}

static lv_obj_t *preference_row(orb_screen_instance_t *instance,
    const char *title, const char *detail, const char *icon, bool licences)
{
    about_state_t *state = orb_screen_state(instance);
    lv_obj_t *row = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
        .title = title, .subtitle = detail,
        .glyph = icon && icon[0] ? icon : ORB_ICON_INFO, .accent = ORB_KIT_GREY,
    });
    lv_obj_set_event_bubble(row, true);
    if (licences) {
        lv_obj_set_user_data(row, (void *)(uintptr_t)440);
        lv_obj_add_event_cb(row, licences_clicked, LV_EVENT_CLICKED, instance);
    }
    else lv_obj_set_clickable(row, false);
    orb_kit_row_wrap_title(row, ORB_TEXT_REGULAR);
    return row;
}

static const char *mac_text(const about_state_t *state)
{
    return !state->model.ready ? state->model.connection_text :
        state->model.hub_name[0] ? state->model.hub_name : "Connected";
}

static void build(orb_screen_instance_t *instance)
{
    about_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_clean(root);
    memset(state->values, 0, sizeof(state->values));
    state->heading = orb_kit_rim_title_create(root,
        state->page == ORB_SETTINGS_LICENCES_PAGE ? "Licenses" : "About", ORB_KIT_SECONDARY);
    lv_obj_update_layout(root);
    state->list = orb_kit_list_page(root,
        &(orb_kit_list_page_config_t){.top = 88, .height = 360, .pad_bottom = 24, .reading = true});
    if (state->page == ORB_SETTINGS_LICENCES_PAGE) {
        static const char *s_names[] = {"Inter", "Nunito", "Framework7 icons", "Phosphor icons"};
        for (unsigned i = 0; i < 4; i++)
            preference_row(instance, s_names[i], i < 2 ? "SIL Open Font License" : "MIT",
                i < 2 ? ORB_ICON_PENCIL : ORB_ICON_APPS, false);
    } else {
        state->values[0] = preference_row(instance, "Version",
            state->model.firmware_version[0] ? state->model.firmware_version : "Unavailable", ORB_ICON_GEAR, false);
        state->values[1] = preference_row(instance, "Mac", mac_text(state), ORB_ICON_DESKTOP, false);
        state->values[2] = preference_row(instance, "Device ID",
            state->model.device_id[0] ? state->model.device_id : "Unavailable", ORB_ICON_WATCH, false);
        preference_row(instance, "Licenses", "Fonts and icons", ORB_ICON_FILE_TEXT, true);
    }
    lv_obj_set_style_pad_top(state->list, ORB_KIT_LIST_INSET, 0);
    orb_kit_list_page_finish(state->list, 120);
    lv_obj_move_foreground(state->heading);
    orb_screen_page_bind(instance, state->list, state->heading);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    about_state_t *state = orb_screen_state(instance);
    state->page = args->page == ORB_SETTINGS_LICENCES_PAGE ? ORB_SETTINGS_LICENCES_PAGE : ORB_SETTINGS_ABOUT_PAGE;
    /* A style-free wrapper adds neither ink nor a semantic control to parity
     * traces. Bubble input to the shell's existing gesture policy. */
    lv_obj_set_clickable(orb_screen_root(instance), false);
    lv_obj_set_event_bubble(orb_screen_root(instance), true);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context,
        ORB_SETTINGS_ABOUT_SCREEN_ID, &projection) && projection.model &&
        projection.model->data && projection.model->size == sizeof(state->model))
        memcpy(&state->model, projection.model->data, sizeof(state->model));
    build(instance);
    return state->heading && state->list;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    about_state_t *state = orb_screen_state(instance);
    orb_settings_about_model_t model = {0};
    if (update->model && update->model->data && update->model->size == sizeof(model))
        memcpy(&model, update->model->data, sizeof(model));
    state->model = model;
    if (state->page != ORB_SETTINGS_ABOUT_PAGE) return;
    orb_kit_row_set_text(state->values[0], ORB_KIT_ROW_TEXT_SUBTITLE,
        model.firmware_version[0] ? model.firmware_version : "Unavailable");
    orb_kit_row_set_text(state->values[1], ORB_KIT_ROW_TEXT_SUBTITLE, mac_text(state));
    orb_kit_row_set_text(state->values[2], ORB_KIT_ROW_TEXT_SUBTITLE,
        model.device_id[0] ? model.device_id : "Unavailable");
}

static void destroy(orb_screen_instance_t *instance)
{
    (void)instance;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(about_context_t)) return false;
    const about_state_t *state = orb_screen_state(instance);
    about_context_t value = {.page = state->page, .scroll = lv_obj_get_scroll_y(state->list)};
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    about_state_t *state = orb_screen_state(instance);
    about_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.page > ORB_SETTINGS_LICENCES_PAGE) return false;
    if (state->page != value.page) {
        state->page = value.page;
        build(instance);
    }
    set_scroll(state, value.scroll);
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
    about_state_t *state = orb_screen_state(instance);
    int32_t maximum = maximum_scroll(state);
    if (!maximum) return false;
    *binding = (orb_rim_binding_t){.subpage = state->page, .property = 1,
        .kind = ORB_RIM_SCROLL, .minimum = 0, .maximum = maximum,
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
    const about_state_t *state = orb_screen_state(instance);
    *value = (orb_screen_chrome_t){.title = state->page == ORB_SETTINGS_LICENCES_PAGE ? "Licenses" : "About"};
}

static const orb_context_field_t s_context_fields[] = {
    {offsetof(about_context_t, page), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(about_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SETTINGS_ABOUT_SCREEN_ID, .parent_id = ORB_SETTINGS_ROOT_SCREEN_ID, .ops = &s_ops,
    .state_bytes = sizeof(about_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(about_context_t),
    .context_version = 1, .context_fields = s_context_fields,
    .context_field_count = sizeof(s_context_fields) / sizeof(s_context_fields[0]),
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(about_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(about_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE | ORB_DOMAIN_MODEL, .privacy = ORB_PRIVACY_PUBLIC,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_settings_about_descriptor(void)
{
    return &s_descriptor;
}

lv_obj_t *orb_app_settings_about_scroll(const orb_screen_instance_t *instance)
{
    const about_state_t *state = orb_screen_state(instance);
    return state ? state->list : NULL;
}

orb_settings_about_page_t orb_app_settings_about_page(const orb_screen_instance_t *instance)
{
    const about_state_t *state = orb_screen_state(instance);
    return state && state->page == ORB_SETTINGS_LICENCES_PAGE ? ORB_SETTINGS_LICENCES_PAGE : ORB_SETTINGS_ABOUT_PAGE;
}
