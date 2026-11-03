#pragma once

#include "orb_app_host.h"
#include "orb_home_text.h"
#include "orb_shell.h"

/* Compatible action IDs and selected-property keys; the shell retains the
 * button policy, service writes and frame-coalesced brightness mailbox. */
enum {
    ORB_CONTROL_SILENT = 263, ORB_CONTROL_SLEEP = 483,
    ORB_CONTROL_QUIET = 484, ORB_CONTROL_FLASHLIGHT = 485,
    ORB_CONTROL_HOME_SCENE = 381, ORB_CONTROL_HOME_FIRST = 382, ORB_CONTROL_HOME_SECOND = 383,
    ORB_CONTROL_BRIGHTNESS = 700, ORB_CONTROL_VOLUME = 701,
};

/* Only the two visible favourites are copied from the call-scoped COW read
 * view. Neither descriptor nor host retains a complete Home generation. */
typedef struct {
    bool ready, has_scene, present[2];
    orb_home_scene_t scene;
    orb_home_accessory_t favourites[2];
    orb_home_status_t status, favourite_status[2];
    uint32_t wait_started;
} orb_control_home_model_t;

typedef struct {
    int brightness, battery_pct;
    unsigned volume;
    bool charging, silent, quiet, flashlight, control_volume, home_panel;
    orb_control_home_model_t home;
} orb_control_model_t;

orb_control_model_t orb_control_project(const orb_shell_snapshot_t *status,
    unsigned volume, bool silent, bool quiet, bool flashlight, bool control_volume);
const orb_screen_descriptor_t *orb_control_descriptor(void);
orb_app_host_t *orb_control_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context);
bool orb_control_host_attach(orb_app_host_t *host, lv_obj_t *page, const orb_control_model_t *model);
bool orb_control_host_update(orb_app_host_t *host, const orb_control_model_t *model);
lv_obj_t *orb_control_level(orb_screen_instance_t *instance);
/* The compatibility crown keeps its original service/cue order. These only
 * update its retained controls or retire an accepted launch press. */
void orb_control_value(orb_screen_instance_t *instance, bool volume, int percent);
void orb_control_prepare_launch(orb_screen_instance_t *instance, unsigned action);

/* The same selection is used at composition and at the shell's current-model
 * action boundary: favourite actions deliberately resolve by ordinal. */
const orb_home_scene_t *orb_control_home_scene(const orb_home_snapshot_t *home, unsigned hour);
const orb_home_accessory_t *orb_control_home_favourite(const orb_home_snapshot_t *home, unsigned slot);
void orb_control_home_project(orb_control_model_t *model, const orb_home_snapshot_t *home, unsigned hour);

/* One absolute Loading budget owned by the workspace, across CC rebuilds. */
uint32_t orb_control_home_wait(bool *waiting, uint32_t *started, const orb_home_snapshot_t *home);
