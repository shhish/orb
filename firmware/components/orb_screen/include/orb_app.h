#pragma once

#include "orb_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_APP_LIMIT 16
#define ORB_SCREEN_LIMIT ORB_NAV_MAX_PLACES

typedef struct {
    uint16_t id;
    const char *name;
    const orb_screen_descriptor_t *const *screens;
    size_t screen_count;
    orb_screen_id_t entry;
} orb_app_manifest_t;
typedef struct {
    const orb_app_manifest_t *apps[ORB_APP_LIMIT];
    const orb_screen_descriptor_t *screens[ORB_SCREEN_LIMIT];
    size_t app_count, screen_count;
} orb_screen_registry_t;
void orb_screen_registry_init(orb_screen_registry_t *);
bool orb_screen_registry_add(orb_screen_registry_t *, const orb_app_manifest_t *, const char **reason);
const orb_screen_descriptor_t *orb_screen_registry_find(const orb_screen_registry_t *, orb_screen_id_t);

typedef struct {
    orb_screen_id_t id;
    uint64_t entity_id;
    uint32_t page;
    orb_screen_context_t *context;
    orb_memory_owner_t owner;
} orb_context_entry_t;
typedef struct { orb_context_entry_t entries[ORB_SCREEN_CONTEXT_LIMIT]; } orb_context_store_t;
void orb_context_store_init(orb_context_store_t *);
void orb_context_store_clear(orb_context_store_t *);
bool orb_context_store_save(orb_context_store_t *, const orb_screen_instance_t *, uint64_t entity_id);
bool orb_context_store_restore(const orb_context_store_t *, orb_screen_instance_t *, uint64_t entity_id);
void orb_context_store_remove(orb_context_store_t *, orb_screen_id_t, uint64_t entity_id);
/* Router contexts distinguish explicit entity/page route arguments. The simpler
 * entity-only helpers above use page zero for adapters without subroutes. */
bool orb_context_store_save_route(orb_context_store_t *, const orb_screen_instance_t *, const orb_screen_args_t *);
bool orb_context_store_restore_route(const orb_context_store_t *, orb_screen_instance_t *, const orb_screen_args_t *);

typedef struct {
    orb_screen_id_t id;
    orb_screen_args_t args;
    orb_launch_token_t launch;
} orb_route_entry_t;
typedef struct {
    orb_screen_registry_t *registry;
    orb_screen_host_t *host;
    lv_obj_t *parent;
    orb_screen_instance_t instances[2];
    orb_screen_instance_t *current, *staged;
    lv_obj_t *staged_parent;
    orb_route_entry_t staged_route;
    orb_context_store_t contexts;
    orb_route_entry_t route, history[ORB_NAV_BACK_CAPACITY];
    size_t history_count;
    bool initialized;
} orb_router_t;
bool orb_router_init(orb_router_t *, orb_screen_registry_t *, orb_screen_host_t *,
                     lv_obj_t *parent, orb_screen_id_t initial, const orb_screen_args_t *);
void orb_router_destroy(orb_router_t *);
bool orb_router_open(orb_router_t *, orb_screen_id_t, const orb_screen_args_t *,
                     const orb_launch_token_t *, bool committed);
orb_back_result_t orb_router_back(orb_router_t *, bool committed);

/* Presentation transaction for a compatibility host that retains outer history
 * and cues. The provisional parent is borrowed and must outlive stage/cancel.
 * Stage does not change current, navigation or saved contexts. Commit reparents
 * body/chrome, closes the source using its reserved context and emits no cue.
 * A false return leaves the current instance usable. Instance handles remain
 * in the router's two fixed slots; callers must never copy/move the router. */
bool orb_router_stage(orb_router_t *, orb_screen_registry_t *, orb_screen_host_t *,
    lv_obj_t *provisional_parent, orb_screen_id_t, const orb_screen_args_t *, bool restore_context);
bool orb_router_commit_staged(orb_router_t *, lv_obj_t *canonical_parent);
void orb_router_cancel_staged(orb_router_t *);
orb_screen_instance_t *orb_router_staged(orb_router_t *);

/* Read-only admission preflight. Validates context capacity and saves into the
 * source's already-reserved scratch record; no allocation, route change or
 * destruction occurs. An empty/departed router can always leave. */
bool orb_router_can_depart(orb_router_t *);

/* Transfer the instance's already-reserved context when closing a page. No
 * allocation is needed; failure leaves the instance and prior store usable. */
bool orb_context_store_close_route(orb_context_store_t *, orb_screen_instance_t *, const orb_screen_args_t *);
bool orb_context_store_copy(orb_context_store_t *, const orb_context_store_t *);

#ifdef __cplusplus
}
#endif

