#pragma once

#include "orb_app_host.h"
#include "orb_workspace_snapshot.h"
#include "orb_route_context.h"
#include "orb_shell.h"
#include "apps_screen.h"
#include "orb_time.h"

/* A capture-only scene owns one registered descriptor and its opaque context.
 * Source values are consumed synchronously; retained COW generations keep the
 * scene independent of later live polling or source destruction. */
typedef struct orb_neighbour orb_neighbour_t;
typedef struct {
    const workspace_snapshots_t *snapshots;
    orb_app_host_t *context;
    /* Source cache attribution is sampled only after scene allocation. */
    void (*prepare)(void *);
    void *prepare_context;
    const orb_shell_snapshot_t *status;
    const orb_route_context_t *place;
    orb_apps_view_t *apps;
    const orb_time_snapshot_t *time;
    const char *card;
    unsigned hour;
    bool contrast, alarm_ringing, reminder_due, silent, control_volume, home_fresh_entry;
} orb_neighbour_source_t;

bool orb_neighbour_supported(orb_screen_id_t route);
orb_neighbour_t *orb_neighbour_create(lv_obj_t *parent, orb_screen_id_t route,
    const orb_neighbour_source_t *source);
lv_obj_t *orb_neighbour_page(const orb_neighbour_t *scene);
orb_screen_instance_t *orb_neighbour_screen(const orb_neighbour_t *scene);
/* Run after the shell's contrast pass, before freezing the capture. */
void orb_neighbour_settle(orb_neighbour_t *scene);
void orb_neighbour_destroy(orb_neighbour_t *scene);
