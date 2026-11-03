#pragma once

#include "orb_screen.h"

#define ORB_SETTINGS_ROOT_SCREEN_ID 8

typedef enum {
    ORB_SETTINGS_ROOT_PAGE,
    ORB_SETTINGS_GLASS_PAGE,
} orb_settings_root_page_t;

typedef enum {
    ORB_SETTINGS_ITEM_DISPLAY,
    ORB_SETTINGS_ITEM_SOUND,
    ORB_SETTINGS_ITEM_ACCESSIBILITY,
    ORB_SETTINGS_ITEM_MAC,
    ORB_SETTINGS_ITEM_ABOUT,
    ORB_SETTINGS_ITEM_SOURCES,
    ORB_SETTINGS_ITEM_UPDATES,
    ORB_SETTINGS_ITEM_UNITS,
    ORB_SETTINGS_ITEM_CONTROLS,
    ORB_SETTINGS_ITEM_BATTERY,
    ORB_SETTINGS_ITEM_GLASS,
    ORB_SETTINGS_ITEM_COUNT,
} orb_settings_root_item_t;

enum {
    ORB_SETTINGS_SETTING_GLASS = 2,
    ORB_SETTINGS_COMMAND_CONNECTION = 1,
};

typedef struct {
    char hub_name[64];
    char firmware_version[32];
    uint32_t glass_preference; /* Clear=0, Default=1, Tinted=2. */
    bool sound_on;
    bool interactive;
} orb_settings_root_model_t;

const orb_screen_descriptor_t *orb_app_settings_root_descriptor(void);
lv_obj_t *orb_app_settings_root_scroll(const orb_screen_instance_t *instance);
orb_settings_root_page_t orb_app_settings_root_page(const orb_screen_instance_t *instance);
uint32_t orb_app_settings_glass_preference(const orb_screen_instance_t *instance);

bool orb_app_settings_root_activate(orb_screen_instance_t *instance,
    orb_settings_root_item_t item, orb_input_origin_t origin, bool physical,
    const orb_launch_token_t *launch);
bool orb_app_settings_glass_select(orb_screen_instance_t *instance,
    uint32_t preference, orb_input_origin_t origin, bool physical);
