#pragma once

#include "orb_model.h"
#include "orb_screen.h"

/* Equal to the shell's ORB_SHELL_WEATHER place. The compatibility host keys
 * migrated descriptors by their surface, as Settings does. */
#define ORB_WEATHER_SCREEN_ID 31

/* Call-scoped immutable projection. The shell formats local clock text, so this
 * presentation never reads time zones, services or shell internals. Values are
 * already in the hub's unit; the screen never converts them again. */
typedef struct {
    char time[16];
    float temperature;
    orb_weather_condition_t condition;
    bool is_day;
} orb_weather_screen_hour_t;

typedef struct {
    bool online, synced, supported, configured, available, stale;
    char place[65];
    float temperature, high, low;
    orb_weather_condition_t condition;
    bool is_day;
    char updated[24]; /* "9:41 AM"; empty when no time is known. */
    uint32_t hourly_count;
    orb_weather_screen_hour_t hourly[ORB_MODEL_MAX_WEATHER_HOURS];
} orb_weather_screen_model_t;

typedef enum {
    ORB_WEATHER_VIEW_NOT_CONFIGURED,
    ORB_WEATHER_VIEW_LOADING,
    ORB_WEATHER_VIEW_OFFLINE,
    ORB_WEATHER_VIEW_UNAVAILABLE,
    ORB_WEATHER_VIEW_CURRENT,
    ORB_WEATHER_VIEW_STALE, /* Retained values, explicitly marked as not current. */
} orb_weather_view_t;

const orb_screen_descriptor_t *orb_app_weather_descriptor(void);
orb_weather_view_t orb_app_weather_classify(const orb_weather_screen_model_t *model);

/* Semantic queries for lifecycle fixtures and parity traces, never topology. */
orb_weather_view_t orb_app_weather_view(const orb_screen_instance_t *instance);
lv_obj_t *orb_app_weather_scroll(const orb_screen_instance_t *instance);
bool orb_app_weather_hourly_rows(const orb_screen_instance_t *instance);
/* Counts compositions. Live value revisions must leave it unchanged. */
unsigned orb_app_weather_builds(const orb_screen_instance_t *instance);
