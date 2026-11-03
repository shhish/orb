#pragma once

#include "orb_screen.h"
#include "orb_sound.h"

#define ORB_SETTINGS_SOUND_SCREEN_ID 21

typedef enum {
    ORB_SETTINGS_SOUND_PAGE,
    ORB_SETTINGS_SOUND_VOLUME_PAGE,
    ORB_SETTINGS_SOUND_QUIET_PAGE,
    ORB_SETTINGS_SOUND_START_PAGE,
    ORB_SETTINGS_SOUND_END_PAGE,
    ORB_SETTINGS_SOUND_PALETTE_PAGE,
} orb_settings_sound_page_t;

typedef enum {
    ORB_SETTING_SOUND_VOLUME = 300,
    ORB_SETTING_SOUND_SPEECH,
    ORB_SETTING_SOUND_SILENT,
    ORB_SETTING_SOUND_QUIET_ENABLED,
    ORB_SETTING_SOUND_QUIET_START,
    ORB_SETTING_SOUND_QUIET_END,
} orb_settings_sound_key_t;

typedef enum {
    ORB_SETTINGS_SOUND_COMMAND_PREVIEW = 1,
} orb_settings_sound_command_t;

/* Immutable, copied projection. The sound preference type is a value-only
 * public service contract. No callback/worker handle crosses into the app.
 * render_epoch advances only at an existing full-render trigger. */
typedef struct {
    orb_sound_preferences_t prefs;
    char speech_voice[65], notice[64];
    bool silent, interactive, defer_save_render;
    uint32_t render_epoch;
} orb_settings_sound_model_t;

const orb_screen_descriptor_t *orb_app_settings_sound_descriptor(void);
lv_obj_t *orb_app_settings_sound_scroll(const orb_screen_instance_t *instance);
lv_obj_t *orb_app_settings_sound_level_control(const orb_screen_instance_t *instance);
orb_settings_sound_page_t orb_app_settings_sound_page(const orb_screen_instance_t *instance);
orb_settings_sound_page_t orb_app_settings_sound_parent_page(const orb_screen_instance_t *instance);
int32_t orb_app_settings_sound_value(const orb_screen_instance_t *instance);
unsigned orb_app_settings_sound_changed(const orb_screen_instance_t *instance);

bool orb_app_settings_sound_open(orb_screen_instance_t *instance,
    orb_settings_sound_page_t page, orb_input_origin_t origin, bool physical,
    const orb_launch_token_t *launch);
bool orb_app_settings_sound_open_display(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical, const orb_launch_token_t *launch);
bool orb_app_settings_sound_toggle(orb_screen_instance_t *instance,
    orb_settings_sound_key_t key, orb_input_origin_t origin, bool physical);
bool orb_app_settings_sound_set_volume(orb_screen_instance_t *instance,
    int32_t volume, orb_input_origin_t origin, bool physical);
/* Absolute crown values clamp; the two visible hour buttons wrap at midnight. */
bool orb_app_settings_sound_set_hour(orb_screen_instance_t *instance,
    int32_t hour, orb_input_origin_t origin, bool physical);
bool orb_app_settings_sound_step_hour(orb_screen_instance_t *instance,
    int delta, orb_input_origin_t origin, bool physical);
bool orb_app_settings_sound_preview(orb_screen_instance_t *instance,
    orb_sound_cue_t cue, orb_input_origin_t origin, bool physical);

/* Same-ID page routing carries the latest opaque group context, including
 * feedback attribution and all list scroll slots, while route args choose the
 * destination page. This performs only value restoration, never a command. */
bool orb_app_settings_sound_restore_group(orb_screen_instance_t *instance,
    const orb_screen_context_t *context, size_t capacity);

/* Retained polling asks the outer compatibility renderer for its old redraw. */
bool orb_app_settings_sound_take_render_request(orb_screen_instance_t *);
