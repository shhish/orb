#include "orb_app_time.h"
#include "orb_app_time_registry.h"

const orb_app_manifest_t *orb_app_time_manifest(void)
{
    static const orb_screen_descriptor_t *s_screens[1];
    static orb_app_manifest_t s_manifest;
    s_screens[0] = orb_app_time_descriptor();
    s_manifest = (orb_app_manifest_t){
        .id = ORB_APP_TIME_SCREEN_ID,
        .name = "Time",
        .screens = s_screens,
        .screen_count = 1,
        .entry = ORB_APP_TIME_SCREEN_ID,
    };
    return &s_manifest;
}
