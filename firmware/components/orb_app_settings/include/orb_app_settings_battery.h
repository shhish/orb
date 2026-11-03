#pragma once

#include "orb_screen.h"

#define ORB_SETTINGS_BATTERY_SCREEN_ID 29
#define ORB_SETTINGS_KEY_DESK_LIMIT 590u

typedef struct {
    bool present, usb, charging, desk_limit_active;
    bool health_valid, temperature_valid;
    int battery_pct, health_pct;
    float temperature_c;
    bool external_usb, desk_charge_limit, battery_setting_error, fahrenheit;
} orb_settings_battery_model_t;

const orb_screen_descriptor_t *orb_app_settings_battery_descriptor(void);
lv_obj_t *orb_app_settings_battery_scroll(const orb_screen_instance_t *instance);

/* An explicit desk-limit activation is a physical touch. The rim only scrolls.
 * The host copies/enqueues the typed intent before returning. */
bool orb_app_settings_battery_toggle_desk_limit(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical);
bool orb_app_settings_battery_desk_checked(const orb_screen_instance_t *instance);
/* Semantic action geometry for pointer fixtures and spatial launch anchors. */
bool orb_app_settings_battery_desk_bounds(const orb_screen_instance_t *instance,
    lv_area_t *bounds);
