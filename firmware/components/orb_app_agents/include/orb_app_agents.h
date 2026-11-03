#pragma once

#include "orb_model.h"
#include "orb_notification_history.h"
#include "orb_screen.h"

enum {
    ORB_APP_AGENTS_SCREEN = 3, ORB_APP_AGENTS_SESSION = 4,
    ORB_APP_AGENTS_CARD = 5, ORB_APP_AGENTS_DOT = 12,
    ORB_APP_AGENTS_IDLE = 16, ORB_APP_AGENTS_USAGE = 27,
    ORB_APP_AGENTS_SESSIONS = 28,
};

/* Projections borrow notification-owned receipts and snapshots for one call. */
typedef struct {
    const orb_model_snapshot_t *model;
    const orb_idle_page_t *idle;
    const orb_session_t *selection;
    const orb_notification_record_t *receipt;
    char session[ORB_MODEL_ID_MAX + 1], card[ORB_MODEL_CARD_ID_MAX + 1];
    unsigned page;
    bool selection_external, fixture, materialise, net_connected, link_online, interactive, outcome_live;
} orb_app_agents_model_t;

const orb_screen_descriptor_t *orb_app_agents_descriptor(orb_screen_id_t id);
bool orb_app_agents_screen(orb_screen_id_t id);
/* False requests the shell's existing deferred full-render path. */
bool orb_app_agents_refreshable(const orb_screen_instance_t *instance);
const char *orb_app_agents_session(const orb_screen_instance_t *instance);
unsigned orb_app_agents_focus_index(const orb_screen_instance_t *instance);
bool orb_app_agents_focus_change(orb_screen_instance_t *instance, int delta);
void orb_app_agents_select_focus(orb_screen_instance_t *instance, unsigned index);
unsigned orb_app_agents_usage_page(const orb_screen_instance_t *instance);
bool orb_app_agents_usage_change(orb_screen_instance_t *instance, unsigned page);
bool orb_app_agents_curved(const orb_screen_instance_t *instance);
/* The shell applies the saved anchor at its original render tail. */
bool orb_app_agents_restore_scroll(orb_screen_instance_t *instance, int *scroll);
bool orb_app_agents_session_token(const orb_screen_instance_t *instance,
    unsigned index, orb_launch_token_t *token);
/* Semantic input seam: command.action retains the compatible action number;
 * navigation and session-row tokens carry the actual presented source bounds. */
bool orb_app_agents_activate(orb_screen_instance_t *instance, int action,
    orb_input_origin_t origin, bool physical, const orb_launch_token_t *launch);
bool orb_app_agents_allow_synthetic(const orb_intent_t *intent);
