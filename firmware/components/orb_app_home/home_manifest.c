#include "orb_app_home.h"
#include "orb_app_home_registry.h"

const orb_app_manifest_t *orb_app_home_manifest(void)
{
    static const orb_screen_descriptor_t *s_screens[1];
    static orb_app_manifest_t s_manifest;
    s_screens[0] = orb_app_home_descriptor();
    s_manifest = (orb_app_manifest_t){
        .id = ORB_APP_HOME_SCREEN_ID,
        .name = "Home",
        .screens = s_screens,
        .screen_count = 1,
        .entry = ORB_APP_HOME_SCREEN_ID,
    };
    return &s_manifest;
}
