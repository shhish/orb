#include "orb_app_faces.h"
#include "orb_app_faces_registry.h"

const orb_app_manifest_t *orb_app_faces_manifest(void)
{
    static const orb_screen_descriptor_t *s_screens[2];
    static orb_app_manifest_t s_manifest;
    s_screens[0] = orb_app_faces_descriptor();
    s_screens[1] = orb_app_faces_public_descriptor();
    s_manifest = (orb_app_manifest_t){.id = ORB_APP_FACES_SCREEN_ID, .name = "Faces",
        .screens = s_screens, .screen_count = 2, .entry = ORB_APP_FACES_SCREEN_ID};
    return &s_manifest;
}
