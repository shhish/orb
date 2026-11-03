#pragma once

#include "orb_app_agents.h"

typedef struct orb_agents_host orb_agents_host_t;

typedef struct {
    void *context;
    /* The workspace retains its snapshot generation for this synchronous call.
     * The host never copies a complete model or borrows another workspace's. */
    bool (*project)(void *context, orb_screen_id_t screen, orb_app_agents_model_t *model);
    /* The copied value enters existing shell policy. App callbacks finish
     * all state access first: completing a pending Back can destroy them. */
    bool (*accept)(void *context, const orb_intent_t *intent);
    void (*synthetic_refused)(void *context, const orb_intent_t *intent);
} orb_agents_host_config_t;

orb_agents_host_t *orb_agents_host_create(bool preview, const orb_agents_host_config_t *config);
void orb_agents_host_destroy(orb_agents_host_t *host);
orb_screen_instance_t *orb_agents_host_current(orb_agents_host_t *host);
bool orb_agents_host_attach(orb_agents_host_t *host, lv_obj_t *page, orb_screen_id_t screen);
bool orb_agents_host_detach_page(orb_agents_host_t *host, lv_obj_t *page);
/* False asks the shell for its existing deferred full render. */
bool orb_agents_host_refresh(orb_agents_host_t *host);
/* Both hosts keep their own workspace callbacks. Only opaque value contexts
 * are copied; no projection, pointer, command or presentation is replayed. */
bool orb_agents_host_clone_contexts(orb_agents_host_t *destination, orb_agents_host_t *source);
