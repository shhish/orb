#pragma once

#include "system_scaffold.h"
#include "../orb_app_host.h"

enum { ORB_BOOT_SCREEN_ID = 0xff05, ORB_BOOT_OPEN_CONNECTION = 1 };

const orb_screen_descriptor_t *orb_boot_descriptor(void);
orb_app_host_t *orb_boot_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context);
bool orb_boot_host_ensure(orb_app_host_t *host, lv_obj_t *parent, orb_system_scaffold_t *scaffold);
/* The shell selects precedence and supplies each full-render opportunity.
 * Status is call-scoped; no setup code or service snapshot is retained. */
/* A refused framework reservation still composes the original allocation-free
 * scaffold. This is the same composer, without instance actions or context. */
void orb_boot_screen_content(orb_screen_instance_t *instance, orb_system_scaffold_t *scaffold,
    view_t view, const orb_shell_snapshot_t *status);
void orb_boot_screen_layout(orb_system_scaffold_t *scaffold, view_t view);
void orb_boot_screen_activate(orb_screen_instance_t *instance);
