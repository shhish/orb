#pragma once
#include "orb_app_host.h"
#include "orb_app_faces.h"
#include "orb_shell.h"

orb_app_host_t *orb_faces_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context);
orb_app_host_t *orb_faces_host_public_create(lv_obj_t *parent);
bool orb_faces_host_attach(orb_app_host_t *host, lv_obj_t *page);
void orb_faces_host_update(orb_app_host_t *host, const orb_faces_snapshot_t *snapshot);
/* Both live and cold ordinary Faces use the same immutable status adapter. */
void orb_faces_host_project(orb_faces_snapshot_t *snapshot, const orb_model_snapshot_t *model,
    const orb_home_snapshot_t *home, const orb_shell_snapshot_t *status);
/* Execute a copied preference command and deliver its retained service ticket
 * synchronously; result polling/retry stays with the originating presenter. */
bool orb_faces_host_preferences(orb_screen_instance_t *instance, const orb_intent_t *intent);
orb_screen_instance_t *orb_faces_host_current(orb_app_host_t *host);
bool orb_faces_host_rim(orb_app_host_t *host, orb_nav_t *nav, orb_nav_id_t route);

/* Preserve the last-lit sample on sleep; admit the ancestor before wake Follow. */
void orb_faces_host_motion(lv_obj_t *root, orb_screen_instance_t *face, bool active, bool shown, bool save);
