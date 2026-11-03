#pragma once

#include "orb_app_host.h"

enum { ORB_APPS_TIME = 480, ORB_APPS_FOCUS = 720, ORB_APPS_CANCEL = 721 };
typedef struct orb_apps_view orb_apps_view_t;

const orb_screen_descriptor_t *orb_apps_descriptor(void);
bool orb_apps_screen_build(orb_screen_instance_t *instance, bool progressive, bool time_available);
/* Match the frozen destination pose to the live post-transition arrival.
 * Back/restore uses false, and a contact since creation suppresses re-arming. */
bool orb_apps_arrival_prepare(orb_screen_instance_t *instance, bool fresh);
orb_apps_view_t *orb_apps_view_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *),
    bool (*active)(void *), void *context);
void orb_apps_view_destroy(orb_apps_view_t *view);
size_t orb_apps_view_size(void);
/* A build opportunity belongs to the shell's existing refresh schedule. Model
 * updates never advance it; a progressive opportunity creates exactly one disc. */
bool orb_apps_view_build(orb_apps_view_t *view, lv_obj_t *page, bool progressive, bool time_available);
void orb_apps_view_detach(orb_apps_view_t *view, lv_obj_t *page);
orb_screen_instance_t *orb_apps_view_current(orb_apps_view_t *view);
lv_obj_t *orb_apps_view_caption(orb_apps_view_t *view);
int orb_apps_view_focus(orb_apps_view_t *view);
void orb_apps_view_select(orb_apps_view_t *view, int focused);
/* Copy only the schema-checked focus context, never controls or commands. */
bool orb_apps_view_copy(orb_apps_view_t *destination, orb_apps_view_t *source);
void orb_apps_prepare_launch(orb_screen_instance_t *instance, unsigned action);
