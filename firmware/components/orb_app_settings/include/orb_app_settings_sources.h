#pragma once

#include "orb_screen.h"

/* Existing Session-source Settings route. Route28 is the Agents sessions list. */
#define ORB_SETTINGS_SOURCES_SCREEN_ID 17
#define ORB_SETTINGS_SOURCE_COUNT 3

typedef enum {
    ORB_SETTING_SOURCE_CLAUDE = 400,
    ORB_SETTING_SOURCE_CODEX,
    ORB_SETTING_SOURCE_DOT,
    ORB_SETTING_SOURCE_RECENT_MINUTES,
} orb_settings_source_key_t;

typedef struct {
    bool sources[ORB_SETTINGS_SOURCE_COUNT];
    uint32_t idle_minutes;
    bool pending, ready, interactive;
    bool known; /* False offline before the hub ever confirmed sources. */
    char error[33], notice[64];
    uint32_t render_epoch; /* Optional bridge revision for an existing full render. */
    /* Last accepted local UI hub-preference intent, not a pending command.
     * Remote/global preference updates do not advance this operation. */
    uint64_t hub_pref_operation;
    uint32_t hub_pref_key;
} orb_settings_sources_model_t;

const orb_screen_descriptor_t *orb_app_settings_sources_descriptor(void);
lv_obj_t *orb_app_settings_sources_scroll(const orb_screen_instance_t *instance);
int orb_app_settings_sources_changed(const orb_screen_instance_t *instance);
bool orb_app_settings_sources_requests_enabled(const orb_screen_instance_t *instance);

bool orb_app_settings_sources_toggle(orb_screen_instance_t *instance, unsigned source,
    orb_input_origin_t origin, bool physical);
bool orb_app_settings_sources_cycle_recent(orb_screen_instance_t *instance,
    orb_input_origin_t origin, bool physical);

/* Semantic diagnostics/actions do not expose the kit's child topology. */
bool orb_app_settings_sources_switch_checked(const orb_screen_instance_t *instance, unsigned source);
bool orb_app_settings_sources_action_bounds(const orb_screen_instance_t *instance,
    unsigned source, bool floating_switch, lv_area_t *bounds);
