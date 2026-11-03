#include "orb_app_voice.h"
#include "orb_app_voice_spoken.h"
#include "orb_app_voice_registry.h"

const orb_app_manifest_t *orb_app_voice_manifest(void)
{
    static const orb_screen_descriptor_t *s_screens[2];
    static orb_app_manifest_t s_manifest;
    s_screens[0] = orb_app_voice_descriptor(); s_screens[1] = orb_app_voice_spoken_descriptor();
    s_manifest = (orb_app_manifest_t){.id = ORB_APP_VOICE_SCREEN, .name = "Voice",
        .screens = s_screens, .screen_count = 2, .entry = ORB_APP_VOICE_SCREEN};
    return &s_manifest;
}
