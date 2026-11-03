#pragma once

#include "orb_app_host.h"
#include "orb_app_time.h"

/* Time on the shared single-route host. Time's presenter still calls its own
 * service setters; the host owns lifetime, value context and preview copies. */
orb_app_host_t *orb_time_host_create(bool preview);
/* Restores any saved context, then lets a ringing alarm or due reminder take
 * the dial, exactly as the workspace's original construction did. */
/* Replaces *host with a fresh, context-free Time screen on the shell's alert
 * layer for a ringing alarm, due reminder or finished timer; the presenter
 * picks that page itself. False (and NULL) when it cannot be admitted. */
bool orb_time_host_present(orb_app_host_t **host, lv_obj_t *layer);
bool orb_time_host_attach(orb_app_host_t *host, lv_obj_t *page, const orb_app_time_model_t *model,
    bool alarm_ringing, bool reminder_due);
