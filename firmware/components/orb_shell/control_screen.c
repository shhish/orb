/* HIG §§1–4/7/8: the existing Control panel has matte toggle discs and
 * two glass value selectors. Service and navigation policy stay in the shell. */
#include "control_screen.h"
#include "orb_shell.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_effects.h"
#include "orb_kit_pages.h"
#include "orb_page.h"
#include "orb_input.h"
#include "orb_feedback.h"
#include "orb_ui.h"
#include "orb_home_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    orb_control_model_t model;
    lv_obj_t *level, *brightness, *volume, *battery, *discs[6], *home_action;
    bool initial_update;
} control_state_t;

static void level_format(float value, char *out, size_t capacity, void *context)
{
    (void)context;
    snprintf(out, capacity, "%d%%", (int)lroundf(value));
}

static void level_number(control_state_t *state)
{
    orb_kit_ring_set_formatter(state->level, level_format, state);
    orb_kit_ring_set_value_label(state->level,
        orb_kit_chip_value_label(state->model.control_volume ? state->volume : state->brightness),
        "100%");
}

static const int s_actions[] = {ORB_CONTROL_SILENT, ORB_CONTROL_QUIET, ORB_CONTROL_FLASHLIGHT,
    ORB_CONTROL_SLEEP, ORB_SHELL_VOICE, ORB_SHELL_SETTINGS};

orb_control_model_t orb_control_project(const orb_shell_snapshot_t *status,
    unsigned volume, bool silent, bool quiet, bool flashlight, bool control_volume)
{
    return (orb_control_model_t){.brightness = status ? status->brightness : 0,
        .battery_pct = status ? status->battery_pct : 0, .charging = status && status->charging,
        .volume = volume, .silent = silent, .quiet = quiet, .flashlight = flashlight,
        .control_volume = control_volume};
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
        .value.command = {.target = ORB_SHELL_CONTROL_CENTER,
            .action = (uint32_t)(uintptr_t)lv_obj_get_user_data(target)}};
    /* Acceptance may replace the page. Nothing accesses the sender afterward. */
    (void)orb_screen_emit(instance, &intent);
}

static void detent(lv_obj_t *slider, int32_t value, void *context)
{
    (void)slider;
    orb_screen_instance_t *instance = context;
    control_state_t *state = orb_screen_state(instance);
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = ORB_INPUT_TOUCH,
        .physical = lv_indev_active() && !orb_input_is_synthetic(lv_indev_active()),
        .value.setting = {.key = state->model.control_volume ? ORB_CONTROL_VOLUME : ORB_CONTROL_BRIGHTNESS,
            .value = value}};
    (void)orb_screen_emit(instance, &intent);
}

static lv_obj_t *chip(orb_screen_instance_t *instance, const char *text, int action, int x)
{
    lv_obj_t *obj = orb_kit_button_secondary_create(orb_screen_root(instance), text);
    if (!obj) return NULL;
    lv_obj_update_layout(obj);
    lv_obj_set_pos(obj, x - lv_obj_get_width(obj) / 2, 310 - lv_obj_get_height(obj) / 2);
    lv_obj_set_user_data(obj, (void *)(intptr_t)action);
    lv_obj_set_event_bubble(obj, true);
    lv_obj_add_event_cb(obj, clicked, LV_EVENT_CLICKED, instance);
    return obj;
}

const orb_home_scene_t *orb_control_home_scene(const orb_home_snapshot_t *home, unsigned hour)
{
    const char *name = hour < 12 ? "Good morning" : hour >= 22 ? "Good Night" : hour >= 17 ? "Evening" : "I'm home";
    for (unsigned i = 0; i < home->scene_count; i++)
        if (!strcmp(home->scenes[i].name, name) && !home->scenes[i].contains_unlock)
            return &home->scenes[i];
    return NULL;
}

const orb_home_accessory_t *orb_control_home_favourite(const orb_home_snapshot_t *home, unsigned slot)
{
    for (unsigned i = 0; i < home->accessory_count; i++) {
        const orb_home_accessory_t *a = &home->accessories[i];
        if (a->favourite && a->kind != ORB_HOME_LOCK && (a->writable & ORB_HOME_ON)) {
            if (!slot--) return a;
        }
    }
    return NULL;
}

uint32_t orb_control_home_wait(bool *waiting, uint32_t *started, const orb_home_snapshot_t *home)
{
    bool loading = home && home->online && !home->synced;
    if (loading && !*waiting) *started = lv_tick_get();
    *waiting = loading;
    return *started;
}

void orb_control_home_project(orb_control_model_t *model, const orb_home_snapshot_t *home, unsigned hour)
{
    orb_control_home_model_t *value = &model->home;
    *value = (orb_control_home_model_t){.ready = home && home->online && home->synced && home->available,
        .status = home ? orb_home_status_home(home) : ORB_HOME_STATUS_OFFLINE};
    model->home_panel = true;
    if (!home) return;
    const orb_home_scene_t *scene = value->ready ? orb_control_home_scene(home, hour) : NULL;
    if (scene) { value->scene = *scene; value->has_scene = true; }
    for (unsigned i = 0; i < 2; i++) {
        const orb_home_accessory_t *a = orb_control_home_favourite(home, i);
        if (!a) continue;
        value->present[i] = true;
        value->favourites[i] = *a;
        value->favourite_status[i] = orb_home_status_accessory(home, a, false);
    }
}

static bool home_create(orb_screen_instance_t *instance)
{
    control_state_t *state = orb_screen_state(instance);
    const orb_control_home_model_t *model = &state->model.home;
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_t *title = orb_kit_rim_title_create(root, "Home", ORB_KIT_SECONDARY);
    if (!title) return false;
    orb_screen_page_bind(instance, NULL, title);
    lv_obj_t *status = orb_kit_page_label(root, orb_home_status_text(model->status), ORB_TEXT_FOOTNOTE1, false,
        233, 62, 300, 1, ORB_LABEL_LEGACY_SECONDARY);
    orb_kit_breathe_since(status, model->status == ORB_HOME_STATUS_LOADING, model->wait_started);
    state->home_action = orb_kit_button_secondary_create(root, model->has_scene ? model->scene.name : "Open Home");
    if (!state->home_action) return false;
    lv_obj_update_layout(state->home_action);
    lv_obj_set_pos(state->home_action, 233 - lv_obj_get_width(state->home_action) / 2,
        130 - lv_obj_get_height(state->home_action) / 2);
    lv_obj_set_user_data(state->home_action, (void *)(intptr_t)(model->has_scene ? ORB_CONTROL_HOME_SCENE : ORB_SHELL_HOME));
    lv_obj_set_event_bubble(state->home_action, true);
    lv_obj_add_event_cb(state->home_action, clicked, LV_EVENT_CLICKED, instance);
    for (unsigned i = 0; i < 2; i++) {
        if (!model->present[i]) continue;
        const orb_home_accessory_t *a = &model->favourites[i];
        int x = i ? 313 : 153;
        /* HIG §5: opaque state fill and adaptive ink remain Home content. */
        lv_obj_t *disc = orb_home_identity_disc(root, ORB_KIT_DISC_STANDARD, a, orb_home_disc_colour(a, model->ready));
        if (!disc) return false;
        lv_obj_set_pos(disc, x - ORB_KIT_DISC / 2, 180);
        lv_obj_set_event_bubble(disc, true);
        lv_obj_set_user_data(disc, (void *)(intptr_t)(i ? ORB_CONTROL_HOME_SECOND : ORB_CONTROL_HOME_FIRST));
        lv_obj_add_event_cb(disc, clicked, LV_EVENT_CLICKED, instance);
        if (!a->reachable || !model->ready) lv_obj_add_state(disc, LV_STATE_DISABLED);
        lv_obj_t *name = orb_kit_page_label(root, a->name, ORB_TEXT_FOOTNOTE2, true, x, 270, 140, 2, ORB_LABEL);
        if (!name) return false;
        orb_home_fit_name(name, a->name);
        orb_home_status_t status = model->favourite_status[i];
        orb_kit_page_label(root, status != ORB_HOME_STATUS_READY && model->ready ? orb_home_status_text(status) :
            !model->ready || !(a->state.present & ORB_HOME_ON) ? "—" : a->state.on ? "On" : "Off",
            ORB_TEXT_FOOTNOTE2, false, x, 274 + lv_obj_get_height(name), 140, 1, ORB_LABEL_LEGACY_SECONDARY);
    }
    orb_kit_page_dots_set_active(orb_kit_page_dots_create(root, 2), 1, ORB_LABEL);
    return true;
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    control_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    lv_obj_set_clickable(root, false);
    lv_obj_set_event_bubble(root, true);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context, ORB_SHELL_CONTROL_CENTER, &projection) &&
        projection.model && projection.model->data && projection.model->size == sizeof(state->model))
        memcpy(&state->model, projection.model->data, sizeof(state->model));
    const orb_control_model_t *model = &state->model;
    if (model->home_panel) return home_create(instance);
    state->level = orb_kit_level_create(root, model->control_volume ? ORB_KIT_SOUND : ORB_LABEL,
        0, 100, 1, detent, instance);
    if (!state->level) return false;
    orb_kit_rim_slider_set_value(state->level, model->control_volume ? model->volume : model->brightness, false);
    orb_kit_level_set_value_hidden(state->level, true);
    lv_obj_t *title = orb_kit_rim_title_create(root, "Control Center", ORB_KIT_SECONDARY);
    if (!title) return false;
    orb_screen_page_bind(instance, NULL, title);
    char battery[32]; int pct = model->battery_pct;
    snprintf(battery, sizeof(battery), pct < 0 ? "%s —" : "%s %d%%", model->charging ? ORB_ICON_BATTERY_CHARGING :
        pct >= 0 && pct <= 20 ? ORB_ICON_BATTERY_LOW : pct >= 70 ? ORB_ICON_BATTERY_HIGH : ORB_ICON_BATTERY_MEDIUM, pct);
    state->battery = orb_kit_page_label(root, battery, ORB_TEXT_FOOTNOTE2, true, 233, 56, 150, 1,
        model->charging ? ORB_KIT_PLAY : pct >= 0 && pct <= 20 ? ORB_RED : ORB_LABEL);
    if (!state->battery) return false;
    orb_kit_value_set(state->battery, battery, ORB_ICON_BATTERY_HIGH " 100%");
    const char *icons[] = {model->silent ? ORB_ICON_BELL_SLASH : ORB_ICON_BELL,
        ORB_ICON_MOON, ORB_ICON_FLASHLIGHT, ORB_ICON_LOCK, ORB_ICON_MIC, ORB_ICON_GEAR};
    for (unsigned i = 0; i < 6; i++) {
        bool on = (i == 0 && model->silent) || (i == 1 && model->quiet) || (i == 2 && model->flashlight);
        state->discs[i] = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD,
            on ? ORB_KIT_ACCENT_FILL : ORB_KIT_TRACK,
            !on ? ORB_KIT_GREY : i == 1 ? ORB_KIT_DOT : i == 0 ? ORB_RED : ORB_LABEL, icons[i]);
        if (!state->discs[i]) return false;
        lv_obj_set_pos(state->discs[i], 93 + (i % 3) * 100, 84 + (i / 3) * 100);
        lv_obj_set_user_data(state->discs[i], (void *)(intptr_t)s_actions[i]);
        lv_obj_set_event_bubble(state->discs[i], true);
        lv_obj_add_event_cb(state->discs[i], clicked, LV_EVENT_CLICKED, instance);
    }
    char brightness[48], volume[48];
    snprintf(brightness, sizeof(brightness), ORB_ICON_SUN " %d%%", model->brightness);
    snprintf(volume, sizeof(volume), ORB_ICON_SPEAKER_LOW " %u%%", model->volume);
    state->brightness = chip(instance, brightness, ORB_CONTROL_BRIGHTNESS, 143);
    state->volume = chip(instance, volume, ORB_CONTROL_VOLUME, 323);
    if (!state->brightness || !state->volume) return false;
    /* The original smaller style reserves 100% with a 20px chip gap at AX3. */
    orb_kit_disc_set_text_style(state->brightness, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED);
    orb_kit_disc_set_text_style(state->volume, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED);
    orb_kit_chip_set_value(state->brightness, brightness, ORB_ICON_SUN " 100%");
    orb_kit_chip_set_value(state->volume, volume, ORB_ICON_SPEAKER_LOW " 100%");
    lv_obj_update_layout(state->brightness); lv_obj_update_layout(state->volume);
    lv_obj_set_x(state->brightness, 143 - lv_obj_get_width(state->brightness) / 2);
    lv_obj_set_x(state->volume, 323 - lv_obj_get_width(state->volume) / 2);
    orb_kit_chip_set_selected(state->brightness, !model->control_volume);
    orb_kit_chip_set_selected(state->volume, model->control_volume);
    orb_kit_page_dots_set_active(orb_kit_page_dots_create(root, 2), 0, ORB_LABEL);
    level_number(state);
    state->initial_update = true;
    return true;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *value)
{
    control_state_t *state = orb_screen_state(instance);
    /* Home's identity, labels and availability rebuild together at the shell's
     * original safe point. Router staging must not refit or poll them early. */
    if (state->model.home_panel) return;
    orb_control_model_t model = {0};
    if (value->model && value->model->data && value->model->size == sizeof(model))
        memcpy(&model, value->model->data, sizeof(model));
    bool initial_same = state->initial_update && state->model.brightness == model.brightness &&
        state->model.battery_pct == model.battery_pct && state->model.volume == model.volume &&
        state->model.charging == model.charging && state->model.silent == model.silent &&
        state->model.quiet == model.quiet && state->model.flashlight == model.flashlight &&
        state->model.control_volume == model.control_volume;
    orb_control_model_t before = state->model;
    state->model = model;
    /* Creation and polling deliberately have different battery presentations.
     * Router staging must not perform the first telemetry poll early. */
    state->initial_update = false;
    if (initial_same) return;
    const bool was_on[] = {before.silent, before.quiet, before.flashlight};
    const bool now_on[] = {model.silent, model.quiet, model.flashlight};
    for (unsigned i = 0; i < 3; i++) {
        if (was_on[i] == now_on[i]) continue;
        lv_color_t accent = i == 1 ? ORB_KIT_DOT : i == 0 ? ORB_RED : ORB_LABEL;
        orb_kit_disc_set_ink(state->discs[i], now_on[i] ? orb_kit_accent_ink(accent) : ORB_KIT_GREY);
        if (!i) orb_kit_disc_set_glyph(state->discs[i], model.silent ? ORB_ICON_BELL_SLASH : ORB_ICON_BELL);
        orb_kit_disc_light_up_to(state->discs[i], now_on[i] ? accent : ORB_KIT_GREY,
            now_on[i] ? LV_OPA_COVER : 46);
    }
    char text[48];
    snprintf(text, sizeof(text), ORB_ICON_SUN " %d%%", model.brightness);
    orb_kit_chip_set_value(state->brightness, text, ORB_ICON_SUN " 100%");
    snprintf(text, sizeof(text), ORB_ICON_SPEAKER_LOW " %u%%", model.volume);
    orb_kit_chip_set_value(state->volume, text, ORB_ICON_SPEAKER_LOW " 100%");
    level_number(state);
    orb_kit_level_set_value(state->level, model.control_volume ? model.volume : model.brightness, true);
    snprintf(text, sizeof(text), model.battery_pct < 0 ? "—" : "%d%%", model.battery_pct);
    orb_kit_value_set_rolling(state->battery, text, "100%");
    lv_color_t color = model.battery_pct >= 0 && model.battery_pct <= 20 ? ORB_RED : ORB_KIT_PLAY;
    if (!lv_color_eq(lv_obj_get_style_text_color(state->battery, 0), color))
        lv_obj_set_style_text_color(state->battery, color, 0);
}

lv_obj_t *orb_control_level(orb_screen_instance_t *instance)
{
    control_state_t *state = orb_screen_state(instance);
    return state ? state->level : NULL;
}

void orb_control_value(orb_screen_instance_t *instance, bool volume, int percent)
{
    control_state_t *state = orb_screen_state(instance);
    if (!state) return;
    state->model.control_volume = volume;
    if (volume) state->model.volume = percent;
    else state->model.brightness = percent;
    char text[48];
    snprintf(text, sizeof(text), "%s %d%%", volume ? ORB_ICON_SPEAKER_LOW : ORB_ICON_SUN, percent);
    orb_kit_chip_set_value(volume ? state->volume : state->brightness, text,
        volume ? ORB_ICON_SPEAKER_LOW " 100%" : ORB_ICON_SUN " 100%");
    level_number(state);
    orb_kit_rim_slider_set_value(state->level, percent, false);
}

void orb_control_prepare_launch(orb_screen_instance_t *instance, unsigned action)
{
    control_state_t *state = orb_screen_state(instance);
    if (!state) return;
    if (state->home_action && action == ORB_SHELL_HOME) {
        lv_obj_remove_state(state->home_action, LV_STATE_PRESSED);
        orb_feedback_cancel(state->home_action);
        return;
    }
    for (unsigned i = 0; i < 6; i++) if ((unsigned)s_actions[i] == action) {
        lv_obj_remove_state(state->discs[i], LV_STATE_PRESSED);
        orb_feedback_cancel(state->discs[i]);
        return;
    }
}

static void destroy(orb_screen_instance_t *instance) { (void)instance; }
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
static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{
    (void)instance; (void)origin; (void)physical;
}
static void rim_preview(orb_screen_instance_t *instance, float value)
{
    control_state_t *state = orb_screen_state(instance);
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = ORB_INPUT_CROWN, .physical = true,
        .value.setting = {.key = state->model.control_volume ? ORB_CONTROL_VOLUME : ORB_CONTROL_BRIGHTNESS,
            .value = (int32_t)lrintf(value)}};
    (void)orb_screen_emit(instance, &intent);
}
static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    /* The compatibility binder owns immediate values; lift adds no write. */
    (void)instance; (void)value; (void)commit; (void)origin; (void)physical;
}
static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    control_state_t *state = orb_screen_state(instance);
    if (state->model.home_panel) return false;
    *binding = (orb_rim_binding_t){.property = state->model.control_volume ? ORB_CONTROL_VOLUME : ORB_CONTROL_BRIGHTNESS,
        .kind = ORB_RIM_VALUE, .maximum = 100, .value = state->model.control_volume ? state->model.volume : state->model.brightness,
        .detent = 1, .units_per_turn = 100, .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
    return true;
}
static void visibility(orb_screen_instance_t *instance, orb_visibility_t value) { (void)instance; (void)value; }
static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{
    const control_state_t *state = orb_screen_state(instance);
    *value = (orb_screen_chrome_t){.title = state->model.home_panel ? "Home" : "Control Center"};
}
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SHELL_CONTROL_CENTER, .parent_id = ORB_SHELL_FACE, .ops = &s_ops,
    .state_bytes = sizeof(control_state_t), .context_bytes = sizeof(orb_screen_context_t), .context_version = 1,
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(control_state_t),
        .context_bytes = sizeof(orb_screen_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE | ORB_DOMAIN_TIME, .privacy = ORB_PRIVACY_PRIVATE,
    .always_on = ORB_AOD_HIDDEN, .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};
const orb_screen_descriptor_t *orb_control_descriptor(void) { return &s_descriptor; }

static bool allow_synthetic(const orb_intent_t *intent)
{
    return (intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_SHELL_CONTROL_CENTER) ||
        (intent->kind == ORB_INTENT_SETTING_SET &&
         (intent->value.setting.key == ORB_CONTROL_BRIGHTNESS || intent->value.setting.key == ORB_CONTROL_VOLUME));
}
static void host_rim_finish(orb_screen_instance_t *instance, bool commit) { (void)instance; (void)commit; }

orb_app_host_t *orb_control_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context)
{
    return orb_app_host_create(preview, &(orb_app_host_config_t){.descriptor = orb_control_descriptor,
        .name = "Control Center", .model_bytes = sizeof(orb_control_model_t), .forget_on_detach = true,
        .command = command, .context = context, .allow_synthetic = allow_synthetic, .rim_finish = host_rim_finish});
}

bool orb_control_host_attach(orb_app_host_t *host, lv_obj_t *page, const orb_control_model_t *model)
{
    if (!model || !orb_app_host_set_model(host, model, sizeof(*model)) || !orb_app_host_attach(host, page)) return false;
    /* Preserve the shell's title plane above the moving body. */
    lv_obj_t *title = orb_screen_page_title(orb_app_host_current(host));
    if (title) lv_obj_set_parent(title, page);
    return true;
}

bool orb_control_host_update(orb_app_host_t *host, const orb_control_model_t *model)
{
    if (!model || !orb_app_host_current(host) || !orb_app_host_set_model(host, model, sizeof(*model))) return false;
    orb_app_host_poll(host);
    return true;
}
