#pragma once

#include "orb_app_host.h"
#include "orb_stack.h"
#include "orb_shell.h"

/* Call-scoped immutable read views of the owning workspace's COW generations.
 * Nothing here is serialized or retained across a create/refresh operation. */
typedef struct {
    const orb_model_snapshot_t *model;
    const orb_home_snapshot_t *home;
    const orb_time_snapshot_t *time;
    const char *clock_time, *clock_date, *card, *notice;
    unsigned page_index;
    int clock_hour;
    bool stack_surface, compact_time;
} orb_stack_read_t;
typedef struct {
    bool (*read)(void *context, orb_stack_read_t *read);
    void *context;
} orb_stack_source_t;
typedef enum {
    ORB_STACK_REFRESH_TIME, ORB_STACK_REFRESH_CLOCK, ORB_STACK_REFRESH_HOME, ORB_STACK_REFRESH_USAGE,
} orb_stack_refresh_t;

/* The returned projection borrows all inputs for this call only. */
orb_stack_read_t orb_stack_project(const orb_model_snapshot_t *model, const orb_home_snapshot_t *home,
    const orb_time_snapshot_t *time, const orb_shell_snapshot_t *status, const char *card,
    const char *notice, unsigned page_index, orb_shell_surface_t surface);
const orb_screen_descriptor_t *orb_stack_descriptor(void);
unsigned orb_stack_screen_entries(const orb_stack_read_t *read, orb_stack_entry_t *entries);
const orb_home_room_t *orb_stack_home_room(const orb_home_snapshot_t *home);
orb_app_host_t *orb_stack_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context);
bool orb_stack_host_attach(orb_app_host_t *host, lv_obj_t *page, const orb_stack_source_t *source);
bool orb_stack_screen_has_metrics(orb_screen_instance_t *instance);
/* False asks for the shell's existing deferred full render at the Usage
 * reset-line threshold; it never deletes a live reader while input unwinds. */
bool orb_stack_screen_refresh(orb_screen_instance_t *instance, orb_stack_refresh_t refresh);
void orb_stack_prepare_launch(orb_screen_instance_t *instance, unsigned action);
