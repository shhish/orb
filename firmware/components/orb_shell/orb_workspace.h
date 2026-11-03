#pragma once

#include "orb_shell.h"
#include "orb_model.h"
#include "orb_home.h"
#include "orb_screen.h"

typedef struct orb_workspace orb_workspace_t;
/* Special navigation requests return to the existing service/confirmation UI. */
typedef void (*orb_workspace_service_cb_t)(void *ctx, bool connection);
orb_workspace_t *orb_workspace_create(lv_obj_t *parent, orb_shell_action_cb_t action,
                                     orb_workspace_service_cb_t service, void *ctx);
void orb_workspace_destroy(orb_workspace_t *w);
void orb_workspace_show(orb_workspace_t *w, bool show);
void orb_workspace_update(orb_workspace_t *w, const orb_shell_snapshot_t *snapshot);
void orb_workspace_poll(orb_workspace_t *w);
void orb_workspace_navigate(orb_workspace_t *w, orb_shell_surface_t surface);
orb_shell_surface_t orb_workspace_surface(const orb_workspace_t *w);
/* Settled/current place used by the recognizer, independent of a pending target. */
orb_shell_surface_t orb_workspace_nav_current(const orb_workspace_t *w);
void orb_workspace_session(orb_workspace_t *w, const char *id);
void orb_workspace_card(orb_workspace_t *w, const char *id);
void orb_workspace_talk_origin(orb_workspace_t *w, bool synthetic);
void orb_workspace_talk(orb_workspace_t *w, bool held);
bool orb_workspace_volume_control_visible(const orb_workspace_t *w);
int orb_workspace_brightness(const orb_workspace_t *w);
void orb_workspace_open_metrics(const orb_workspace_t *w, orb_shell_open_metrics_t *out);
void orb_workspace_back(orb_workspace_t *w);
/* Face editors unwind locally before BOOT's root-Face Apps fallback. */
bool orb_workspace_face_local(const orb_workspace_t *w);
bool orb_workspace_twist(orb_workspace_t *w, int detents);
void orb_workspace_set_silent(orb_workspace_t *w, bool silent);
bool orb_workspace_dismiss_banner(orb_workspace_t *w);
bool orb_workspace_nav_bench(orb_workspace_t *w, const char *gesture, uint32_t elapsed_ms, bool finish);

void orb_workspace_environment(orb_workspace_t *w, float x, float y, float z, bool always_on);
void orb_workspace_display_active(orb_workspace_t *w, bool active);
/* A modal owns visible motion while retaining the task's input/navigation state. */
void orb_workspace_covered(orb_workspace_t *w, bool covered);

/* Private developer scenes use the real renderer without changing live stores. */
orb_workspace_t *orb_workspace_create_fixture(lv_obj_t *parent, const orb_model_snapshot_t *model,
                                             const orb_home_snapshot_t *home);
lv_obj_t *orb_workspace_fixture_scroll(orb_workspace_t *w);
/* A private fixture's Home screen, for its motion measurements. */
orb_screen_instance_t *orb_workspace_fixture_home(const orb_workspace_t *w);

bool orb_workspace_increase_contrast(const orb_workspace_t *w);
bool orb_workspace_speech_agent(const orb_workspace_t *w, orb_agent_t *agent);
void orb_workspace_fixture_notification_age(orb_workspace_t *w, const char *id, unsigned age_ms);
