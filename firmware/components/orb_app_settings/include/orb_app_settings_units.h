#pragma once

#include "orb_screen.h"
#include "orb_units.h"

#define ORB_SETTINGS_UNITS_SCREEN_ID 19
#define ORB_SETTINGS_TEMPERATURE_SCREEN_ID 20

typedef enum {
    ORB_SETTINGS_SETTING_TEMPERATURE = 1,
} orb_settings_setting_key_t;

typedef enum {
    ORB_SETTINGS_TEMPERATURE_AUTOMATIC,
    ORB_SETTINGS_TEMPERATURE_F,
    ORB_SETTINGS_TEMPERATURE_C,
} orb_settings_temperature_selection_t;

typedef struct {
    orb_temperature_t temperature;
    bool temperature_owner, prefs_pending, ready;
    bool known; /* False offline before the hub ever confirmed the unit. */
    char prefs_error[33];
    char notice[64];
} orb_settings_units_model_t;

const orb_screen_descriptor_t *orb_app_settings_units_descriptor(void);
const orb_screen_descriptor_t *orb_app_settings_temperature_descriptor(void);
lv_obj_t *orb_app_settings_units_scroll(const orb_screen_instance_t *instance);

bool orb_app_settings_units_open_temperature(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical, const orb_launch_token_t *launch);
bool orb_app_settings_temperature_select(orb_screen_instance_t *instance,
    orb_settings_temperature_selection_t selection, orb_input_origin_t origin, bool physical);
bool orb_app_settings_temperature_enabled(const orb_screen_instance_t *instance);
orb_settings_temperature_selection_t orb_app_settings_temperature_selection(const orb_screen_instance_t *instance);
