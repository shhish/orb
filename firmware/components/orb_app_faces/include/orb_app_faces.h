#pragma once

#include "orb_faces.h"
#include "orb_home.h"
#include "orb_screen.h"

#define ORB_APP_FACES_SCREEN_ID 0
#define ORB_APP_FACES_PUBLIC_SCREEN_ID 0xff00
#define ORB_APP_FACES_OPEN 0xface0001u
#define ORB_APP_FACES_PREFERENCES 0xface0002u

/* The large service snapshots remain in the workspace's refcounted generations.
 * Only this bounded face summary is copied into the shared host. */
typedef struct {
    orb_faces_snapshot_t snapshot;
} orb_app_faces_model_t;

typedef struct {
    uint32_t prefs[sizeof(orb_faces_prefs_t)];
    uint32_t mode, slot, face_before_gallery;
    bool browsing, dirty, save_error, failed_select;
} orb_app_faces_context_t;

const orb_screen_descriptor_t *orb_app_faces_descriptor(void);
const orb_screen_descriptor_t *orb_app_faces_public_descriptor(void);
void orb_app_faces_snapshot(orb_faces_snapshot_t *out, const orb_model_snapshot_t *model,
    const orb_home_snapshot_t *home, const char *clock, const char *date, int battery,
    bool charging, bool online);
/* Remove the household presence signal and unused weather location before the
 * public host receives a projection. Visible public summaries stay unchanged. */
void orb_app_faces_public_projection(orb_faces_snapshot_t *snapshot);
bool orb_app_faces_preferences_from_intent(const orb_intent_t *intent, orb_faces_prefs_t *prefs);
void orb_app_faces_request_result(orb_screen_instance_t *instance, uint64_t ticket);

lv_obj_t *orb_app_faces_root(orb_screen_instance_t *instance);
orb_faces_mode_t orb_app_faces_mode(orb_screen_instance_t *instance);
void orb_app_faces_active(orb_screen_instance_t *instance, bool active, bool save);
void orb_app_faces_hold(orb_screen_instance_t *instance);
void orb_app_faces_confirm_selection(orb_screen_instance_t *instance);
bool orb_app_faces_rotary(orb_screen_instance_t *instance, int *value, int *maximum);
void orb_app_faces_rotate(orb_screen_instance_t *instance, int value);
void orb_app_faces_rotary_finish(orb_screen_instance_t *instance, bool commit);
/* The same instance, including its save ticket and editor rollback target,
 * survives a system layer. Value contexts never contain those resources. */
bool orb_app_faces_retain(orb_screen_instance_t *instance, lv_obj_t *root);
bool orb_app_faces_resume(orb_screen_instance_t *instance, lv_obj_t *page);
bool orb_app_faces_retained(orb_screen_instance_t *instance);
