#pragma once

#include "orb_model.h"
#include "orb_screen.h"

enum { ORB_APP_VOICE_SCREEN = 13, ORB_APP_VOICE_TOUCH_START = 0xff10,
    ORB_APP_VOICE_TOUCH_RELEASE = 0xff11 };
typedef struct orb_voice_flow orb_voice_flow_t;

/* Snapshot and selection are borrowed from the shell's COW generation for one
 * callback. Flow has workspace lifetime, independent of the review page. */
typedef struct {
    const orb_model_snapshot_t *model;
    const orb_session_t *selection;
    orb_voice_flow_t *flow;
    const char *notice;
    bool selection_external, fixture, materialise, interactive, touch_allowed, outcome_live;
    lv_color_t capture_colour;
} orb_app_voice_model_t;

typedef struct {
    void *context;
    bool (*physical)(void *context);
    bool (*request)(void *context, orb_voice_action_t action);
    void (*notice)(void *context, const char *text);
} orb_voice_command_port_t;

orb_voice_flow_t *orb_voice_flow_create(void);
void orb_voice_flow_destroy(orb_voice_flow_t *flow);
size_t orb_voice_flow_bytes(void);
bool orb_voice_flow_pending(const orb_voice_flow_t *flow);
bool orb_voice_flow_held(const orb_voice_flow_t *flow);
uint32_t orb_voice_flow_sent_at(const orb_voice_flow_t *flow);
void orb_voice_flow_poll(orb_voice_flow_t *flow, orb_voice_phase_t phase);
void orb_voice_flow_depart(orb_voice_flow_t *flow);
void orb_voice_flow_retry(orb_voice_flow_t *flow);
void orb_voice_flow_sent(orb_voice_flow_t *flow, uint32_t now);
void orb_voice_flow_touch(orb_voice_flow_t *flow, bool held);
void orb_voice_flow_boot(orb_voice_flow_t *flow);
bool orb_voice_flow_command(orb_voice_flow_t *flow, const orb_app_voice_model_t *model,
    orb_voice_action_t action, const orb_voice_command_port_t *port);
bool orb_app_voice_supported(const orb_app_voice_model_t *model, const char *target, const char *card);
bool orb_app_voice_speech_agent(const orb_model_snapshot_t *model, const orb_voice_state_t *voice, orb_agent_t *agent);
const char *orb_voice_review_blocked(bool ready, bool available, bool session, const char *target, bool text);

const orb_screen_descriptor_t *orb_app_voice_descriptor(void);
void orb_app_voice_meter(orb_screen_instance_t *instance);
bool orb_app_voice_restore_scroll(orb_screen_instance_t *instance, int *scroll);
bool orb_app_voice_activate(orb_screen_instance_t *instance, unsigned action,
    orb_input_origin_t origin, bool physical);
bool orb_app_voice_allow_synthetic(const orb_intent_t *intent);
