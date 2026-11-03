#pragma once

#include "orb_screen.h"

#define ORB_SETTINGS_DISPLAY_SCREEN_ID 9

typedef enum {
    ORB_SETTINGS_DISPLAY_PAGE,
    ORB_SETTINGS_BRIGHTNESS_PAGE,
    ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE,
} orb_settings_display_page_t;

/* These are semantic keys, independent of the retired workspace action IDs.
 * The host coalesces the two level keys until REFR_READY, preserving the
 * device mailbox timing and keeping pending values alive after view teardown. */
typedef enum {
    ORB_SETTING_DISPLAY_BRIGHTNESS = 100,
    ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS,
    ORB_SETTING_DISPLAY_ALWAYS_ON,
    ORB_SETTING_DISPLAY_PICKUP,
    ORB_SETTING_DISPLAY_QUIET,
} orb_settings_display_key_t;

typedef struct {
    int32_t brightness;
    uint32_t always_on_brightness;
    bool always_on, motion_wake, quiet, has_host;
    /* Native projections keep interactive true. The live host accept callback
     * fences fixture/suppressed/opening input; preview is framework-read-only. */
    bool interactive, level_pending;
} orb_settings_display_model_t;

const orb_screen_descriptor_t *orb_app_settings_display_descriptor(void);
lv_obj_t *orb_app_settings_display_scroll(const orb_screen_instance_t *instance);
lv_obj_t *orb_app_settings_display_level_control(const orb_screen_instance_t *instance);
orb_settings_display_page_t orb_app_settings_display_page(const orb_screen_instance_t *instance);
int32_t orb_app_settings_display_level(const orb_screen_instance_t *instance);

/* The same semantic actions drive rows, direct slider callbacks and tests.
 * Model projections, context restore and preview construction never call these. */
bool orb_app_settings_display_open(orb_screen_instance_t *instance,
    orb_settings_display_page_t page, orb_input_origin_t origin, bool physical,
    const orb_launch_token_t *launch);
bool orb_app_settings_display_toggle(orb_screen_instance_t *instance,
    orb_settings_display_key_t key, orb_input_origin_t origin, bool physical);
bool orb_app_settings_display_set_level(orb_screen_instance_t *instance,
    int32_t value, orb_input_origin_t origin, bool physical);
