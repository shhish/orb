#pragma once

#include "orb_screen.h"
#include "orb_time_view.h"

/* Compatible with the shell's existing ORB_SHELL_TIME route. The service in
 * orb_time owns every timer, stopwatch, alarm and reminder: destroying this
 * screen never stops one, and restoring its context never replays one. */
#define ORB_APP_TIME_SCREEN_ID 22

/* Call-scoped immutable projection: environment only. Time's view reads the
 * service snapshot itself on its existing poll clock. */
typedef struct {
    bool contrast, system_input;
} orb_app_time_model_t;

const orb_screen_descriptor_t *orb_app_time_descriptor(void);

/* Time service changes arrive as ORB_INTENT_APP_COMMAND: action is the
 * command kind, target the presenter's timestamp, value/extra its arguments.
 * The host decodes, then executes through orb_time_command_execute. */
bool orb_app_time_command_from_intent(const orb_intent_t *intent, orb_time_command_t *command);

/* After the host restores any saved context: the default page when nothing
 * was restored, then the service's ringing alarm or due reminder, which
 * always take the dial. */
void orb_app_time_enter(orb_screen_instance_t *instance, bool alarm_ringing, bool reminder_due);
orb_time_page_t orb_app_time_page(const orb_screen_instance_t *instance);
/* True once per page change since the previous query (cached planes). */
bool orb_app_time_page_changed(orb_screen_instance_t *instance);
/* An editor sheet or reminder detail consumes Back before Time is left. */
bool orb_app_time_local_back(orb_screen_instance_t *instance);

/* The shell's existing rim handoff. NULL/closed instances do nothing. */
lv_obj_t *orb_app_time_scroll(orb_screen_instance_t *instance);
bool orb_app_time_rotary(orb_screen_instance_t *instance, orb_time_rotary_t *owner);
void orb_app_time_rotate(orb_screen_instance_t *instance, float value);
/* Time's rim never commits: commit only shares the host's finish signature. */
void orb_app_time_rotary_finish(orb_screen_instance_t *instance, bool commit);
