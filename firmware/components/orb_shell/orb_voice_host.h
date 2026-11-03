#pragma once

#include "orb_app_voice.h"

typedef struct orb_voice_host orb_voice_host_t;

typedef struct {
    void *context;
    /* The workspace retains its snapshot generation for this synchronous call.
     * The host never copies a complete model or borrows another workspace's. */
    bool (*project)(void *context, orb_screen_id_t screen, orb_app_voice_model_t *model);
    /* The copied value enters existing shell policy. App callbacks finish
     * all state access first: completing a pending Back can destroy them. */
    bool (*accept)(void *context, const orb_intent_t *intent);
    void (*synthetic_refused)(void *context, const orb_intent_t *intent);
} orb_voice_host_config_t;

orb_voice_host_t *orb_voice_host_create(bool preview, const orb_voice_host_config_t *config);
void orb_voice_host_destroy(orb_voice_host_t *host);
orb_screen_instance_t *orb_voice_host_current(orb_voice_host_t *host);
bool orb_voice_host_attach(orb_voice_host_t *host, lv_obj_t *page, orb_screen_id_t screen);
bool orb_voice_host_detach_page(orb_voice_host_t *host, lv_obj_t *page);
void orb_voice_host_meter(orb_voice_host_t *host);
