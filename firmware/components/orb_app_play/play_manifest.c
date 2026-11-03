#include "orb_app_play.h"
#include "orb_app_play_registry.h"

const orb_app_manifest_t *orb_app_play_manifest(void)
{
    static const orb_screen_descriptor_t *s_screens[2];
    static orb_app_manifest_t s_manifest;
    s_screens[0] = orb_app_play_descriptor();
    s_screens[1] = orb_app_play_game_descriptor();
    s_manifest = (orb_app_manifest_t){.id = ORB_APP_PLAY_SCREEN_ID, .name = "Play",
        .screens = s_screens, .screen_count = 2, .entry = ORB_APP_PLAY_SCREEN_ID};
    return &s_manifest;
}
