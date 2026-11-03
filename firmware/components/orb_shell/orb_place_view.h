#pragma once

#include "lvgl.h"
#include "orb_place_copy.h"

typedef struct {
    lv_obj_t *number, *ring, *icon, *caption;
    bool known;
} orb_charging_view_t;

/* Update the existing widgets together when live power telemetry changes. */
void orb_charging_view_update(orb_charging_view_t *view, int battery_pct,
                              orb_charging_copy_t state, lv_color_t colour);

/* The short arrival replaces the title in their shared top lane. */
void orb_place_title_lane(lv_obj_t *title, lv_obj_t *arrival);

/* Shell contrast policy runs after composition, including capture-only pages. */
void orb_place_increase_contrast(lv_obj_t *obj);

/* Unstyled event-bubbling layer used for workspace composition. */
lv_obj_t *orb_place_layer_create(lv_obj_t *parent);

/* Legacy system controls retain their separate contrast palette. */
void orb_system_increase_contrast(lv_obj_t *obj);

/* Identity hues follow the system catalogue or legacy contrast palette. */
lv_color_t orb_place_identity_accent(orb_delight_color_t color, bool system, bool contrast);
lv_color_t orb_place_agent_accent(orb_agent_t agent, bool system, bool contrast);
