#include "orb_app_settings_sources.h"
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_effects.h"
#include "orb_kit_pages.h"
#include "orb_page.h"
#include "orb_ui.h"
#include "settings_list_page.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int32_t scroll, changed_row;
    uint32_t wait_started;
    uint64_t observed_operation;
} sources_context_t;

typedef struct {
    orb_settings_sources_model_t model;
    int32_t changed_row;
    uint64_t observed_operation;
    uint32_t wait_started;
    lv_obj_t *heading, *list, *rows[ORB_SETTINGS_SOURCE_COUNT];
} sources_state_t;

/* Retain developer/harness action metadata; typed intent keys are separate. */
enum { SOURCE_ROW_BASE = 273, RECENT_ROW = 276 };

static void build(orb_screen_instance_t *instance);

static bool reconcile_operation(sources_state_t *state)
{
    if (state->model.hub_pref_operation <= state->observed_operation) return false;
    state->observed_operation = state->model.hub_pref_operation;
    int32_t previous = state->changed_row;
    uint32_t key = state->model.hub_pref_key;
    /* The old Temperature action reset shared row attribution even while
     * Sources was hidden. Retain that UI operation without retaining commands. */
    if (key == 1) state->changed_row = -1;
    else if (key >= ORB_SETTING_SOURCE_CLAUDE && key <= ORB_SETTING_SOURCE_DOT)
        state->changed_row = (int32_t)(key - ORB_SETTING_SOURCE_CLAUDE);
    else if (key == ORB_SETTING_SOURCE_RECENT_MINUTES) state->changed_row = ORB_SETTINGS_SOURCE_COUNT;
    return state->changed_row != previous;
}

bool orb_app_settings_sources_requests_enabled(const orb_screen_instance_t *instance)
{
    const sources_state_t *state = orb_screen_state(instance);
    return state && state->model.ready && !state->model.pending;
}

static bool user_origin(orb_input_origin_t origin)
{
    return origin == ORB_INPUT_TOUCH || origin == ORB_INPUT_BUTTON;
}

static bool emit_setting(orb_screen_instance_t *instance, orb_settings_source_key_t key,
    int32_t value, orb_input_origin_t origin, bool physical)
{
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = origin, .physical = physical};
    intent.value.setting.key = key;
    intent.value.setting.value = value;
    return orb_screen_emit(instance, &intent);
}

bool orb_app_settings_sources_toggle(orb_screen_instance_t *instance, unsigned source,
    orb_input_origin_t origin, bool physical)
{
    sources_state_t *state = orb_screen_state(instance);
    if (!state || source >= ORB_SETTINGS_SOURCE_COUNT || !state->model.interactive ||
        !orb_app_settings_sources_requests_enabled(instance) || !user_origin(origin) ||
        orb_screen_visibility_get(instance) != ORB_VISIBILITY_ACTIVE) return false;
    int32_t previous = state->changed_row;
    state->changed_row = source;
    bool accepted = emit_setting(instance, ORB_SETTING_SOURCE_CLAUDE + source,
        !state->model.sources[source], origin, physical);
    if (!accepted) state->changed_row = previous;
    return accepted;
}

bool orb_app_settings_sources_cycle_recent(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical)
{
    sources_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive || !orb_app_settings_sources_requests_enabled(instance) ||
        !user_origin(origin) || orb_screen_visibility_get(instance) != ORB_VISIBILITY_ACTIVE) return false;
    int32_t previous = state->changed_row;
    state->changed_row = ORB_SETTINGS_SOURCE_COUNT;
    uint32_t current = state->model.idle_minutes;
    uint32_t next = current == 0 ? 5 : current == 5 ? 10 : current == 10 ? 30 : 0;
    bool accepted = emit_setting(instance, ORB_SETTING_SOURCE_RECENT_MINUTES, next, origin, physical);
    if (!accepted) state->changed_row = previous;
    return accepted;
}

static void clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    sources_state_t *state = orb_screen_state(instance);
    lv_obj_t *target = lv_event_get_target_obj(event);
    lv_indev_t *input = lv_indev_active();
    if (state->list && lv_obj_get_parent(target) == state->list) {
        if (lv_obj_get_style_opa(target, 0) == LV_OPA_TRANSP) return;
        if (input) {
            lv_point_t point;
            lv_indev_get_point(input, &point);
            /* Preserve the current target-based check even for the floating
             * switch. Normalizing it to its row would be a behavior change. */
            if (!orb_round_list_hit_test(target, &point)) return;
        }
    }
    unsigned action = (unsigned)(uintptr_t)lv_obj_get_user_data(target);
    bool physical = input && !orb_input_is_synthetic(input);
    if (action >= SOURCE_ROW_BASE && action < SOURCE_ROW_BASE + ORB_SETTINGS_SOURCE_COUNT)
        (void)orb_app_settings_sources_toggle(instance, action - SOURCE_ROW_BASE, ORB_INPUT_TOUCH, physical);
    else if (action == RECENT_ROW)
        (void)orb_app_settings_sources_cycle_recent(instance, ORB_INPUT_TOUCH, physical);
}

static void list_edge(lv_event_t *event)
{
    lv_obj_t *list = lv_event_get_current_target_obj(event);
    if (!ORB_UI_SOFT_EDGES)
        orb_scroll_edge_apply(list, lv_obj_get_scroll_y(list) > 0 ? 16 : 0, 24);
}

static lv_obj_t *preference_row(orb_screen_instance_t *instance, const char *title,
    const char *detail, const char *glyph, unsigned action)
{
    sources_state_t *state = orb_screen_state(instance);
    lv_obj_t *row = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
        .title = title, .subtitle = detail,
        .glyph = glyph && glyph[0] ? glyph : ORB_ICON_INFO, .accent = ORB_KIT_GREY,
    });
    lv_obj_set_user_data(row, (void *)(uintptr_t)action);
    lv_obj_set_event_bubble(row, true);
    if (action) lv_obj_add_event_cb(row, clicked, LV_EVENT_CLICKED, instance);
    else lv_obj_set_clickable(row, false);
    orb_kit_row_wrap_title(row, ORB_TEXT_REGULAR);
    return row;
}

static const char *feedback(const sources_state_t *state)
{
    return state->model.notice[0] ? state->model.notice : state->model.pending ? "Saving…" :
        state->model.error[0] ? ORB_ICON_WARNING " Not saved" : NULL;
}

static void source_rows(orb_screen_instance_t *instance)
{
    sources_state_t *state = orb_screen_state(instance);
    static const char *s_names[] = {"Claude", "Codex", "Dot"};
    static const char *s_glyphs[] = {ORB_ICON_PROMPT, ORB_ICON_CODE, ORB_ICON_CHAT};
    lv_color_t accents[] = {ORB_KIT_CLAUDE, ORB_KIT_CODEX, ORB_KIT_DOT};
    const char *status = feedback(state);
    /* Offline before the hub confirmed anything, the defaults are not facts:
     * say Unknown and show no switch, rather than a dimmed "Off" (review #12). */
    bool known = state->model.known || state->model.ready;
    for (unsigned i = 0; i < ORB_SETTINGS_SOURCE_COUNT; i++) {
        bool on = state->model.sources[i];
        char detail[96];
        /* The switch already says On/Off; the focused row gives save failures
         * the whole subtitle lane instead of truncating them after that state. */
        snprintf(detail, sizeof(detail), "%s", status && state->changed_row == (int)i ? status :
            !known ? "Unknown" : "");
        lv_obj_t *row = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
            .title = s_names[i], .subtitle = detail,
            .glyph = s_glyphs[i], .accent = accents[i], .show_switch = known, .checked = on,
        });
        state->rows[i] = row;
        orb_kit_row_set_waiting_since(row, ORB_KIT_ROW_TEXT_SUBTITLE,
            state->model.pending && state->changed_row == (int)i, state->wait_started);
        lv_obj_set_user_data(row, (void *)(uintptr_t)(SOURCE_ROW_BASE + i));
        lv_obj_add_event_cb(row, clicked, LV_EVENT_CLICKED, instance);
        lv_obj_t *toggle = orb_kit_row_switch(row);
        if (toggle) {
            lv_obj_set_user_data(toggle, (void *)(uintptr_t)(SOURCE_ROW_BASE + i));
            lv_obj_add_event_cb(toggle, clicked, LV_EVENT_CLICKED, instance);
        }
        orb_kit_row_set_enabled(row, !state->model.pending && state->model.ready);
    }
}

static void build(orb_screen_instance_t *instance)
{
    sources_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_clean(root);
    state->heading = orb_kit_rim_title_create(root, "Sessions", ORB_KIT_SECONDARY);
    lv_obj_update_layout(root);
    state->list = orb_kit_list_page(root,
        &(orb_kit_list_page_config_t){.top = 88, .height = 360, .pad_bottom = 24});
    lv_obj_set_scroll_elastic(state->list, !orb_ui_reduce_motion());
    if (ORB_UI_SOFT_EDGES) orb_scroll_edge_follow(state->list, 32, 48);
    else {
        orb_scroll_edge_apply(state->list, 0, 24);
        lv_obj_add_event_cb(state->list, list_edge, LV_EVENT_SCROLL, NULL);
    }
    lv_obj_set_style_pad_bottom(state->list, 200, 0);
    if (!state->model.ready)
        orb_settings_list_page_status(root, state->list, state->model.known ? ORB_KIT_OFFLINE_CAPTION : "Mac offline");
    lv_obj_set_style_pad_top(state->list, ORB_KIT_LIST_INSET, 0);
    source_rows(instance);
    char value[96];
    const char *status = feedback(state);
    if (status && state->changed_row == ORB_SETTINGS_SOURCE_COUNT) snprintf(value, sizeof(value), "%s", status);
    else if (!state->model.known && !state->model.ready) snprintf(value, sizeof(value), "Unknown");
    else if (!state->model.idle_minutes) snprintf(value, sizeof(value), "Off");
    else snprintf(value, sizeof(value), "%u min", (unsigned)state->model.idle_minutes);
    lv_obj_t *recent = preference_row(instance,
        "Recent activity", value, ORB_ICON_CLOCK, RECENT_ROW);
    orb_kit_row_set_waiting_since(recent, ORB_KIT_ROW_TEXT_SUBTITLE,
        state->model.pending && state->changed_row == ORB_SETTINGS_SOURCE_COUNT, state->wait_started);
    orb_kit_row_set_enabled(recent, !state->model.pending && state->model.ready);
    /* HIG §§1/7: this explanation is ordinary text, not a dead settings row. */
    lv_obj_update_layout(recent);
    int32_t footnote_y = lv_obj_get_y(recent) + lv_obj_get_height(recent) + 20;
    /* The round compositor captures each direct child's descendants; keep
     * this label in a transparent, noninteractive content group. */
    lv_obj_t *note = lv_obj_create(state->list);
    lv_obj_remove_style_all(note);
    lv_obj_set_scrollable(note, false);
    lv_obj_set_clickable(note, false);
    lv_obj_set_event_bubble(note, true);
    lv_obj_set_width(note, 418);
    lv_obj_set_pos(note, 24, footnote_y);
    lv_obj_t *footnote = lv_label_create(note);
    orb_text_apply(footnote, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(footnote, "Includes recently changed sessions");
    lv_obj_set_style_text_color(footnote, ORB_KIT_SECONDARY, 0);
    lv_obj_set_style_text_opa(footnote, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_set_style_text_align(footnote, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(footnote, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(footnote, 314);
    lv_obj_set_pos(footnote, 52, 0);
    lv_obj_set_clickable(footnote, false);
    lv_obj_update_layout(footnote);
    lv_obj_set_height(note, lv_obj_get_height(footnote));
    orb_kit_list_page_finish(state->list, 200);
    if (state->model.error[0] && state->changed_row >= 0 && state->changed_row < ORB_SETTINGS_SOURCE_COUNT)
        orb_settings_list_page_focus(state->list, state->rows[state->changed_row]);
    lv_obj_move_foreground(state->heading);
    orb_screen_page_bind(instance, state->list, state->heading);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    sources_state_t *state = orb_screen_state(instance);
    state->changed_row = -1;
    lv_obj_set_clickable(orb_screen_root(instance), false);
    lv_obj_set_event_bubble(orb_screen_root(instance), true);
    /* The initial public projection must precede row preparation. An empty
     * placeholder rebuild can otherwise leave stale fitting/scroll events. */
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context,
        ORB_SETTINGS_SOURCES_SCREEN_ID, &projection) && projection.model &&
        projection.model->data && projection.model->size == sizeof(state->model))
        memcpy(&state->model, projection.model->data, sizeof(state->model));
    if (state->model.pending) state->wait_started = lv_tick_get();
    (void)reconcile_operation(state);
    build(instance);
    return state->heading && state->list;
}

static bool same_content(const orb_settings_sources_model_t *a, const orb_settings_sources_model_t *b)
{
    return !memcmp(a->sources, b->sources, sizeof(a->sources)) && a->idle_minutes == b->idle_minutes &&
        a->pending == b->pending && a->ready == b->ready && a->known == b->known && !strcmp(a->error, b->error) &&
        !strcmp(a->notice, b->notice) && a->render_epoch == b->render_epoch;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    sources_state_t *state = orb_screen_state(instance);
    orb_settings_sources_model_t model = {0};
    if (update->model && update->model->data && update->model->size == sizeof(model))
        memcpy(&model, update->model->data, sizeof(model));
    bool rebuild = !same_content(&state->model, &model);
    if (model.pending && !state->model.pending) state->wait_started = lv_tick_get();
    state->model = model;
    rebuild |= reconcile_operation(state);
    if (!rebuild) return;
    int32_t scroll = lv_obj_get_scroll_y(state->list);
    build(instance);
    if (scroll && !state->model.error[0]) {
        lv_obj_scroll_to_y(state->list, scroll, LV_ANIM_OFF);
        orb_round_list_update(state->list);
    }
}

static void destroy(orb_screen_instance_t *instance)
{
    (void)instance;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(sources_context_t)) return false;
    const sources_state_t *state = orb_screen_state(instance);
    /* Zero padding too: context validation rejects any nonzero hole byte. */
    sources_context_t value;
    memset(&value, 0, sizeof(value));
    value.scroll = lv_obj_get_scroll_y(state->list);
    value.changed_row = state->changed_row;
    value.wait_started = state->wait_started;
    value.observed_operation = state->observed_operation;
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    sources_state_t *state = orb_screen_state(instance);
    sources_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.changed_row < -1 || value.changed_row > ORB_SETTINGS_SOURCE_COUNT) return false;
    int32_t previous = state->changed_row;
    bool wait_changed = state->model.pending && value.wait_started && state->wait_started != value.wait_started;
    if (wait_changed) state->wait_started = value.wait_started;
    state->changed_row = value.changed_row;
    state->observed_operation = value.observed_operation;
    /* The current projection can carry an accepted Temperature/source action
     * newer than this saved Sources context. Apply it before choosing feedback. */
    (void)reconcile_operation(state);
    if (state->changed_row != previous || wait_changed) build(instance);
    if (value.scroll || lv_obj_get_scroll_y(state->list))
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
    sources_state_t *state = orb_screen_state(instance);
    int32_t position = (int32_t)lrintf(value);
    if (!orb_round_list_scroll_to(state->list, position))
        lv_obj_scroll_to_y(state->list, position, LV_ANIM_OFF);
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    (void)commit; (void)origin; (void)physical;
    rim_preview(instance, value);
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    sources_state_t *state = orb_screen_state(instance);
    lv_obj_update_layout(state->list);
    int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(state->list) + lv_obj_get_scroll_bottom(state->list));
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
    *value = (orb_screen_chrome_t){.title = "Sessions"};
}

static const orb_context_field_t s_context_fields[] = {
    {offsetof(sources_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(sources_context_t, changed_row), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(sources_context_t, observed_operation), sizeof(uint64_t), ORB_CONTEXT_U64},
    {offsetof(sources_context_t, wait_started), sizeof(uint32_t), ORB_CONTEXT_U32},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SETTINGS_SOURCES_SCREEN_ID, .parent_id = 8, .ops = &s_ops,
    .state_bytes = sizeof(sources_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(sources_context_t),
    .context_version = 2, .context_fields = s_context_fields,
    .context_field_count = sizeof(s_context_fields) / sizeof(s_context_fields[0]),
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(sources_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(sources_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_MODEL | ORB_DOMAIN_DEVICE,
    .privacy = ORB_PRIVACY_PUBLIC, .always_on = ORB_AOD_HIDDEN,
    .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_settings_sources_descriptor(void)
{
    return &s_descriptor;
}

lv_obj_t *orb_app_settings_sources_scroll(const orb_screen_instance_t *instance)
{
    const sources_state_t *state = orb_screen_state(instance);
    return state ? state->list : NULL;
}

int orb_app_settings_sources_changed(const orb_screen_instance_t *instance)
{
    const sources_state_t *state = orb_screen_state(instance);
    return state ? state->changed_row : -1;
}

bool orb_app_settings_sources_switch_checked(const orb_screen_instance_t *instance, unsigned source)
{
    const sources_state_t *state = orb_screen_state(instance);
    return state && source < ORB_SETTINGS_SOURCE_COUNT &&
        orb_kit_switch_get_checked(orb_kit_row_switch(state->rows[source]));
}

bool orb_app_settings_sources_action_bounds(const orb_screen_instance_t *instance,
    unsigned source, bool floating_switch, lv_area_t *bounds)
{
    const sources_state_t *state = orb_screen_state(instance);
    if (!state || source >= ORB_SETTINGS_SOURCE_COUNT || !bounds) return false;
    lv_obj_update_layout(orb_screen_root(instance));
    if (!floating_switch) return orb_round_list_row_bounds(state->rows[source], bounds);
    lv_obj_t *toggle = orb_kit_row_switch(state->rows[source]);
    if (!toggle || !lv_obj_is_visible(toggle)) return false;
    lv_obj_get_coords(toggle, bounds);
    return true;
}
