#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ORB_SHELL_APP_ID_MAX 24

typedef struct {
    uint32_t revision;
    bool pending;
    char active[ORB_SHELL_APP_ID_MAX + 1];
    char requested[ORB_SHELL_APP_ID_MAX + 1];
    int32_t error; /* ESP_OK or the actual registry launch error. */
} orb_shell_app_state_t;

/* LVGL may enqueue a copied ID, but never invokes application callbacks.
 * NULL or an empty ID requests exit. A second pending request is rejected. */
bool orb_shell_app_request(const char *id);
void orb_shell_apps_snapshot(orb_shell_app_state_t *out);

/* Main-task only. Service before orb_apps_poll(), publish afterward so queued
 * lifecycle changes are reflected immediately. Neither takes the LVGL lock. */
void orb_shell_apps_service(void);
void orb_shell_apps_publish(void);
