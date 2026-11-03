#include "orb_app_agents.h"
#include "orb_app_agents_registry.h"

const orb_app_manifest_t *orb_app_agents_manifest(void)
{
    static const orb_screen_descriptor_t *s_screens[7];
    static orb_app_manifest_t s_manifest;
    const orb_screen_id_t routes[] = {ORB_APP_AGENTS_SCREEN, ORB_APP_AGENTS_SESSION,
        ORB_APP_AGENTS_CARD, ORB_APP_AGENTS_DOT, ORB_APP_AGENTS_IDLE,
        ORB_APP_AGENTS_USAGE, ORB_APP_AGENTS_SESSIONS};
    for (unsigned i = 0; i < 7; i++) s_screens[i] = orb_app_agents_descriptor(routes[i]);
    s_manifest = (orb_app_manifest_t){.id = ORB_APP_AGENTS_SCREEN, .name = "Agents",
        .screens = s_screens, .screen_count = 7, .entry = ORB_APP_AGENTS_SCREEN};
    return &s_manifest;
}
