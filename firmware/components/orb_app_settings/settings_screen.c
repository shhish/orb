#include "orb_app_settings_root.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_ui.h"
#include "orb_input.h"
#include "orb_page.h"
#include "settings_list_page.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t page;
    int32_t root_scroll, detail_scroll;
} settings_context_t;
typedef struct {
    orb_settings_root_model_t model;
    settings_context_t context;
    lv_obj_t *heading, *list, *choices[3], *done;
} settings_state_t;
typedef enum { SETTINGS_GREY, SETTINGS_BLUE, SETTINGS_PINK, SETTINGS_GREEN } settings_accent_t;
typedef struct {
    const char *title, *icon;
    orb_screen_id_t route;
    uint32_t action;
    settings_accent_t accent;
} settings_item_t;

/* Compatibility route IDs. App code never includes the shell implementation. */
static const settings_item_t s_items[ORB_SETTINGS_ITEM_COUNT] = {
    {"Display", ORB_ICON_SUN, 9, 9, SETTINGS_BLUE},
    {"Sound", ORB_ICON_SPEAKER_LOW, 21, 21, SETTINGS_PINK},
    {"Accessibility", ORB_ICON_ACCESSIBILITY, 10, 10, SETTINGS_BLUE},
    {"Mac", ORB_ICON_DESKTOP, ORB_SCREEN_INVALID_ID, 260, SETTINGS_GREY},
    {"About", ORB_ICON_INFO, 11, 11, SETTINGS_GREY},
    {"Sessions", ORB_ICON_TERMINAL, 17, 17, SETTINGS_GREY},
    {"Updates", ORB_ICON_ARROW_UP_CIRCLE, 15, 15, SETTINGS_BLUE},
    {"Units", ORB_ICON_GLOBE, 19, 19, SETTINGS_GREY},
    {"Control Center", ORB_ICON_SLIDERS, 7, 7, SETTINGS_GREY},
    {"Battery", ORB_ICON_BATTERY_FULL, 29, 29, SETTINGS_GREEN},
    {"Glass", ORB_ICON_EYE, ORB_SETTINGS_ROOT_SCREEN_ID, 462, SETTINGS_GREY},
};
static const char *s_glass_names[] = {"Clear", "Default", "Tinted"};

static lv_color_t item_accent(settings_accent_t accent)
{
    switch (accent) {
    case SETTINGS_BLUE: return ORB_KIT_CODEX;
    case SETTINGS_PINK: return ORB_KIT_SOUND;
    case SETTINGS_GREEN: return ORB_KIT_PLAY;
    default: return ORB_KIT_GREY;
    }
}

static int32_t maximum_scroll(const settings_state_t *state)
{
    lv_obj_update_layout(state->list);
    return LV_MAX(0, lv_obj_get_scroll_y(state->list) + lv_obj_get_scroll_bottom(state->list));
}

static void set_scroll(settings_state_t *state, int32_t value)
{
    value = orb_screen_clamp_scroll(value, maximum_scroll(state));
    if (value == lv_obj_get_scroll_y(state->list)) return;
    if (!orb_round_list_scroll_to(state->list, value))
        lv_obj_scroll_to_y(state->list, value, LV_ANIM_OFF);
}

static void capture_scroll(settings_state_t *state)
{
    if (state->context.page == ORB_SETTINGS_GLASS_PAGE)
        state->context.detail_scroll = lv_obj_get_scroll_y(state->list);
    else state->context.root_scroll = lv_obj_get_scroll_y(state->list);
}

bool orb_app_settings_root_activate(orb_screen_instance_t *instance,
    orb_settings_root_item_t item, orb_input_origin_t origin, bool physical,
    const orb_launch_token_t *launch)
{
    settings_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive || state->context.page != ORB_SETTINGS_ROOT_PAGE ||
        (unsigned)item >= ORB_SETTINGS_ITEM_COUNT ||
        (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON)) return false;
    orb_intent_t intent = {.origin = origin, .physical = physical};
    if (item == ORB_SETTINGS_ITEM_MAC) {
        intent.kind = ORB_INTENT_APP_COMMAND;
        intent.value.command.action = ORB_SETTINGS_COMMAND_CONNECTION;
    } else {
        intent.kind = ORB_INTENT_NAVIGATE;
        intent.value.navigate.screen = s_items[item].route;
        if (item == ORB_SETTINGS_ITEM_GLASS) {
            capture_scroll(state);
            intent.value.navigate.args.page = ORB_SETTINGS_GLASS_PAGE;
        }
        if (launch) intent.value.navigate.launch = *launch;
    }
    return orb_screen_emit(instance, &intent);
}

bool orb_app_settings_glass_select(orb_screen_instance_t *instance,
    uint32_t preference, orb_input_origin_t origin, bool physical)
{
    const settings_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive || state->context.page != ORB_SETTINGS_GLASS_PAGE ||
        preference > 2 || (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON)) return false;
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = origin, .physical = physical};
    intent.value.setting.key = ORB_SETTINGS_SETTING_GLASS;
    intent.value.setting.value = preference;
    return orb_screen_emit(instance, &intent);
}

static void row_clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    const settings_state_t *state = orb_screen_state(instance);
    lv_obj_t *row = lv_event_get_current_target_obj(event);
    if (lv_obj_get_style_opa(row, 0) == LV_OPA_TRANSP) return;
    lv_indev_t *input = lv_indev_active();
    if (input) {
        lv_point_t point;
        lv_indev_get_point(input, &point);
        if (!orb_round_list_hit_test(row, &point)) return;
    }
    uint32_t action = (uintptr_t)lv_obj_get_user_data(row);
    bool physical = input && !orb_input_is_synthetic(input);
    if (state->context.page == ORB_SETTINGS_GLASS_PAGE) {
        if (action >= 1462 && action <= 1464)
            (void)orb_app_settings_glass_select(instance, action - 1462, ORB_INPUT_TOUCH, physical);
        return;
    }
    unsigned item = 0;
    while (item < ORB_SETTINGS_ITEM_COUNT && s_items[item].action != action) item++;
    if (item == ORB_SETTINGS_ITEM_COUNT) return;
    lv_area_t box;
    lv_obj_get_coords(row, &box);
    orb_launch_token_t launch = {.identity = item + 1, .valid = true,
        .rect = {box.x1, box.y1, box.x2 - box.x1 + 1, box.y2 - box.y1 + 1}};
    /* External route clicks retire PRESSED before navigation, as the legacy
     * route branch did. The same-route Glass and Mac actions retain release. */
    if (item != ORB_SETTINGS_ITEM_GLASS && item != ORB_SETTINGS_ITEM_MAC)
        lv_obj_remove_state(row, LV_STATE_PRESSED);
    (void)orb_app_settings_root_activate(instance, item, ORB_INPUT_TOUCH, physical, &launch);
}

static lv_obj_t *row_create(orb_screen_instance_t *instance, unsigned item,
    const char *title, const char *detail, const char *glyph, lv_color_t accent, const char *value)
{
    settings_state_t *state = orb_screen_state(instance);
    lv_obj_t *row = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
        .title = title, .subtitle = detail, .value = value,
        .glyph = glyph && glyph[0] ? glyph : ORB_ICON_INFO, .accent = accent,
    });
    lv_obj_set_user_data(row, (void *)(uintptr_t)(state->context.page == ORB_SETTINGS_GLASS_PAGE ?
        1462 + item : s_items[item].action));
    lv_obj_set_event_bubble(row, true);
    lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, instance);
    orb_kit_row_wrap_title(row, ORB_TEXT_REGULAR);
    return row;
}

static void glass_done(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    const settings_state_t *state = orb_screen_state(instance);
    if (!state->model.interactive) return;
    lv_indev_t *input = lv_indev_active();
    orb_intent_t intent = {.kind = ORB_INTENT_NAVIGATE, .origin = ORB_INPUT_TOUCH,
        .physical = input && !orb_input_is_synthetic(input)};
    intent.value.navigate.screen = ORB_SETTINGS_ROOT_SCREEN_ID;
    (void)orb_screen_emit(instance, &intent);
}

static void build(orb_screen_instance_t *instance)
{
    settings_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_clean(root);
    memset(state->choices, 0, sizeof(state->choices));
    state->done = NULL;
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    state->heading = orb_kit_rim_title_create(root,
        state->context.page == ORB_SETTINGS_GLASS_PAGE ? "Glass" : "Settings", ORB_KIT_SECONDARY);
    /* design_row's former centre/height/rim arguments were unused. Preserve
     * the actual kit-flow list geometry, including its default elastic flag. */
    state->list = orb_kit_list_page(root,
        &(orb_kit_list_page_config_t){.top = 88, .height = 340, .reading = false});
    lv_obj_set_style_pad_top(state->list, ORB_KIT_LIST_INSET, 0);
    lv_obj_t *selected = NULL;
    if (state->context.page == ORB_SETTINGS_GLASS_PAGE) {
        static const char *s_details[] = {"More transparent", "Balanced", "More opaque"};
        lv_obj_set_height(state->list, 252);
        /* HIG §§2/6: an actual Done control previews the applied material,
         * instead of putting decorative glass on a content sample. */
        lv_obj_t *done = orb_kit_button_secondary_create(root, "Done");
        state->done = done;
        lv_obj_update_layout(done);
        lv_obj_set_pos(done, 233 - lv_obj_get_width(done) / 2, 354);
        lv_obj_set_user_data(done, (void *)(uintptr_t)1465);
        lv_obj_add_event_cb(done, glass_done, LV_EVENT_CLICKED, instance);
        orb_kit_control_set_enabled(done, state->model.interactive);
        for (unsigned i = 0; i < 3; i++) {
            lv_obj_t *row = row_create(instance, i, s_glass_names[i], s_details[i],
                ORB_ICON_EYE, ORB_KIT_GREY, i == state->model.glass_preference ? ORB_ICON_CHECK : "");
            state->choices[i] = row;
            orb_kit_row_set_live_value(row, i == state->model.glass_preference ? ORB_ICON_CHECK : "", ORB_ICON_CHECK);
            if (i == state->model.glass_preference) selected = row;
        }
    } else {
        for (unsigned i = 0; i < ORB_SETTINGS_ITEM_COUNT; i++) {
            char name[64];
            snprintf(name, sizeof(name), "%s", s_items[i].title);
            if (i == ORB_SETTINGS_ITEM_ABOUT && state->model.firmware_version[0])
                snprintf(name, sizeof(name), "About · %.31s", state->model.firmware_version);
            const char *detail = i == ORB_SETTINGS_ITEM_MAC ? state->model.hub_name :
                i == ORB_SETTINGS_ITEM_GLASS ? s_glass_names[state->model.glass_preference] : "";
            lv_obj_t *row = row_create(instance, i, name, detail, s_items[i].icon,
                item_accent(s_items[i].accent), i == ORB_SETTINGS_ITEM_SOUND ? "" : NULL);
            if (i == ORB_SETTINGS_ITEM_SOUND)
                orb_kit_row_set_live_value(row, state->model.sound_on ? "On" : "Off", "888 min");
        }
    }
    orb_kit_list_page_finish(state->list, 120);
    if (selected) orb_settings_list_page_focus(state->list, selected);
    lv_obj_move_foreground(state->heading);
    orb_screen_page_bind(instance, state->list, state->heading);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    settings_state_t *state = orb_screen_state(instance);
    state->context.page = args->page == ORB_SETTINGS_GLASS_PAGE ? ORB_SETTINGS_GLASS_PAGE : ORB_SETTINGS_ROOT_PAGE;
    /* Prepare the first immutable value before laying out cached rows. A
     * following router update receives the same projection and stays silent. */
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context,
        ORB_SETTINGS_ROOT_SCREEN_ID, &projection) && projection.model &&
        projection.model->data && projection.model->size == sizeof(state->model))
        memcpy(&state->model, projection.model->data, sizeof(state->model));
    if (state->model.glass_preference > 2) state->model.glass_preference = 1;
    build(instance);
    return state->heading && state->list;
}

static bool model_equal(const orb_settings_root_model_t *a, const orb_settings_root_model_t *b)
{
    return a->glass_preference == b->glass_preference && a->sound_on == b->sound_on &&
        a->interactive == b->interactive && !strcmp(a->hub_name, b->hub_name) &&
        !strcmp(a->firmware_version, b->firmware_version);
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    settings_state_t *state = orb_screen_state(instance);
    orb_settings_root_model_t model = {0};
    if (update->model && update->model->data && update->model->size == sizeof(model))
        memcpy(&model, update->model->data, sizeof(model));
    if (model.glass_preference > 2) model.glass_preference = 1;
    if (model_equal(&state->model, &model)) return;
    if (state->context.page == ORB_SETTINGS_GLASS_PAGE) {
        /* HIG §§1/7: apply a choice where it was tapped. Prepared value lanes
         * change only the checkmark, without rebuilding or moving its rows. */
        state->model = model;
        for (unsigned i = 0; i < 3; i++) {
            orb_kit_row_set_live_value(state->choices[i], i == model.glass_preference ? ORB_ICON_CHECK : "", NULL);
            orb_kit_row_set_enabled(state->choices[i], model.interactive);
        }
        orb_kit_control_set_enabled(state->done, model.interactive);
        return;
    }
    capture_scroll(state);
    state->model = model;
    build(instance);
    int32_t scroll = state->context.page == ORB_SETTINGS_GLASS_PAGE ? state->context.detail_scroll : state->context.root_scroll;
    orb_page_restore(state->list, &(orb_page_context_t){.scroll = scroll});
}

static void destroy(orb_screen_instance_t *instance) { (void)instance; }

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(settings_context_t)) return false;
    const settings_state_t *state = orb_screen_state(instance);
    settings_context_t value = state->context;
    if (value.page == ORB_SETTINGS_GLASS_PAGE) value.detail_scroll = lv_obj_get_scroll_y(state->list);
    else value.root_scroll = lv_obj_get_scroll_y(state->list);
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    settings_state_t *state = orb_screen_state(instance);
    settings_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.page > ORB_SETTINGS_GLASS_PAGE) return false;
    bool changed = state->context.page != value.page;
    state->context = value;
    if (changed) build(instance);
    int32_t scroll = value.page == ORB_SETTINGS_GLASS_PAGE ? value.detail_scroll : value.root_scroll;
    if (scroll) orb_page_restore(state->list, &(orb_page_context_t){.scroll = scroll});
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
    rim_preview(instance, value);
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    settings_state_t *state = orb_screen_state(instance);
    int32_t maximum = maximum_scroll(state);
    if (!maximum) return false;
    *binding = (orb_rim_binding_t){.subpage = state->context.page, .property = 1, .kind = ORB_RIM_SCROLL,
        .minimum = 0, .maximum = maximum,
        .value = orb_screen_clamp_scroll(lv_obj_get_scroll_y(state->list), maximum),
        .detent = 72, .units_per_turn = 900, .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}

static void visibility(orb_screen_instance_t *instance, orb_visibility_t value) { (void)instance; (void)value; }

static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    const settings_state_t *state = orb_screen_state(instance);
    *value = (orb_screen_chrome_t){.title = state->context.page == ORB_SETTINGS_GLASS_PAGE ? "Glass" : "Settings"};
}

static const orb_context_field_t s_context_fields[] = {
    {offsetof(settings_context_t, page), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(settings_context_t, root_scroll), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(settings_context_t, detail_scroll), sizeof(int32_t), ORB_CONTEXT_I32},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SETTINGS_ROOT_SCREEN_ID, .parent_id = ORB_SCREEN_INVALID_ID, .ops = &s_ops,
    .state_bytes = sizeof(settings_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(settings_context_t),
    .context_version = 1, .context_fields = s_context_fields, .context_field_count = 3,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(settings_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(settings_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE | ORB_DOMAIN_SETTINGS, .privacy = ORB_PRIVACY_PUBLIC,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_settings_root_descriptor(void) { return &s_descriptor; }
lv_obj_t *orb_app_settings_root_scroll(const orb_screen_instance_t *instance)
{
    const settings_state_t *state = orb_screen_state(instance);
    return state ? state->list : NULL;
}
orb_settings_root_page_t orb_app_settings_root_page(const orb_screen_instance_t *instance)
{
    const settings_state_t *state = orb_screen_state(instance);
    return state && state->context.page == ORB_SETTINGS_GLASS_PAGE ? ORB_SETTINGS_GLASS_PAGE : ORB_SETTINGS_ROOT_PAGE;
}
uint32_t orb_app_settings_glass_preference(const orb_screen_instance_t *instance)
{
    const settings_state_t *state = orb_screen_state(instance);
    return state ? state->model.glass_preference : 1;
}
