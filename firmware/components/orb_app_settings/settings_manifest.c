#include "orb_app_settings.h"
#include "orb_app_settings_root.h"
#include "orb_app_settings_sound.h"
#include "orb_app_settings_display.h"
#include "orb_app_settings_battery.h"
#include "orb_app_settings_accessibility.h"
#include "orb_app_settings_units.h"
#include "orb_app_settings_sources.h"
#include "orb_app_settings_registry.h"

const orb_app_manifest_t *orb_app_settings_manifest(void)
{
    static const orb_screen_descriptor_t *s_screens[9];
    static orb_app_manifest_t s_manifest;
    s_screens[0] = orb_app_settings_about_descriptor();
    s_screens[1] = orb_app_settings_units_descriptor();
    s_screens[2] = orb_app_settings_temperature_descriptor();
    s_screens[3] = orb_app_settings_root_descriptor();
    s_screens[4] = orb_app_settings_sources_descriptor();
    s_screens[5] = orb_app_settings_accessibility_descriptor();
    s_screens[6] = orb_app_settings_battery_descriptor();
    s_screens[7] = orb_app_settings_display_descriptor();
    s_screens[8] = orb_app_settings_sound_descriptor();
    s_manifest = (orb_app_manifest_t){
        .id = ORB_SETTINGS_ROOT_SCREEN_ID,
        .name = "Settings",
        .screens = s_screens,
        .screen_count = 9,
        .entry = ORB_SETTINGS_ROOT_SCREEN_ID,
    };
    return &s_manifest;
}
