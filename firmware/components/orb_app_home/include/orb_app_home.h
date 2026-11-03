#pragma once

#include "orb_home_command.h"
#include "orb_screen.h"

/* Compatible with the shell's existing ORB_SHELL_HOME route and developer
 * tooling. The shell owns Home's canonical Face parent and outer history. */
#define ORB_APP_HOME_SCREEN_ID 18

/* Call-scoped immutable projection. A private fixture snapshot is borrowed
 * only by create; NULL means the view reads the live Home service. */
typedef struct {
    unsigned hour;
    bool contrast, system_navigation;
    const orb_home_snapshot_t *fixture;
} orb_app_home_model_t;

const orb_screen_descriptor_t *orb_app_home_descriptor(void);

/* Home writes arrive as ORB_INTENT_HOME_SET_LEVEL (dial values) or
 * ORB_INTENT_HOME_COMMAND (discrete sets, unlock, toggles, scenes, room off).
 * The host decodes, then executes; command->id borrows the intent's text. */
bool orb_app_home_command_from_intent(const orb_intent_t *intent, orb_home_command_t *command);

/* Semantic queries for the shell's existing outer navigation and rim policy.
 * Every call accepts a NULL/closed instance and then does nothing. */
lv_obj_t *orb_app_home_scroll(orb_screen_instance_t *instance);
bool orb_app_home_can_back(const orb_screen_instance_t *instance);
bool orb_app_home_dial(orb_screen_instance_t *instance, float *value, float *step);
/* Native lists leave value ownership to the shell's scroll path. */
bool orb_app_home_dial_writes(const orb_screen_instance_t *instance);
void orb_app_home_rotate(orb_screen_instance_t *instance, float value);
void orb_app_home_rotary(orb_screen_instance_t *instance, float delta);
/* Also the contact-suspension entry point before lock, alerts or layers. */
void orb_app_home_rotary_finish(orb_screen_instance_t *instance, bool commit);
/* Explicit fresh shell entry, separate from context restore and local Back. */
void orb_app_home_enter(orb_screen_instance_t *instance);
/* Stack's Scenes shortcut, applied after any restored context. */
void orb_app_home_show_scenes(orb_screen_instance_t *instance);

/* Private fixture motion query. */
bool orb_app_home_moving(const orb_screen_instance_t *instance);

/* Suspended navigation preview: render fresh-arrival start pixels. */
void orb_app_home_arrival_snapshot(orb_screen_instance_t *instance);

/* Called by the host after commit; only a cold, unrestored active instance arms. */
void orb_app_home_arrival_prepare(orb_screen_instance_t *instance);
