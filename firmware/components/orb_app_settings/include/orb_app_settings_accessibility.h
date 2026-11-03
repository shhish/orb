#pragma once

#include "orb_screen.h"

#define ORB_SETTINGS_ACCESSIBILITY_SCREEN_ID 10

typedef enum {
    ORB_SETTINGS_ACCESSIBILITY_PAGE,
    ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE,
    ORB_SETTINGS_ACCESS_BOLD_PAGE,
    ORB_SETTINGS_ACCESS_MOTION_PAGE,
    ORB_SETTINGS_ACCESS_TRANSPARENCY_PAGE,
    ORB_SETTINGS_ACCESS_CONTRAST_PAGE,
} orb_settings_accessibility_page_t;

typedef enum {
    ORB_SETTING_ACCESS_TEXT_SIZE = 200,
    ORB_SETTING_ACCESS_BOLD,
    ORB_SETTING_ACCESS_MOTION,
    ORB_SETTING_ACCESS_TRANSPARENCY,
    ORB_SETTING_ACCESS_CONTRAST,
} orb_settings_accessibility_key_t;

/* Immutable value projection; only the host writes global type/material/power
 * preferences or interprets their success/failure. Notice retains exact copy. */
typedef struct {
    uint32_t content_size;
    bool bold, reduce_motion, reduce_transparency, contrast, interactive;
    char notice[64];
} orb_settings_accessibility_model_t;

const orb_screen_descriptor_t *orb_app_settings_accessibility_descriptor(void);
lv_obj_t *orb_app_settings_accessibility_scroll(const orb_screen_instance_t *instance);
orb_settings_accessibility_page_t orb_app_settings_accessibility_page(const orb_screen_instance_t *instance);
const char *orb_app_settings_accessibility_value(const orb_screen_instance_t *instance, unsigned index);

bool orb_app_settings_accessibility_open(orb_screen_instance_t *instance,
    unsigned index, orb_input_origin_t origin, bool physical, const orb_launch_token_t *launch);
bool orb_app_settings_accessibility_choose_size(orb_screen_instance_t *instance,
    unsigned size, orb_input_origin_t origin, bool physical);
bool orb_app_settings_accessibility_toggle(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical);
