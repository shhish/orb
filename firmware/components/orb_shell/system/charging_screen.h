#pragma once

#include "../orb_app_host.h"
#include "orb_shell.h"

typedef struct {
    int battery_pct;
    bool usb, charging, contrast;
} orb_charging_model_t;

orb_charging_model_t orb_charging_project(const orb_shell_snapshot_t *status, bool contrast);
const orb_screen_descriptor_t *orb_charging_descriptor(void);
orb_app_host_t *orb_charging_host_create(bool preview);
bool orb_charging_host_attach(orb_app_host_t *host, lv_obj_t *page, const orb_charging_model_t *model);
bool orb_charging_host_update(orb_app_host_t *host, const orb_charging_model_t *model);
