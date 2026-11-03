#include "orb_app_settings_display.h"
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_effects.h"
#include "orb_kit_pages.h"
#include "orb_ui.h"
#include "orb_page.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t page;
    int32_t scroll;
} display_context_t;

typedef struct {
    orb_settings_display_model_t model;
    uint32_t page;
    int32_t level;
    bool projected, arrival;
    lv_obj_t *heading, *list, *level_control, *rows[5];
} display_state_t;

/* Preserve existing developer/parity action metadata; service keys remain
 * the typed public Settings contract. Values were read from the W enum. */
enum {
    DISPLAY_ACTION_BRIGHTNESS = 702,
    DISPLAY_ACTION_ALWAYS_ON = 486,
    DISPLAY_ACTION_ALWAYS_BRIGHTNESS = 487,
    DISPLAY_ACTION_PICKUP = 488,
    DISPLAY_ACTION_QUIET = 484,
};

static int32_t maximum_scroll(display_state_t *state)
{
    if (!state->list) return 0;
    lv_obj_update_layout(state->list);
    return LV_MAX(0, lv_obj_get_scroll_y(state->list) + lv_obj_get_scroll_bottom(state->list));
}

static void set_scroll(display_state_t *state, int32_t requested)
{
    if (!state->list) return;
    int32_t value = orb_screen_clamp_scroll(requested, maximum_scroll(state));
    if (value == lv_obj_get_scroll_y(state->list)) return;
    if (!orb_round_list_scroll_to(state->list, value))
        lv_obj_scroll_to_y(state->list, value, LV_ANIM_OFF);
}

bool orb_app_settings_display_open(orb_screen_instance_t *instance,
    orb_settings_display_page_t page, orb_input_origin_t origin, bool physical,
    const orb_launch_token_t *launch)
{
    display_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive || state->page ||
        page < ORB_SETTINGS_BRIGHTNESS_PAGE || page > ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE) return false;
    orb_intent_t intent = {.kind = ORB_INTENT_NAVIGATE, .origin = origin, .physical = physical};
    intent.value.navigate.screen = ORB_SETTINGS_DISPLAY_SCREEN_ID;
    intent.value.navigate.args.page = page;
    if (launch) intent.value.navigate.launch = *launch;
    return orb_screen_emit(instance, &intent);
}

bool orb_app_settings_display_toggle(orb_screen_instance_t *instance,
    orb_settings_display_key_t key, orb_input_origin_t origin, bool physical)
{
    display_state_t *state = orb_screen_state(instance);
    if (!state || state->page || !state->model.interactive) return false;
    bool value;
    if (key == ORB_SETTING_DISPLAY_ALWAYS_ON || key == ORB_SETTING_DISPLAY_PICKUP) {
        if (!state->model.has_host) return false;
        value = key == ORB_SETTING_DISPLAY_ALWAYS_ON ? !state->model.always_on : !state->model.motion_wake;
    } else if (key == ORB_SETTING_DISPLAY_QUIET) {
        value = !state->model.quiet;
    } else return false;
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = origin, .physical = physical};
    intent.value.setting.key = key;
    intent.value.setting.value = value;
    return orb_screen_emit(instance, &intent);
}

bool orb_app_settings_display_set_level(orb_screen_instance_t *instance,
    int32_t value, orb_input_origin_t origin, bool physical)
{
    display_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive || !state->level_control) return false;
    bool idle = state->page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE;
    value = LV_CLAMP(idle ? 1 : 0, value, idle ? 15 : 100);
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = origin, .physical = physical};
    intent.value.setting.key = idle ? ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS : ORB_SETTING_DISPLAY_BRIGHTNESS;
    intent.value.setting.value = value;
    if (!orb_screen_emit(instance, &intent)) return false;
    state->level = value;
    orb_kit_level_set_value(state->level_control, value, false);
    return true;
}

static void level_changed(lv_obj_t *slider, int32_t value, void *data)
{
    (void)slider;
    lv_indev_t *input = lv_indev_active();
    (void)orb_app_settings_display_set_level(data, value, ORB_INPUT_TOUCH,
        input && !orb_input_is_synthetic(input));
}

static void clicked(lv_event_t *event)
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
    unsigned action = (unsigned)(uintptr_t)lv_obj_get_user_data(row);
    bool physical = input && !orb_input_is_synthetic(input);
    if (action == DISPLAY_ACTION_BRIGHTNESS || action == DISPLAY_ACTION_ALWAYS_BRIGHTNESS) {
        lv_area_t box;
        lv_obj_get_coords(row, &box);
        orb_launch_token_t launch = {.identity = action, .valid = true,
            .rect = {box.x1, box.y1, box.x2 - box.x1 + 1, box.y2 - box.y1 + 1}};
        (void)orb_app_settings_display_open(instance, action == DISPLAY_ACTION_BRIGHTNESS ? ORB_SETTINGS_BRIGHTNESS_PAGE :
            ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE, ORB_INPUT_TOUCH, physical, &launch);
        return;
    }
    orb_settings_display_key_t key;
    if (action == DISPLAY_ACTION_ALWAYS_ON) key = ORB_SETTING_DISPLAY_ALWAYS_ON;
    else if (action == DISPLAY_ACTION_PICKUP) key = ORB_SETTING_DISPLAY_PICKUP;
    else if (action == DISPLAY_ACTION_QUIET) key = ORB_SETTING_DISPLAY_QUIET;
    else return;
    (void)orb_app_settings_display_toggle(instance, key, ORB_INPUT_TOUCH, physical);
}

static void root_values(display_state_t *state, bool first)
{
    char owner[32], idle[32];
    snprintf(owner, sizeof(owner), "%d%%", (int)state->model.brightness);
    snprintf(idle, sizeof(idle), "%u%%", (unsigned)state->model.always_on_brightness);
    orb_kit_row_set_live_text_rolling(state->rows[0], ORB_KIT_ROW_TEXT_SUBTITLE, owner, "100%");
    orb_kit_row_set_live_text_rolling(state->rows[2], ORB_KIT_ROW_TEXT_SUBTITLE, idle, "100%");
    (void)first;
    for (unsigned i = 0; i < 5; i++)
        orb_kit_row_set_enabled(state->rows[i], state->model.interactive &&
            ((i != 1 && i != 3) || state->model.has_host));
    orb_kit_switch_set_checked(orb_kit_row_switch(state->rows[1]), state->model.always_on, false);
    orb_kit_switch_set_checked(orb_kit_row_switch(state->rows[3]), state->model.motion_wake, false);
    orb_kit_switch_set_checked(orb_kit_row_switch(state->rows[4]), state->model.quiet, false);
}

static void build(orb_screen_instance_t *instance)
{
    display_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_clean(root);
    state->heading = state->list = state->level_control = NULL;
    memset(state->rows, 0, sizeof(state->rows));
    if (state->page) {
        bool idle = state->page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE;
        state->level_control = orb_kit_level_create(root, ORB_KIT_SECONDARY,
            idle ? 1 : 0, idle ? 15 : 100, 1, level_changed, instance);
        orb_kit_level_set_title(state->level_control, idle ? "Always On brightness" : "Brightness");
        orb_kit_level_set_icon(state->level_control, idle ? ORB_ICON_MOON : ORB_ICON_SUN);
        orb_kit_level_set_text_style(state->level_control, ORB_TEXT_LARGE_TITLE, ORB_TEXT_EMPHASIZED);
        orb_kit_level_set_value_center(state->level_control, 233, 240);
        state->level = idle ? state->model.always_on_brightness : state->model.brightness;
        if (state->arrival) orb_kit_level_set_arrival(state->level_control, state->level);
        else orb_kit_level_set_value(state->level_control, state->level, false);
        /* The old Display header is deleted for these pages. The kit owns
         * its inset level title; never hoist or enumerate that private title. */
        orb_screen_page_bind(instance, NULL, NULL);
        return;
    }
    state->heading = orb_kit_rim_title_create(root, "Display", ORB_KIT_SECONDARY);
    state->list = orb_kit_list_page(root, &(orb_kit_list_page_config_t){.top = 88, .height = 340});
    static const char *s_titles[] = {
        "Brightness", "Always On", "Always On brightness", "Wake on pickup", "Quiet mode",
    };
    char owner[32], idle[32];
    snprintf(owner, sizeof(owner), "%d%%", (int)state->model.brightness);
    snprintf(idle, sizeof(idle), "%u%%", (unsigned)state->model.always_on_brightness);
    const char *values[] = {owner, state->model.always_on ? "On" : "Off", idle,
        state->model.motion_wake ? "On" : "Off", state->model.quiet ? "Quiet" : "Active"};
    static const unsigned s_actions[] = {
        DISPLAY_ACTION_BRIGHTNESS, DISPLAY_ACTION_ALWAYS_ON,
        DISPLAY_ACTION_ALWAYS_BRIGHTNESS, DISPLAY_ACTION_PICKUP, DISPLAY_ACTION_QUIET,
    };
    static const char *s_glyphs[] = {ORB_ICON_SUN, ORB_ICON_WATCH, ORB_ICON_SUN_DIM, ORB_ICON_HAND, ORB_ICON_MOON};
    bool checked[] = {false, state->model.always_on, false, state->model.motion_wake, state->model.quiet};
    lv_obj_set_style_pad_top(state->list, ORB_KIT_LIST_INSET, 0);
    for (unsigned i = 0; i < 5; i++) {
        bool toggle = i == 1 || i == 3 || i == 4;
        state->rows[i] = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
            .title = s_titles[i], .subtitle = toggle ? "" : values[i],
            .glyph = s_glyphs[i], .accent = ORB_KIT_GREY,
            .show_switch = toggle, .checked = checked[i],
        });
        lv_obj_set_user_data(state->rows[i], (void *)(uintptr_t)s_actions[i]);
        lv_obj_set_event_bubble(state->rows[i], true);
        lv_obj_add_event_cb(state->rows[i], clicked, toggle ? LV_EVENT_VALUE_CHANGED : LV_EVENT_CLICKED, instance);
        orb_kit_row_set_enabled(state->rows[i], state->model.interactive &&
            ((i != 1 && i != 3) || state->model.has_host));
        if (i == 0 || i == 2)
            orb_kit_row_set_live_text(state->rows[i], ORB_KIT_ROW_TEXT_SUBTITLE, values[i], "100%");
        orb_kit_row_wrap_title(state->rows[i], ORB_TEXT_REGULAR);
    }
    orb_kit_list_page_finish(state->list, 120);
    lv_obj_move_foreground(state->heading);
    orb_screen_page_bind(instance, state->list, state->heading);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    display_state_t *state = orb_screen_state(instance);
    state->page = args->page <= ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ? args->page : ORB_SETTINGS_DISPLAY_PAGE;
    /* Preserve the existing black page without adding a trace-visible control. */
    lv_obj_set_clickable(orb_screen_root(instance), false);
    lv_obj_set_event_bubble(orb_screen_root(instance), true);
    /* Prepare the real initial strings/values once. A placeholder first build
     * followed by an equal projection can retain stale fitting and scroll work. */
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context,
        ORB_SETTINGS_DISPLAY_SCREEN_ID, &projection) && projection.model &&
        projection.model->data && projection.model->size == sizeof(state->model)) {
        memcpy(&state->model, projection.model->data, sizeof(state->model));
        state->projected = true;
    }
    state->arrival = true;
    build(instance);
    state->arrival = false;
    return state->page ? state->level_control != NULL : state->heading && state->list;
}

static bool same_model(const orb_settings_display_model_t *a, const orb_settings_display_model_t *b)
{
    return a->brightness == b->brightness && a->always_on_brightness == b->always_on_brightness &&
        a->always_on == b->always_on && a->motion_wake == b->motion_wake &&
        a->quiet == b->quiet && a->has_host == b->has_host &&
        a->interactive == b->interactive && a->level_pending == b->level_pending;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    display_state_t *state = orb_screen_state(instance);
    orb_settings_display_model_t model = {0};
    if (update->model && update->model->data && update->model->size == sizeof(model))
        memcpy(&model, update->model->data, sizeof(model));
    if (same_model(&state->model, &model)) {
        state->projected = true;
        return;
    }
    bool first = !state->projected;
    state->projected = true;
    state->model = model;
    if (!state->page) root_values(state, first);
    else if (!lv_obj_has_state(state->level_control, LV_STATE_PRESSED) && !model.level_pending) {
        state->level = state->page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ?
            model.always_on_brightness : model.brightness;
        orb_kit_level_set_value(state->level_control, state->level, !first);
    }
}

static void destroy(orb_screen_instance_t *instance)
{
    (void)instance;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(display_context_t)) return false;
    const display_state_t *state = orb_screen_state(instance);
    display_context_t value = {.page = state->page,
        .scroll = state->list ? lv_obj_get_scroll_y(state->list) : 0};
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    display_state_t *state = orb_screen_state(instance);
    display_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.page > ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE) return false;
    if (state->page != value.page) {
        state->page = value.page;
        build(instance);
    }
    if (state->list && value.scroll) orb_page_restore(state->list, &(orb_page_context_t){.scroll = value.scroll});
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
    display_state_t *state = orb_screen_state(instance);
    /* Display's sliders currently use direct touch and hardware twist only.
     * Adding a crown lease here would change behavior/cues during extraction. */
    if (state->page) return false;
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
    const display_state_t *state = orb_screen_state(instance);
    *value = (orb_screen_chrome_t){.title = state->page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ?
        "Always On brightness" : state->page ? "Brightness" : "Display"};
}

static const orb_context_field_t s_context_fields[] = {
    {offsetof(display_context_t, page), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(display_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SETTINGS_DISPLAY_SCREEN_ID, .parent_id = 8, .ops = &s_ops,
    .state_bytes = sizeof(display_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(display_context_t),
    .context_version = 1, .context_fields = s_context_fields,
    .context_field_count = sizeof(s_context_fields) / sizeof(s_context_fields[0]),
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(display_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(display_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE | ORB_DOMAIN_SETTINGS, .privacy = ORB_PRIVACY_PUBLIC,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_settings_display_descriptor(void)
{
    return &s_descriptor;
}

lv_obj_t *orb_app_settings_display_scroll(const orb_screen_instance_t *instance)
{
    const display_state_t *state = orb_screen_state(instance);
    return state ? state->list : NULL;
}

lv_obj_t *orb_app_settings_display_level_control(const orb_screen_instance_t *instance)
{
    const display_state_t *state = orb_screen_state(instance);
    return state ? state->level_control : NULL;
}

orb_settings_display_page_t orb_app_settings_display_page(const orb_screen_instance_t *instance)
{
    const display_state_t *state = orb_screen_state(instance);
    return state ? state->page : ORB_SETTINGS_DISPLAY_PAGE;
}

int32_t orb_app_settings_display_level(const orb_screen_instance_t *instance)
{
    const display_state_t *state = orb_screen_state(instance);
    return state && state->level_control ? orb_kit_rim_slider_get_value(state->level_control) : 0;
}
