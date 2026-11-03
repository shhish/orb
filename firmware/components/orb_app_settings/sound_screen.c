#include "orb_app_settings_sound.h"
#include "orb_fonts.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_effects.h"
#include "orb_kit_pages.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "orb_page.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t page, changed, wait_started;
    int32_t scroll[3];
} sound_context_t;

typedef struct {
    orb_settings_sound_model_t model;
    uint32_t page, changed, save_state, wait_started;
    int32_t scroll[3], volume, hour, rim_start;
    bool projected, render_requested, arrival;
    lv_obj_t *heading, *list, *level, *volume_row, *hour_value;
} sound_state_t;

enum {
    OPEN_VOLUME = 450, OPEN_QUIET = 451, TOGGLE_QUIET = 452,
    OPEN_START = 453, OPEN_END = 454, HOUR_DOWN = 455, HOUR_UP = 456,
    OPEN_PALETTE = 461, TOGGLE_SPEECH = 460, TOGGLE_SILENT = 263,
    OPEN_DISPLAY = 9, PREVIEW_BASE = 600,
};

static void build(orb_screen_instance_t *instance);

static bool hour_page(const sound_state_t *state)
{
    return state->page == ORB_SETTINGS_SOUND_START_PAGE || state->page == ORB_SETTINGS_SOUND_END_PAGE;
}

static int scroll_slot(uint32_t page)
{
    return page == ORB_SETTINGS_SOUND_PAGE ? 0 : page == ORB_SETTINGS_SOUND_QUIET_PAGE ? 1 :
        page == ORB_SETTINGS_SOUND_PALETTE_PAGE ? 2 : -1;
}

static void capture_scroll(sound_state_t *state)
{
    int slot = scroll_slot(state->page);
    if (slot >= 0 && state->list) state->scroll[slot] = lv_obj_get_scroll_y(state->list);
}

static int32_t maximum_scroll(sound_state_t *state)
{
    if (!state->list) return 0;
    lv_obj_update_layout(state->list);
    return LV_MAX(0, lv_obj_get_scroll_y(state->list) + lv_obj_get_scroll_bottom(state->list));
}

static void set_scroll(sound_state_t *state, int32_t value)
{
    if (!state->list) return;
    value = orb_screen_clamp_scroll(value, maximum_scroll(state));
    if (value == lv_obj_get_scroll_y(state->list)) return;
    if (!orb_round_list_scroll_to(state->list, value))
        lv_obj_scroll_to_y(state->list, value, LV_ANIM_OFF);
}

static bool emit_setting(orb_screen_instance_t *instance, orb_settings_sound_key_t key,
    int32_t value, orb_input_origin_t origin, bool physical)
{
    orb_intent_t intent = {.kind = ORB_INTENT_SETTING_SET, .origin = origin, .physical = physical};
    intent.value.setting.key = key;
    intent.value.setting.value = value;
    return orb_screen_emit(instance, &intent);
}

bool orb_app_settings_sound_open(orb_screen_instance_t *instance,
    orb_settings_sound_page_t page, orb_input_origin_t origin, bool physical,
    const orb_launch_token_t *launch)
{
    sound_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive) return false;
    bool allowed = state->page == ORB_SETTINGS_SOUND_PAGE ?
        page == ORB_SETTINGS_SOUND_VOLUME_PAGE || page == ORB_SETTINGS_SOUND_QUIET_PAGE ||
        page == ORB_SETTINGS_SOUND_PALETTE_PAGE : state->page == ORB_SETTINGS_SOUND_QUIET_PAGE &&
        (page == ORB_SETTINGS_SOUND_START_PAGE || page == ORB_SETTINGS_SOUND_END_PAGE);
    if (!allowed) return false;
    capture_scroll(state);
    /* Explicit local entry started these child readers at zero; outer return
     * still restores their saved page through its opaque group context. */
    if (state->page == ORB_SETTINGS_SOUND_PAGE && page == ORB_SETTINGS_SOUND_QUIET_PAGE) state->scroll[1] = 0;
    if (page == ORB_SETTINGS_SOUND_PALETTE_PAGE) state->scroll[2] = 0;
    orb_intent_t intent = {.kind = ORB_INTENT_NAVIGATE, .origin = origin, .physical = physical};
    intent.value.navigate.screen = ORB_SETTINGS_SOUND_SCREEN_ID;
    intent.value.navigate.args.page = page;
    if (launch) intent.value.navigate.launch = *launch;
    return orb_screen_emit(instance, &intent);
}

bool orb_app_settings_sound_open_display(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical, const orb_launch_token_t *launch)
{
    sound_state_t *state = orb_screen_state(instance);
    if (!state || state->page != ORB_SETTINGS_SOUND_PAGE || !state->model.interactive) return false;
    capture_scroll(state);
    orb_intent_t intent = {.kind = ORB_INTENT_NAVIGATE, .origin = origin, .physical = physical};
    intent.value.navigate.screen = 9;
    if (launch) intent.value.navigate.launch = *launch;
    return orb_screen_emit(instance, &intent);
}

bool orb_app_settings_sound_toggle(orb_screen_instance_t *instance,
    orb_settings_sound_key_t key, orb_input_origin_t origin, bool physical)
{
    sound_state_t *state = orb_screen_state(instance);
    if (!state || !state->model.interactive) return false;
    if (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON) return false;
    uint32_t previous = state->changed;
    bool value;
    if (state->page == ORB_SETTINGS_SOUND_PAGE && key == ORB_SETTING_SOUND_SPEECH)
        value = !state->model.prefs.spoken_replies;
    else if (state->page == ORB_SETTINGS_SOUND_PAGE && key == ORB_SETTING_SOUND_SILENT)
        value = !state->model.silent;
    else if (state->page == ORB_SETTINGS_SOUND_QUIET_PAGE && key == ORB_SETTING_SOUND_QUIET_ENABLED) {
        state->changed = ORB_SETTINGS_SOUND_QUIET_PAGE;
        value = !state->model.prefs.quiet_enabled;
    } else return false;
    bool accepted = emit_setting(instance, key, value, origin, physical);
    if (!accepted) state->changed = previous;
    return accepted;
}

bool orb_app_settings_sound_set_volume(orb_screen_instance_t *instance,
    int32_t volume, orb_input_origin_t origin, bool physical)
{
    sound_state_t *state = orb_screen_state(instance);
    if (!state || state->page != ORB_SETTINGS_SOUND_VOLUME_PAGE || !state->model.interactive) return false;
    volume = LV_CLAMP(0, volume, 100);
    if (!emit_setting(instance, ORB_SETTING_SOUND_VOLUME, volume, origin, physical)) return false;
    state->volume = volume;
    orb_kit_level_set_value(state->level, volume, false);
    return true;
}

bool orb_app_settings_sound_set_hour(orb_screen_instance_t *instance,
    int32_t hour, orb_input_origin_t origin, bool physical)
{
    sound_state_t *state = orb_screen_state(instance);
    if (!state || !hour_page(state) || !state->model.interactive) return false;
    if (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_CROWN && origin != ORB_INPUT_BUTTON) return false;
    hour = LV_CLAMP(0, hour, 23);
    if (hour == state->hour) return true;
    orb_settings_sound_key_t key = state->page == ORB_SETTINGS_SOUND_START_PAGE ?
        ORB_SETTING_SOUND_QUIET_START : ORB_SETTING_SOUND_QUIET_END;
    if (!emit_setting(instance, key, hour, origin, physical)) return false;
    state->hour = hour;
    return true;
}

bool orb_app_settings_sound_step_hour(orb_screen_instance_t *instance,
    int delta, orb_input_origin_t origin, bool physical)
{
    sound_state_t *state = orb_screen_state(instance);
    if (!state || !hour_page(state) || !state->model.interactive || (delta != -1 && delta != 1)) return false;
    if (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_BUTTON) return false;
    uint32_t previous = state->changed;
    state->changed = state->page;
    bool accepted = orb_app_settings_sound_set_hour(instance, (state->hour + (delta > 0 ? 1 : 23)) % 24,
        origin, physical);
    if (!accepted) state->changed = previous;
    return accepted;
}

bool orb_app_settings_sound_preview(orb_screen_instance_t *instance,
    orb_sound_cue_t cue, orb_input_origin_t origin, bool physical)
{
    sound_state_t *state = orb_screen_state(instance);
    if (!state || state->page != ORB_SETTINGS_SOUND_PALETTE_PAGE || !state->model.interactive ||
        (unsigned)cue >= ORB_SOUND_CUE_COUNT) return false;
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = origin, .physical = physical};
    intent.value.command.target = ORB_SETTINGS_SOUND_SCREEN_ID;
    intent.value.command.action = ORB_SETTINGS_SOUND_COMMAND_PREVIEW;
    intent.value.command.value = cue;
    return orb_screen_emit(instance, &intent);
}

static void level_changed(lv_obj_t *slider, int32_t value, void *data)
{
    (void)slider;
    lv_indev_t *input = lv_indev_active();
    (void)orb_app_settings_sound_set_volume(data, value, ORB_INPUT_TOUCH,
        input && !orb_input_is_synthetic(input));
}

static void clicked(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    sound_state_t *state = orb_screen_state(instance);
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
    bool physical = input && !orb_input_is_synthetic(input);
    unsigned action = (unsigned)(uintptr_t)lv_obj_get_user_data(target);
    lv_area_t box;
    lv_obj_get_coords(target, &box);
    orb_launch_token_t launch = {.identity = action, .valid = true,
        .rect = {box.x1, box.y1, box.x2 - box.x1 + 1, box.y2 - box.y1 + 1}};
    if (action == OPEN_VOLUME || action == OPEN_QUIET || action == OPEN_PALETTE ||
        action == OPEN_START || action == OPEN_END) {
        orb_settings_sound_page_t page = action == OPEN_VOLUME ? ORB_SETTINGS_SOUND_VOLUME_PAGE :
            action == OPEN_QUIET ? ORB_SETTINGS_SOUND_QUIET_PAGE :
            action == OPEN_PALETTE ? ORB_SETTINGS_SOUND_PALETTE_PAGE :
            action == OPEN_START ? ORB_SETTINGS_SOUND_START_PAGE : ORB_SETTINGS_SOUND_END_PAGE;
        (void)orb_app_settings_sound_open(instance, page, ORB_INPUT_TOUCH, physical, &launch);
    } else if (action == OPEN_DISPLAY) {
        lv_obj_remove_state(target, LV_STATE_PRESSED);
        (void)orb_app_settings_sound_open_display(instance, ORB_INPUT_TOUCH, physical, &launch);
    } else if (action == TOGGLE_SPEECH || action == TOGGLE_SILENT || action == TOGGLE_QUIET)
        (void)orb_app_settings_sound_toggle(instance, action == TOGGLE_SPEECH ? ORB_SETTING_SOUND_SPEECH :
            action == TOGGLE_SILENT ? ORB_SETTING_SOUND_SILENT : ORB_SETTING_SOUND_QUIET_ENABLED,
            ORB_INPUT_TOUCH, physical);
    else if (action == HOUR_DOWN || action == HOUR_UP)
        (void)orb_app_settings_sound_step_hour(instance, action == HOUR_UP ? 1 : -1, ORB_INPUT_TOUCH, physical);
    else if (action >= PREVIEW_BASE && action < PREVIEW_BASE + ORB_SOUND_CUE_COUNT)
        (void)orb_app_settings_sound_preview(instance, action - PREVIEW_BASE, ORB_INPUT_TOUCH, physical);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, orb_text_style_t style,
    bool bold, int y, int width, lv_color_t color)
{
    return orb_kit_page_label(parent, text, style, bold, 233, y, width, 1, color);
}

static lv_obj_t *sound_row(orb_screen_instance_t *instance, const char *title,
    const char *subtitle, const char *value, const char *glyph, unsigned action, bool toggle, bool checked)
{
    sound_state_t *state = orb_screen_state(instance);
    lv_obj_t *row = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
        .title = title, .subtitle = subtitle, .value = value, .glyph = glyph,
        .accent = ORB_KIT_SOUND, .show_switch = toggle, .checked = checked,
    });
    lv_obj_set_user_data(row, (void *)(uintptr_t)action);
    if (action) lv_obj_add_event_cb(row, clicked, toggle ? LV_EVENT_VALUE_CHANGED : LV_EVENT_CLICKED, instance);
    else lv_obj_set_clickable(row, false);
    orb_kit_row_wrap_title(row, ORB_TEXT_REGULAR);
    orb_kit_row_set_waiting_since(row, ORB_KIT_ROW_TEXT_SUBTITLE,
        state->model.prefs.saving && subtitle && !strcmp(subtitle, "Saving…"), state->wait_started);
    return row;
}

static void sound_list(orb_screen_instance_t *instance, const char *title)
{
    sound_state_t *state = orb_screen_state(instance);
    lv_obj_t *root = orb_screen_root(instance);
    state->heading = orb_kit_rim_title_create(root, title, ORB_KIT_SECONDARY);
    state->list = orb_kit_list_page(root,
        &(orb_kit_list_page_config_t){.top = 84, .height = 344, .pad_bottom = 108});
}

static void sound_list_end(sound_state_t *state)
{
    lv_obj_set_style_pad_top(state->list, ORB_KIT_LIST_INSET, 0);
    orb_scroll_edge_follow(state->list, 32, 48);
    orb_kit_list_apply(state->list);
    orb_kit_scroll_arc_attach(state->list);
}

static void root_content(orb_screen_instance_t *instance)
{
    sound_state_t *state = orb_screen_state(instance);
    const orb_sound_preferences_t *prefs = &state->model.prefs;
    sound_list(instance, "Sound");
    char volume[12]; snprintf(volume, sizeof(volume), "%u%%", prefs->volume);
    state->volume_row = sound_row(instance, "Volume", "", volume, ORB_ICON_SPEAKER_HIGH, OPEN_VOLUME, false, false);
    orb_kit_row_set_live_value(state->volume_row, volume, "100%");
    /* Polish review #14: the root shows the worker's save state itself, on the
     * same rows that report a failure; Saving is not a Quiet Hours detail. */
    const char *saving = prefs->save_error ? ORB_ICON_WARNING " Not saved" : prefs->saving ? "Saving…" : NULL;
    sound_row(instance, "Spoken replies", saving ? saving : "", NULL,
        ORB_ICON_WAVEFORM, TOGGLE_SPEECH, true, prefs->spoken_replies);
    sound_row(instance, "Silent", saving ? saving : "Alarms ring", NULL,
        ORB_ICON_BELL_SLASH, TOGGLE_SILENT, true, state->model.silent);
    if (state->model.speech_voice[0])
        sound_row(instance, "Voice", state->model.speech_voice, NULL, ORB_ICON_WAVEFORM, 0, false, false);
    sound_row(instance, "System sounds", "Tap to preview", NULL, ORB_ICON_SPEAKER_LOW, OPEN_PALETTE, false, false);
    char quiet[80], starts[32], ends[32];
    orb_time_format_clock(starts, sizeof(starts), prefs->quiet_start, 0);
    orb_time_format_clock(ends, sizeof(ends), prefs->quiet_end, 0);
    snprintf(quiet, sizeof(quiet), "%s · %s–%s", prefs->quiet_enabled ? "On" : "Off", starts, ends);
    sound_row(instance, "Quiet hours", saving ? saving : quiet, NULL,
        ORB_ICON_MOON, OPEN_QUIET, false, false);
    sound_row(instance, "Always On", "Quiet while idle", NULL, ORB_ICON_CLOCK, OPEN_DISPLAY, false, false);
    sound_list_end(state);
}

static void palette_content(orb_screen_instance_t *instance)
{
    static const char *s_names[ORB_SOUND_CUE_COUNT] = {
        "Detent", "Tap", "Open", "Close", "Needs you", "Done", "Sent",
        "Listening start", "Listening stop", "Failed", "Home on", "Home off",
        "Timer", "Alarm", "Marble roll", "Bead hiss", "Pop", "Game switch", "Collect",
    };
    sound_state_t *state = orb_screen_state(instance);
    sound_list(instance, "System sounds");
    for (unsigned i = 0; i < ORB_SOUND_CUE_COUNT; i++)
        sound_row(instance, s_names[i], "", NULL, ORB_ICON_SPEAKER_HIGH, PREVIEW_BASE + i, false, false);
    if (state->model.notice[0]) {
        label(orb_screen_root(instance), state->model.notice, ORB_TEXT_FOOTNOTE2,
            false, 70, 330, ORB_KIT_SECONDARY);
        lv_obj_set_y(state->list, 116);
        lv_obj_set_height(state->list, 312);
    }
    sound_list_end(state);
}

static void list_edge(lv_event_t *event)
{
    lv_obj_t *list = lv_event_get_current_target_obj(event);
    if (!ORB_UI_SOFT_EDGES)
        orb_scroll_edge_apply(list, lv_obj_get_scroll_y(list) > 0 ? 16 : 0, 24);
}

static void quiet_row(orb_screen_instance_t *instance, unsigned index,
    const char *title, const char *detail, unsigned action)
{
    sound_state_t *state = orb_screen_state(instance);
    static const char *s_glyphs[] = {ORB_ICON_MOON, ORB_ICON_CLOCK, ORB_ICON_ALARM};
    bool toggle = action == TOGGLE_QUIET;
    lv_obj_t *row = orb_kit_row_create(state->list, &(orb_kit_row_config_t){
        .title = title, .subtitle = detail, .glyph = s_glyphs[index], .accent = ORB_KIT_GREY,
        .show_switch = toggle, .checked = state->model.prefs.quiet_enabled,
    });
    lv_obj_set_user_data(row, (void *)(uintptr_t)action);
    lv_obj_set_event_bubble(row, true);
    lv_obj_add_event_cb(row, clicked, toggle ? LV_EVENT_VALUE_CHANGED : LV_EVENT_CLICKED, instance);
    orb_kit_row_wrap_title(row, ORB_TEXT_REGULAR);
    orb_kit_row_set_waiting_since(row, ORB_KIT_ROW_TEXT_SUBTITLE,
        state->model.prefs.saving && !strcmp(detail, "Saving…"), state->wait_started);
}

static void quiet_content(orb_screen_instance_t *instance)
{
    sound_state_t *state = orb_screen_state(instance);
    const orb_sound_preferences_t *prefs = &state->model.prefs;
    lv_obj_t *root = orb_screen_root(instance);
    state->heading = orb_kit_rim_title_create(root, "Quiet hours", ORB_KIT_SECONDARY);
    lv_obj_update_layout(root);
    state->list = orb_kit_list_page(root,
        &(orb_kit_list_page_config_t){.top = 88, .height = 360, .pad_bottom = 24});
    lv_obj_set_scroll_elastic(state->list, !orb_ui_reduce_motion());
    if (ORB_UI_SOFT_EDGES) orb_scroll_edge_follow(state->list, 32, 48);
    else {
        orb_scroll_edge_apply(state->list, 0, 24);
        lv_obj_add_event_cb(state->list, list_edge, LV_EVENT_SCROLL, NULL);
    }
    lv_obj_set_style_pad_top(state->list, ORB_KIT_LIST_INSET, 0);
    char value[64], quiet[64];
    const char *feedback = prefs->save_error ? ORB_ICON_WARNING " Not saved" : prefs->saving ? "Saving…" : NULL;
    snprintf(quiet, sizeof(quiet), "%s", feedback && state->changed == 2 ? feedback : "");
    quiet_row(instance, 0, "Quiet hours", quiet, TOGGLE_QUIET);
    orb_time_format_clock(value, sizeof(value), prefs->quiet_start, 0);
    quiet_row(instance, 1, "Starts", value, OPEN_START);
    orb_time_format_clock(value, sizeof(value), prefs->quiet_end, 0);
    quiet_row(instance, 2, "Ends", value, OPEN_END);
    lv_obj_set_style_pad_bottom(state->list, 120, 0);
    orb_kit_list_apply(state->list);
    orb_kit_scroll_arc_attach(state->list);
}

static void hour_action(orb_screen_instance_t *instance, const char *glyph, unsigned action, int x)
{
    lv_obj_t *button = orb_kit_glyph_button_create(orb_screen_root(instance), glyph, false, ORB_KIT_SOUND);
    lv_obj_update_layout(button);
    lv_obj_set_pos(button, x - lv_obj_get_width(button) / 2, 272 - lv_obj_get_height(button) / 2);
    lv_obj_set_user_data(button, (void *)(uintptr_t)action);
    lv_obj_set_event_bubble(button, true);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, instance);
}

static void hour_content(orb_screen_instance_t *instance)
{
    sound_state_t *state = orb_screen_state(instance);
    bool start = state->page == ORB_SETTINGS_SOUND_START_PAGE;
    lv_obj_t *root = orb_screen_root(instance);
    state->heading = orb_kit_rim_title_create(root, start ? "Starts" : "Ends", ORB_KIT_SECONDARY);
    state->hour = start ? state->model.prefs.quiet_start : state->model.prefs.quiet_end;
    char value[64]; orb_time_format_clock(value, sizeof(value), state->hour, 0);
    state->hour_value = label(root, value, ORB_TEXT_TITLE1, true, 144, 320, ORB_LABEL);
    hour_action(instance, ORB_ICON_MINUS, HOUR_DOWN, 154);
    hour_action(instance, ORB_ICON_PLUS, HOUR_UP, 312);
    const char *status = state->model.notice[0] ? state->model.notice : state->model.prefs.save_error ?
        "Not saved" : state->model.prefs.saving ? "Saving…" : "Local time";
    char feedback[96];
    bool error = state->model.notice[0] || state->model.prefs.save_error;
    snprintf(feedback, sizeof(feedback), "%s%s", error ? ORB_ICON_WARNING " " : "", status);
    lv_obj_t *waiting = label(root, feedback, ORB_TEXT_FOOTNOTE1, false, 328, 300,
        error ? ORB_RED : ORB_KIT_SECONDARY);
    orb_kit_breathe_since(waiting, state->model.prefs.saving && !error, state->wait_started);
}

static void volume_content(orb_screen_instance_t *instance)
{
    sound_state_t *state = orb_screen_state(instance);
    state->level = orb_kit_level_create(orb_screen_root(instance), ORB_KIT_SECONDARY,
        0, 100, 1, level_changed, instance);
    orb_kit_level_set_title(state->level, "Volume");
    orb_kit_level_set_icon(state->level, ORB_ICON_SPEAKER_HIGH);
    orb_kit_level_set_text_style(state->level, ORB_TEXT_LARGE_TITLE, ORB_TEXT_EMPHASIZED);
    orb_kit_level_set_value_center(state->level, 233, 240);
    state->volume = state->model.prefs.volume;
    if (state->arrival) orb_kit_level_set_arrival(state->level, state->volume);
    else orb_kit_level_set_value(state->level, state->volume, false);
}

static void build(orb_screen_instance_t *instance)
{
    sound_state_t *state = orb_screen_state(instance);
    lv_obj_clean(orb_screen_root(instance));
    state->heading = state->list = state->level = state->volume_row = state->hour_value = NULL;
    if (state->page == ORB_SETTINGS_SOUND_PAGE) root_content(instance);
    else if (state->page == ORB_SETTINGS_SOUND_VOLUME_PAGE) volume_content(instance);
    else if (state->page == ORB_SETTINGS_SOUND_QUIET_PAGE) quiet_content(instance);
    else if (state->page == ORB_SETTINGS_SOUND_PALETTE_PAGE) palette_content(instance);
    else hour_content(instance);
    if (state->heading) lv_obj_move_foreground(state->heading);
    orb_screen_page_bind(instance, state->list, state->heading);
}

static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    sound_state_t *state = orb_screen_state(instance);
    state->page = args->page <= ORB_SETTINGS_SOUND_PALETTE_PAGE ? args->page : ORB_SETTINGS_SOUND_PAGE;
    lv_obj_set_clickable(orb_screen_root(instance), false);
    lv_obj_set_event_bubble(orb_screen_root(instance), true);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context,
        ORB_SETTINGS_SOUND_SCREEN_ID, &projection) && projection.model &&
        projection.model->data && projection.model->size == sizeof(state->model)) {
        memcpy(&state->model, projection.model->data, sizeof(state->model));
        state->save_state = (state->model.prefs.saving ? 1u : 0u) | (state->model.prefs.save_error ? 2u : 0u);
        state->projected = true;
    }
    if (state->model.prefs.saving) state->wait_started = lv_tick_get();
    state->arrival = true;
    build(instance);
    state->arrival = false;
    return state->page == ORB_SETTINGS_SOUND_VOLUME_PAGE ? state->level != NULL : state->heading != NULL;
}

static void update(orb_screen_instance_t *instance, const orb_screen_update_t *update)
{
    sound_state_t *state = orb_screen_state(instance);
    orb_settings_sound_model_t model = {0};
    if (update->model && update->model->data && update->model->size == sizeof(model))
        memcpy(&model, update->model->data, sizeof(model));
    unsigned save_state = (model.prefs.saving ? 1u : 0u) | (model.prefs.save_error ? 2u : 0u);
    bool rebuild = !state->projected || state->model.render_epoch != model.render_epoch ||
        (state->save_state != save_state && (state->page != ORB_SETTINGS_SOUND_VOLUME_PAGE || model.prefs.save_error));
    if (model.prefs.saving && !state->model.prefs.saving) state->wait_started = lv_tick_get();
    state->model = model;
    state->save_state = save_state;
    state->projected = true;
    if (rebuild && model.defer_save_render) {
        /* Keep the original shell redraw boundary, including held-input and
         * scroll coalescing. Polling never deletes the published viewport. */
        state->render_requested = true;
        rebuild = false;
    }
    if (rebuild) {
        capture_scroll(state);
        build(instance);
        int slot = scroll_slot(state->page);
        if (slot >= 0 && state->scroll[slot]) {
            lv_obj_scroll_to_y(state->list, state->scroll[slot], LV_ANIM_OFF);
            orb_round_list_update(state->list);
        }
    } else if (state->page == ORB_SETTINGS_SOUND_VOLUME_PAGE) {
        state->volume = model.prefs.volume;
        orb_kit_level_set_value(state->level, state->volume, true);
    } else if (state->page == ORB_SETTINGS_SOUND_PAGE) {
        char value[16]; snprintf(value, sizeof(value), "%u%%", model.prefs.volume);
        orb_kit_row_set_live_value(state->volume_row, value, "100%");
    } else if (hour_page(state)) {
        /* The old crown read the current service value even while its static
         * hour label awaited the next ordinary full render. */
        state->hour = state->page == ORB_SETTINGS_SOUND_START_PAGE ? model.prefs.quiet_start : model.prefs.quiet_end;
    }
}

static void destroy(orb_screen_instance_t *instance)
{
    (void)instance;
}

static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(sound_context_t)) return false;
    const sound_state_t *state = orb_screen_state(instance);
    sound_context_t value = {.page = state->page, .changed = state->changed, .wait_started = state->wait_started};
    memcpy(value.scroll, state->scroll, sizeof(value.scroll));
    int slot = scroll_slot(state->page);
    if (slot >= 0 && state->list) value.scroll[slot] = lv_obj_get_scroll_y(state->list);
    memcpy(context->payload, &value, sizeof(value));
    return true;
}

static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    sound_state_t *state = orb_screen_state(instance);
    sound_context_t value;
    memcpy(&value, context->payload, sizeof(value));
    if (value.page > ORB_SETTINGS_SOUND_PALETTE_PAGE || value.changed > ORB_SETTINGS_SOUND_END_PAGE) return false;
    bool rebuild = state->page != value.page || state->changed != value.changed;
    state->page = value.page;
    state->changed = value.changed;
    if (state->model.prefs.saving && value.wait_started) {
        rebuild |= state->wait_started != value.wait_started;
        state->wait_started = value.wait_started;
    }
    memcpy(state->scroll, value.scroll, sizeof(state->scroll));
    if (rebuild) build(instance);
    int slot = scroll_slot(state->page);
    if (slot >= 0 && state->scroll[slot]) orb_page_restore(state->list, &(orb_page_context_t){.scroll = state->scroll[slot]});
    return true;
}

static orb_back_result_t back(orb_screen_instance_t *instance)
{
    (void)instance;
    return ORB_BACK_LEAVE_SCREEN;
}

static void rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{
    (void)origin; (void)physical;
    sound_state_t *state = orb_screen_state(instance);
    state->rim_start = hour_page(state) ? state->hour : state->volume;
}

static void rim_preview(orb_screen_instance_t *instance, float value)
{
    sound_state_t *state = orb_screen_state(instance);
    int32_t position = (int32_t)lrintf(value);
    if (state->page == ORB_SETTINGS_SOUND_VOLUME_PAGE) {
        state->volume = position;
        orb_kit_level_set_value(state->level, position, false);
    } else if (hour_page(state)) {
        state->hour = position;
        char text[64]; orb_time_format_clock(text, sizeof(text), position, 0);
        lv_label_set_text(state->hour_value, text);
    } else set_scroll(state, position);
}

static void rim_finish(orb_screen_instance_t *instance, float value, bool commit,
    orb_input_origin_t origin, bool physical)
{
    sound_state_t *state = orb_screen_state(instance);
    if (state->page == ORB_SETTINGS_SOUND_VOLUME_PAGE) {
        if (commit) (void)orb_app_settings_sound_set_volume(instance, (int32_t)lrintf(value), origin, physical);
        else rim_preview(instance, value);
    } else if (hour_page(state)) {
        state->hour = state->rim_start;
        if (commit) (void)orb_app_settings_sound_set_hour(instance, (int32_t)lrintf(value), origin, physical);
        else rim_preview(instance, value);
    } else set_scroll(state, (int32_t)lrintf(value));
}

static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{
    sound_state_t *state = orb_screen_state(instance);
    if (state->page == ORB_SETTINGS_SOUND_VOLUME_PAGE || hour_page(state)) {
        bool hour = hour_page(state);
        *binding = (orb_rim_binding_t){.subpage = state->page, .property = hour ? state->page : 1,
            .kind = ORB_RIM_VALUE, .minimum = 0, .maximum = hour ? 23 : 100,
            .value = hour ? state->hour : state->volume, .detent = 1, .units_per_turn = hour ? 24 : 100,
            .begin = rim_begin, .preview = rim_preview, .finish = rim_finish};
        return true;
    }
    int32_t maximum = maximum_scroll(state);
    if (!maximum) return false;
    *binding = (orb_rim_binding_t){.subpage = state->page, .property = 100,
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
    const sound_state_t *state = orb_screen_state(instance);
    static const char *s_titles[] = {"Sound", "Volume", "Quiet hours", "Starts", "Ends", "System sounds"};
    *value = (orb_screen_chrome_t){.title = s_titles[state->page]};
}

static const orb_context_field_t s_context_fields[] = {
    {offsetof(sound_context_t, page), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(sound_context_t, changed), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(sound_context_t, wait_started), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(sound_context_t, scroll), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(sound_context_t, scroll) + sizeof(int32_t), sizeof(int32_t), ORB_CONTEXT_I32},
    {offsetof(sound_context_t, scroll) + 2 * sizeof(int32_t), sizeof(int32_t), ORB_CONTEXT_I32},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SETTINGS_SOUND_SCREEN_ID, .parent_id = 8, .ops = &s_ops,
    .state_bytes = sizeof(sound_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(sound_context_t),
    .context_version = 2, .context_fields = s_context_fields,
    .context_field_count = sizeof(s_context_fields) / sizeof(s_context_fields[0]),
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(sound_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(sound_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_DEVICE | ORB_DOMAIN_MODEL | ORB_DOMAIN_SETTINGS,
    .privacy = ORB_PRIVACY_PUBLIC, .always_on = ORB_AOD_HIDDEN,
    .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};

const orb_screen_descriptor_t *orb_app_settings_sound_descriptor(void)
{
    return &s_descriptor;
}

lv_obj_t *orb_app_settings_sound_scroll(const orb_screen_instance_t *instance)
{
    const sound_state_t *state = orb_screen_state(instance);
    return state ? state->list : NULL;
}

lv_obj_t *orb_app_settings_sound_level_control(const orb_screen_instance_t *instance)
{
    const sound_state_t *state = orb_screen_state(instance);
    return state ? state->level : NULL;
}

orb_settings_sound_page_t orb_app_settings_sound_page(const orb_screen_instance_t *instance)
{
    const sound_state_t *state = orb_screen_state(instance);
    return state ? state->page : ORB_SETTINGS_SOUND_PAGE;
}

orb_settings_sound_page_t orb_app_settings_sound_parent_page(const orb_screen_instance_t *instance)
{
    const sound_state_t *state = orb_screen_state(instance);
    return state && hour_page(state) ? ORB_SETTINGS_SOUND_QUIET_PAGE : ORB_SETTINGS_SOUND_PAGE;
}

int32_t orb_app_settings_sound_value(const orb_screen_instance_t *instance)
{
    const sound_state_t *state = orb_screen_state(instance);
    if (!state) return 0;
    return hour_page(state) ? state->hour : state->volume;
}

unsigned orb_app_settings_sound_changed(const orb_screen_instance_t *instance)
{
    const sound_state_t *state = orb_screen_state(instance);
    return state ? state->changed : 0;
}

bool orb_app_settings_sound_restore_group(orb_screen_instance_t *instance,
    const orb_screen_context_t *context, size_t capacity)
{
    sound_state_t *state = orb_screen_state(instance);
    if (!state || !orb_screen_context_valid(&s_descriptor, context, capacity)) return false;
    uint32_t storage[(sizeof(orb_screen_context_t) + sizeof(sound_context_t)) / sizeof(uint32_t)];
    orb_screen_context_t *copy = (orb_screen_context_t *)storage;
    memcpy(copy, context, sizeof(storage));
    sound_context_t value;
    memcpy(&value, copy->payload, sizeof(value));
    value.page = state->page;
    memcpy(copy->payload, &value, sizeof(value));
    return orb_screen_restore(instance, copy, sizeof(storage));
}

bool orb_app_settings_sound_take_render_request(orb_screen_instance_t *instance)
{
    sound_state_t *state = orb_screen_state(instance);
    if (!state) return false;
    bool requested = state->render_requested;
    state->render_requested = false;
    return requested;
}
