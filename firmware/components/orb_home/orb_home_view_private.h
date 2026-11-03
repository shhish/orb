#pragma once

/* Shared by Home's navigator (orb_home_view.c) and its list pages
 * (orb_home_pages.c: Scenes, Climate and Room Sensors).
 * UI thread only; the view owns every object these builders create. */
#include "orb_home_view.h"
#include "orb_home_detail.h"
#include "orb_home_text.h"
#include "orb_kit.h"
#include "orb_home_motion.h"

#define SECONDARY ORB_KIT_SECONDARY
typedef struct {
    lv_obj_t *page, *climate_value, *climate_target, *room_list, *room_status, *outcome;
    lv_obj_t *room_rows[ORB_HOME_MAX_ROOMS];
    unsigned offset;
    bool room_devices, scene_list, sensor_list, landing;
    int rooms_rest; /* A fresh landing list's scroll; saved as 0, meaning "fresh". */
    char room_id[33];
    orb_home_page_t surface;
} home_page_t;

struct orb_home_view {
    lv_obj_t *root, *navigation;
    home_page_t current;
    orb_home_snapshot_t *model;
    orb_home_detail_t *detail;
    orb_home_detail_state_t detail_state;
    bool restore_detail;
    char detail_id[33];
    orb_home_transition_cb_t transition;
    void *transition_ctx;
    lv_area_t room_token, accessory_token, transition_token;
    bool transition_pending, transition_reverse;
    orb_home_commands_t commands;
    orb_temperature_t temperature;
    unsigned hour, detail_page;
    char room[33], selected[33];
    bool fixture;
    bool rebuilding, render_pending, contrast, press_moved;
    bool fresh_detail, arrival_armed, outcome_changed;
    uint16_t arrival_rows, arrival_started;
    int32_t arrival_scroll;
    bool sections, sensors, scene_page, system_navigation;
    unsigned scene_index;
    char room_semantic[ORB_HOME_MAX_ROOMS][40];
    int scene_scroll, sensor_scroll;
    orb_home_room_position_t room_positions[ORB_HOME_MAX_ROOMS];
    int rooms_scroll, room_scroll;
    orb_home_wait_t wait;
    orb_home_wait_t room_off_wait;
    orb_home_pending_wait_t pending_waits[ORB_HOME_MAX_PENDING];
    uint32_t scene_feedback_at, failure_at;
    bool failure_active;
    uint32_t press_at;
    lv_point_t press;
};

bool orb_home_ui_ready(const orb_home_snapshot_t *model);
lv_obj_t *orb_home_ui_text(lv_obj_t *p, const char *s, orb_text_style_t style, bool bold,
                           int x, int y, int width, int lines, lv_color_t color);
lv_obj_t *orb_home_ui_header(orb_home_view_t *v, const char *title);
lv_obj_t *orb_home_ui_list(orb_home_view_t *v, int top, int bottom);
void orb_home_ui_list_finish(lv_obj_t *list, int scroll);
void orb_home_ui_guard(lv_event_t *e);
void orb_home_ui_render(orb_home_view_t *v);
bool orb_home_ui_dragged(const orb_home_view_t *v);
/* Home's one empty/offline/loading composition, below the page's rim title. */
lv_obj_t *orb_home_ui_state(orb_home_view_t *v, const char *glyph, const char *title, const char *detail);
/* "Last known · Offline" while Home is stale, else "" (HIG §§1/8). */
const char *orb_home_ui_stale_header(const orb_home_view_t *v, char *out, size_t capacity);

void orb_home_pages_scenes(orb_home_view_t *v);
void orb_home_pages_climate(orb_home_view_t *v);
void orb_home_pages_readouts(orb_home_view_t *v);
void orb_home_pages_scenes_event(lv_event_t *e);

/* Retained room list construction and live projection. */
void orb_home_rooms_build(orb_home_view_t *v);
void orb_home_room_build(orb_home_view_t *v);
void orb_home_room_actions_build(orb_home_view_t *v);
void orb_home_rooms_summary(orb_home_view_t *v);
void orb_home_room_values_refresh(orb_home_view_t *v);
uint64_t orb_home_rooms_topology(const orb_home_snapshot_t *model);
int orb_home_ui_row_height(void);
lv_area_t orb_home_ui_token_area(orb_home_view_t *v, lv_obj_t *object);
lv_obj_t *orb_home_ui_button(orb_home_view_t *v, const char *caption, int x, int y,
                            bool primary, lv_event_cb_t cb, intptr_t data);

/* Arrival owns only a finite visual sequence; it never touches commands/model. */
void orb_home_motion_cancel(orb_home_view_t *v);
bool orb_home_room_is_on(const orb_home_view_t *v, unsigned room);
void orb_home_waits_sync(orb_home_view_t *v);
uint32_t orb_home_accessory_wait_start(const orb_home_view_t *v, const char *id);
uint32_t orb_home_room_wait_start(const orb_home_view_t *v, const char *id);
