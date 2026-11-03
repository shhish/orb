#pragma once

/* Shared by the detail's input/lifecycle half (orb_home_detail.c) and its
 * single accessory template (orb_home_detail_view.c). UI thread only. */
#include "orb_home_detail.h"
#include "orb_home_text.h"
#include "orb_home_motion.h"
#include "orb_rim.h"
#include "lvgl.h"

enum { ACTION_POWER, ACTION_MODE, ACTION_SCENE = 16, ACTION_MODE_SET = 64 };

/* One template for every accessory (HIG §§1/3/7): rim title on the 200px
 * baseline, an 80px identity disc, one value style, then captions in a
 * fixed order: context or "Last known", status, battery. */
#define DETAIL_DISC_TOP 100
#define DETAIL_DISC_CY (DETAIL_DISC_TOP + 40)

struct orb_home_detail {
    lv_obj_t *root, *content, *canvas, *name, *value, *caption, *status, *battery, *list;
    lv_obj_t *outcome;
    lv_obj_t *disc;      /* identity disc: power, mode entry, lock hold or passive */
    lv_obj_t *lock_disc; /* the disc while it owns the deliberate unlock hold */
    lv_color_t disc_fill;
    const char *disc_glyph;
    lv_timer_t *timer;
    const orb_home_commands_t *commands;
    orb_home_snapshot_t snapshot;
    orb_home_accessory_t accessory;
    char id[33], scene_focus_id[33];
    unsigned page, scene_offset;
    int32_t scroll_y[3];
    char requested_scene[33];
    orb_temperature_t temperature;
    bool system_navigation, system_touch, rotating;
    bool contrast, available, dragging, holding, hold_fired, suppress, colour_page, scene_acknowledged;
    bool physical_gesture, failure_visible, contact_moved;
    lv_indev_t *gesture_input;
    orb_home_wait_t wait;
    uint32_t hold_at, failure_at, press_at;
    float presented, target;
    orb_rim_drag_t rim_drag;
    int tick_index;
    lv_point_t press;
};

bool orb_home_detail_ready(const orb_home_snapshot_t *model);
bool orb_home_detail_has_dial(const orb_home_detail_t *v);
uint32_t orb_home_detail_field(const orb_home_detail_t *v);
float orb_home_detail_model_value(const orb_home_detail_t *v);
bool orb_home_detail_writable(const orb_home_detail_t *v);
orb_home_status_t orb_home_detail_status(const orb_home_detail_t *v);

/* Presentation (orb_home_detail_view.c). build() recreates the page;
 * refresh() updates words, colours and the template stack in place. */
void orb_home_detail_build(orb_home_detail_t *v);
void orb_home_detail_refresh(orb_home_detail_t *v, bool animate);
/* Clock-only update of a stale reading's age; idle polls change nothing else. */
void orb_home_detail_refresh_age(orb_home_detail_t *v);
void orb_home_detail_present(orb_home_detail_t *v, float value);
void orb_home_detail_show_number(orb_home_detail_t *v);
const char *orb_home_detail_lock_instruction(const orb_home_detail_t *v);
void orb_home_detail_clicked(lv_event_t *event);
void orb_home_detail_start_settle(orb_home_detail_t *v, float value);

bool orb_home_detail_animating(const orb_home_detail_t *v);
