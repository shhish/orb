#pragma once

#include "orb_shell.h"
#include "orb_faces.h"

typedef struct orb_shell_calm orb_shell_calm_t;
struct orb_workspace;
/* One allocation-free public summary feeds both the active and locked face.
 * It never carries a session name, card title or reminder text. */
bool orb_workspace_face_snapshot(const struct orb_workspace *workspace, orb_faces_snapshot_t *snapshot);
bool orb_shell_face_snapshot(const orb_shell_t *shell, orb_faces_snapshot_t *snapshot);
bool orb_shell_face_visible(const orb_shell_t *shell);
bool orb_shell_notice_allowed(const orb_shell_t *shell);
orb_shell_calm_t *orb_shell_calm_create(orb_shell_t *shell, lv_obj_t *root);
void orb_shell_calm_destroy(orb_shell_calm_t *calm);
void orb_shell_calm_status(orb_shell_calm_t *calm, const orb_shell_snapshot_t *status);
void orb_shell_calm_poll(orb_shell_calm_t *calm);
bool orb_shell_calm_button(orb_shell_calm_t *calm, const orb_button_event_t *event);
void orb_shell_calm_lock(orb_shell_calm_t *calm);
/* Urgent Time presentation wakes the panel without authorizing the private
 * task. Dismissal restores the existing lock; alert controls do not unlock. */
void orb_shell_calm_alert(orb_shell_calm_t *calm, bool presenting);
bool orb_shell_calm_dismiss_notice(orb_shell_calm_t *calm);
bool orb_shell_calm_notice_visible(const orb_shell_calm_t *calm);
orb_shell_calm_t *orb_shell_calm_context(const orb_shell_t *shell);
bool orb_shell_time_alert_active(const orb_shell_t *shell);
