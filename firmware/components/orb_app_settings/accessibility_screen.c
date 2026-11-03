#include "orb_app_settings_accessibility.h"
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_ui.h"
#include "orb_page.h"
#include "settings_list_page.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t page;
    int32_t scroll;
} accessibility_context_t;

typedef struct {
    orb_settings_accessibility_model_t model;
    uint32_t page;
    lv_obj_t *heading, *list, *rows[5], *detail_toggle;
} accessibility_state_t;

static const char *s_names[] = {
    "Text size", "Bold text", "Reduce motion", "Reduce transparency", "Increase contrast",
};
static const char *s_glyphs[] = {
    ORB_ICON_PENCIL, ORB_ICON_FILE_TEXT, ORB_ICON_PAUSE, ORB_ICON_CIRCLE_FILL, ORB_ICON_EYE,
};
static const char *s_details[] = {
    "Choose your text size", "Make text bolder", "Use simple fades",
    "Use opaque controls", "Increase label contrast",
};

const char *orb_app_settings_accessibility_value(const orb_screen_instance_t *instance, unsigned index)
{
    const accessibility_state_t *state = orb_screen_state(instance);
    if (!state) return "Off";
    switch (index) {
    case 0: return state->model.content_size == ORB_CONTENT_SIZE_DEFAULT ? "Default" :
        state->model.content_size == ORB_CONTENT_SIZE_LARGER ? "Larger" : "Largest";
    case 1: return state->model.bold ? "On" : "Off";
    case 2: return state->model.reduce_motion ? "On" : "Off";
    case 3: return state->model.reduce_transparency ? "On" : "Off";
    default: return state->model.contrast ? "On" : "Off";
    }
}

static int32_t maximum_scroll(accessibility_state_t *state)
{
    if (!state->list) return 0;
    lv_obj_update_layout(state->list);
    return LV_MAX(0, lv_obj_get_scroll_y(state->list) + lv_obj_get_scroll_bottom(state->list));
}

static void set_scroll(accessibility_state_t *state, int32_t requested)
{
    if (!state->list) return;
    int32_t value = orb_screen_clamp_scroll(requested, maximum_scroll(state));
    if (value == lv_obj_get_scroll_y(state->list)) return;
    if (!orb_round_list_scroll_to(state->list, value))
        lv_obj_scroll_to_y(state->list, value, LV_ANIM_OFF);
}

bool orb_app_settings_accessibility_open(orb_screen_instance_t *instance,
    unsigned index, orb_input_origin_t origin, bool physical, const orb_launch_token_t *launch)
{
    accessibility_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive || state->page || index >= 5 ||
        (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON)) return false;
    orb_intent_t intent = {.kind = ORB_INTENT_NAVIGATE, .origin = origin, .physical = physical};
    intent.value.navigate.screen = ORB_SETTINGS_ACCESSIBILITY_SCREEN_ID;
    intent.value.navigate.args.page = index + 1;
    if (launch) intent.value.navigate.launch = *launch;
    return orb_screen_emit(instance, &intent);
}

bool orb_app_settings_accessibility_choose_size(orb_screen_instance_t *instance,
    unsigned size, orb_input_origin_t origin, bool physical)
{
    accessibility_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive || state->page != ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE ||
        size >= ORB_CONTENT_SIZE_COUNT ||
        (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON)) return false;
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = origin, .physical = physical};
    intent.value.setting.key = ORB_SETTING_ACCESS_TEXT_SIZE;
    intent.value.setting.value = size;
    return orb_screen_emit(instance, &intent);
}

static bool toggle_index(orb_screen_instance_t *instance, unsigned index,
    orb_input_origin_t origin, bool physical)
{
    accessibility_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive || index < 1 || index > 4 ||
        (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON)) return false;
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = origin, .physical = physical};
    intent.value.setting.key = ORB_SETTING_ACCESS_TEXT_SIZE + index;
    intent.value.setting.value = strcmp(orb_app_settings_accessibility_value(instance, index), "On") != 0;
    return orb_screen_emit(instance, &intent);
}

bool orb_app_settings_accessibility_toggle(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical)
{
    accessibility_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive || state->page < ORB_SETTINGS_ACCESS_BOLD_PAGE ||
        state->page > ORB_SETTINGS_ACCESS_CONTRAST_PAGE ||
        (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON)) return false;
    return toggle_index(instance, state->page - 1, origin, physical);
}

static void clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    accessibility_state_t *state = orb_screen_state(instance);
    lv_obj_t *target = lv_event_get_current_target_obj(event);
    lv_indev_t *input = lv_indev_active();
    if (state->list && lv_obj_get_parent(target) == state->list) {
        if (lv_obj_get_style_opa(target, 0) == LV_OPA_TRANSP) return;
        if (input) {
            lv_point_t point;
            lv_indev_get_point(input, &point);
            if (!orb_round_list_hit_test(target, &point)) return;
        }
    }
    unsigned action = (unsigned)(uintptr_t)lv_obj_get_user_data(target);
    bool physical = input && !orb_input_is_synthetic(input);
    /* Local entry preserves notice, PRESSED and the outer launch token. */
    if (!state->page && action == 420)
        (void)orb_app_settings_accessibility_open(instance, 0, ORB_INPUT_TOUCH, physical, NULL);
    else if (!state->page && action > 420 && action < 425)
        (void)toggle_index(instance, action - 420, ORB_INPUT_TOUCH, physical);
    else if (state->page == ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE && action >= 430 && action < 433)
        (void)orb_app_settings_accessibility_choose_size(instance, action - 430, ORB_INPUT_TOUCH, physical);
    else if (action == 265)
        (void)orb_app_settings_accessibility_toggle(instance, ORB_INPUT_TOUCH, physical);
}

static lv_obj_t *preference_row(orb_screen_instance_t *instance, unsigned index,
    const char *title, const char *detail, const char *icon)
{
    accessibility_state_t *state = orb_screen_state(instance);
    bool toggle = !state->page && index > 0;
    lv_obj_t *row = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
        .title = title, .subtitle = detail,
        .glyph = icon && icon[0] ? icon : s_glyphs[0], .accent = ORB_KIT_GREY,
        .show_switch = toggle, .checked = toggle && !strcmp(orb_app_settings_accessibility_value(instance, index), "On"),
        .value = state->page == ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE && index == state->model.content_size ? ORB_ICON_CHECK : NULL,
    });
    lv_obj_set_user_data(row, (void *)(uintptr_t)(state->page == ORB_SETTINGS_ACCESSIBILITY_PAGE ?
        420 + index : 430 + index));
    lv_obj_set_event_bubble(row, true);
    lv_obj_add_event_cb(row, clicked, toggle ? LV_EVENT_VALUE_CHANGED : LV_EVENT_CLICKED, instance);
    orb_kit_row_set_enabled(row, state->model.interactive);
    state->rows[index] = row;
    orb_kit_row_wrap_title(row, ORB_TEXT_REGULAR);
    return row;
}

static void detail_label(lv_obj_t *parent, const char *value, lv_color_t color)
{
    /* HIG §§3/4: this is the exact legacy three-line box, including AX3's
     * top/bottom chord clamp and DOTS mode. Extraction does not redesign it. */
    orb_kit_page_label(parent, value, ORB_TEXT_BODY, false, 233, 252, 330, 3, color);
}

static void build(orb_screen_instance_t *instance)
{
    accessibility_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_clean(root);
    state->list = state->detail_toggle = NULL;
    memset(state->rows, 0, sizeof(state->rows));
    orb_settings_list_page_t reading = {0};
    if (state->page <= ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE) {
        orb_settings_list_page_begin(root, state->page ? s_names[state->page - 1] : "Accessibility", &reading);
        state->heading = reading.heading;
        state->list = reading.list;
    } else state->heading = orb_kit_rim_title_create(root, s_names[state->page - 1], ORB_KIT_SECONDARY);
    if (!state->page) {
        for (unsigned i = 0; i < 5; i++)
            preference_row(instance, i, s_names[i],
                i ? "" : orb_app_settings_accessibility_value(instance, i), s_glyphs[i]);
        if (state->model.notice[0])
            orb_settings_list_page_status(root, state->list, state->model.notice);
        orb_settings_list_page_end(&reading);
    } else if (state->page == ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE) {
        static const char *s_sizes[] = {"Default", "Larger", "Largest"};
        /* The sample uses the real selected Body style, so it follows the
         * applied size and Bold Text without temporary global font changes. */
        /* HIG §§3/8: a single short status occupies the sample's safe chord;
         * radio rows retain their applied selection and readable titles. */
        if (state->model.notice[0]) orb_kit_status_caption_create(root, state->model.notice, 104);
        else orb_kit_page_label(root, "Preview text", ORB_TEXT_BODY, false, 233, 104, 330, 1, ORB_LABEL);
        lv_obj_set_y(state->list, 164);
        lv_obj_set_height(state->list, 264);
        lv_obj_t *selected = NULL;
        for (unsigned i = 0; i < ORB_CONTENT_SIZE_COUNT; i++) {
            lv_obj_t *row = preference_row(instance, i, s_sizes[i], "", s_glyphs[0]);
            if (i == state->model.content_size) selected = row;
        }
        orb_settings_list_page_end(&reading);
        orb_settings_list_page_focus(state->list, selected);
    } else {
        if (state->model.notice[0]) orb_kit_status_caption_create(root, state->model.notice, 210);
        else detail_label(root, s_details[state->page - 1], ORB_LABEL_LEGACY_SECONDARY);
        const char *value = orb_app_settings_accessibility_value(instance, state->page - 1);
        lv_obj_t *toggle = orb_kit_switch_create(root, !strcmp(value, "On"), ORB_KIT_PLAY);
        state->detail_toggle = toggle;
        lv_obj_set_pos(toggle, 193, 326);
        lv_obj_set_user_data(toggle, (void *)(uintptr_t)265);
        lv_obj_add_event_cb(toggle, clicked, LV_EVENT_VALUE_CHANGED, instance);
        orb_kit_control_set_enabled(toggle, state->model.interactive);

    }
    lv_obj_move_foreground(state->heading);
    orb_screen_page_bind(instance, state->list, state->heading);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    accessibility_state_t *state = orb_screen_state(instance);
    state->page = args->page <= ORB_SETTINGS_ACCESS_CONTRAST_PAGE ? args->page : ORB_SETTINGS_ACCESSIBILITY_PAGE;
    lv_obj_set_clickable(orb_screen_root(instance), false);
    lv_obj_set_event_bubble(orb_screen_root(instance), true);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context,
        ORB_SETTINGS_ACCESSIBILITY_SCREEN_ID, &projection) && projection.model &&
        projection.model->data && projection.model->size == sizeof(state->model))
        memcpy(&state->model, projection.model->data, sizeof(state->model));
    build(instance);
    return state->heading && (state->page > ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE || state->list);
}

static bool presentation_changed(const orb_settings_accessibility_model_t *a,
    const orb_settings_accessibility_model_t *b)
{
    return a->content_size != b->content_size || a->bold != b->bold ||
        a->reduce_motion != b->reduce_motion || a->reduce_transparency != b->reduce_transparency ||
        a->contrast != b->contrast || strcmp(a->notice, b->notice);
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    accessibility_state_t *state = orb_screen_state(instance);
    orb_settings_accessibility_model_t model = {0};
    if (update->model && update->model->data && update->model->size == sizeof(model))
        memcpy(&model, update->model->data, sizeof(model));
    bool rebuild = presentation_changed(&state->model, &model);
    if (!rebuild) {
        /* HIG §8: a native switch moves before its intent completes. An equal
         * projection still restores the applied truth during a font request
         * or after rejection; a later successful projection moves it on. */
        for (unsigned i = 1; i < 5; i++) {
            lv_obj_t *toggle = state->rows[i] ? orb_kit_row_switch(state->rows[i]) : NULL;
            if (toggle) orb_kit_switch_set_checked(toggle,
                !strcmp(orb_app_settings_accessibility_value(instance, i), "On"), true);
        }
        if (state->detail_toggle) orb_kit_switch_set_checked(state->detail_toggle,
            !strcmp(orb_app_settings_accessibility_value(instance, state->page - 1), "On"), true);
    }
    if (!rebuild && state->model.interactive == model.interactive) return;
    int32_t scroll = state->list ? lv_obj_get_scroll_y(state->list) : 0;
    bool selection_changed = state->model.content_size != model.content_size;
    state->model = model;
    if (rebuild) {
        build(instance);
        /* HIG §§1/7/8: font changes may reflow, but a choice keeps its place.
         * Reveal it only if the new line boxes no longer fit the viewport. */
        orb_page_restore(state->list, &(orb_page_context_t){.scroll = scroll});
        if (selection_changed && state->page == ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE)
            orb_settings_list_page_reveal(state->list, state->rows[model.content_size]);
    } else {
        for (unsigned i = 0; i < 5; i++)
            if (state->rows[i]) orb_kit_row_set_enabled(state->rows[i], model.interactive);
        if (state->detail_toggle) orb_kit_control_set_enabled(state->detail_toggle, model.interactive);
    }
}

static void destroy(orb_screen_instance_t *instance)
{
    (void)instance;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(accessibility_context_t)) return false;
    const accessibility_state_t *state = orb_screen_state(instance);
    accessibility_context_t value = {.page = state->page,
        .scroll = state->list ? lv_obj_get_scroll_y(state->list) : 0};
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    accessibility_state_t *state = orb_screen_state(instance);
    accessibility_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.page > ORB_SETTINGS_ACCESS_CONTRAST_PAGE) return false;
    if (state->page != value.page) {
        state->page = value.page;
        build(instance);
    }
    if (value.scroll || (state->list && lv_obj_get_scroll_y(state->list)))
        orb_page_restore(state->list, &(orb_page_context_t){.scroll = value.scroll});
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
    accessibility_state_t *state = orb_screen_state(instance);
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
    const accessibility_state_t *state = orb_screen_state(instance);
    *value = (orb_screen_chrome_t){.title = state->page ? s_names[state->page - 1] : "Accessibility"};
}

static const orb_context_field_t s_context_fields[] = {
    {offsetof(accessibility_context_t, page), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(accessibility_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SETTINGS_ACCESSIBILITY_SCREEN_ID, .parent_id = 8, .ops = &s_ops,
    .state_bytes = sizeof(accessibility_state_t),
    .context_bytes = sizeof(orb_screen_context_t) + sizeof(accessibility_context_t),
    .context_version = 1, .context_fields = s_context_fields,
    .context_field_count = sizeof(s_context_fields) / sizeof(s_context_fields[0]),
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(accessibility_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(accessibility_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_SETTINGS, .privacy = ORB_PRIVACY_PUBLIC,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_settings_accessibility_descriptor(void)
{
    return &s_descriptor;
}

lv_obj_t *orb_app_settings_accessibility_scroll(const orb_screen_instance_t *instance)
{
    const accessibility_state_t *state = orb_screen_state(instance);
    return state ? state->list : NULL;
}

orb_settings_accessibility_page_t orb_app_settings_accessibility_page(const orb_screen_instance_t *instance)
{
    const accessibility_state_t *state = orb_screen_state(instance);
    return state ? state->page : ORB_SETTINGS_ACCESSIBILITY_PAGE;
}
