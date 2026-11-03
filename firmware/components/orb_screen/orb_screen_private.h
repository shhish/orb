#pragma once

#include "orb_app.h"

typedef struct orb_screen_payload {
    struct orb_screen_payload *next;
    size_t bytes;
    max_align_t alignment;
    unsigned char data[];
} orb_screen_payload_t;
typedef struct {
    orb_screen_resource_kind_t kind;
    void *resource;
    orb_screen_cleanup_cb_t cleanup;
    orb_screen_pause_cb_t pause;
} orb_screen_resource_t;
typedef struct {
    orb_screen_instance_t *handle;
    orb_screen_host_t *host;
    const orb_screen_descriptor_t *descriptor;
    lv_obj_t *root, *viewport, *title;
    void *state;
    orb_screen_context_t *context;
    orb_screen_payload_t *payloads;
    orb_screen_resource_t resources[ORB_SCREEN_RESOURCE_LIMIT];
    orb_model_revisions_t revisions;
    uint64_t environment_revision;
    size_t payload_used, payload_peak;
    unsigned silent;
    bool updated, preview, destroying, create_entered, rim_callback;
    orb_input_origin_t origin;
    bool physical;
    orb_visibility_t visibility;
} orb_screen_data_t;

static inline orb_screen_data_t *orb_screen_data(const orb_screen_instance_t *instance)
{
    return instance ? instance->framework : NULL;
}
void orb_screen_scope_enter(orb_screen_data_t *);
void orb_screen_scope_leave(orb_screen_data_t *);
bool orb_screen_ui_admitted(const orb_screen_data_t *);
void orb_screen_release_resources(orb_screen_data_t *);
void orb_screen_pause_resources(orb_screen_data_t *, bool);
void orb_screen_rim_callback(orb_screen_data_t *, bool begin, bool finish, bool commit);

void orb_screen_page_visibility(orb_screen_data_t *);
void orb_screen_page_cleanup(orb_screen_data_t *);
