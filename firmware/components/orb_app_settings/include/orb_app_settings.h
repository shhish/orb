#pragma once

#include "orb_screen.h"

/* Compatibility IDs remain usable by existing shell/developer routes. */
#define ORB_SETTINGS_ABOUT_SCREEN_ID 11
#define ORB_SETTINGS_ROOT_SCREEN_ID 8

typedef enum {
    ORB_SETTINGS_ABOUT_PAGE,
    ORB_SETTINGS_LICENCES_PAGE,
} orb_settings_about_page_t;

/* Call-scoped immutable projection. The shell interprets connection state;
 * this presentation component never includes shell or service internals. */
typedef struct {
    char firmware_version[32];
    char device_id[32];
    char hub_name[64];
    char connection_text[64];
    bool ready;
} orb_settings_about_model_t;

const orb_screen_descriptor_t *orb_app_settings_about_descriptor(void);
lv_obj_t *orb_app_settings_about_scroll(const orb_screen_instance_t *instance);
orb_settings_about_page_t orb_app_settings_about_page(const orb_screen_instance_t *instance);

/* Semantic action used by the row callback and by lifecycle/intent fixtures. */
bool orb_app_settings_about_open_licences(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical, const orb_launch_token_t *launch);
