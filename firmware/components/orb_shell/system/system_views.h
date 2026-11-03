#pragma once

#include "../orb_app_host.h"
#include "orb_shell.h"

enum { ORB_SYSTEM_CHECK_UPDATE = 457, ORB_SYSTEM_ONBOARD_DONE = 591 };

typedef struct { unsigned save_state; uint32_t wait_started; } orb_onboarding_model_t;
typedef struct {
    bool online, pending;
    uint32_t wait_started;
    char firmware_version[32], result[64];
} orb_updates_model_t;

const orb_screen_descriptor_t *orb_onboarding_descriptor(void);
const orb_screen_descriptor_t *orb_updates_descriptor(void);

typedef struct orb_system_views orb_system_views_t;
typedef enum {
    ORB_SYSTEM_VIEW_INPUT, ORB_SYSTEM_VIEW_PHYSICAL, ORB_SYSTEM_VIEW_DONE,
    ORB_SYSTEM_VIEW_CHECK, ORB_SYSTEM_VIEW_CHANGED,
} orb_system_view_event_t;

/* The service port is copied; reads and request acceptance retain their old
 * order. No filesystem work or pending operation enters a screen context. */
typedef struct {
    bool (*needed)(void);
    bool (*saving)(void);
    bool (*error)(void);
    bool (*dismiss)(void);
} orb_onboarding_port_t;

orb_system_views_t *orb_system_views_create(bool preview,
    bool (*event)(void *, orb_system_view_event_t), void *context);
void orb_system_views_destroy(orb_system_views_t *views);
void orb_system_views_onboarding_port(orb_system_views_t *views, const orb_onboarding_port_t *port);
bool orb_system_views_attach(orb_system_views_t *views, lv_obj_t *page,
    orb_shell_surface_t surface, const orb_shell_snapshot_t *status);
void orb_system_views_detach(orb_system_views_t *views, lv_obj_t *page);
orb_screen_instance_t *orb_system_views_current(orb_system_views_t *views, orb_shell_surface_t surface);
bool orb_system_views_update(orb_system_views_t *views,
    const orb_shell_snapshot_t *status, const orb_shell_snapshot_t *previous);
void orb_system_views_poll(orb_system_views_t *views, bool onboarding);
