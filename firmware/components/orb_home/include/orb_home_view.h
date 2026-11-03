#pragma once
#include "lvgl.h"
#include "orb_home_command.h"
#include "orb_home_text.h"
typedef struct orb_home_view orb_home_view_t;
typedef enum { ORB_HOME_ROOMS, ORB_HOME_CLIMATE, ORB_HOME_ROOM } orb_home_page_t;
/* The embedding navigator captures the source before mutation, then the
 * settled destination after deferred rendering. Coordinates are root-local. */
typedef void (*orb_home_transition_cb_t)(void *ctx, const lv_area_t *token, bool reverse, bool before);
void orb_home_view_set_transition(orb_home_view_t *view, orb_home_transition_cb_t transition, void *ctx);
/* UI-thread owned. No network calls, timers are paused when at rest. Writes
 * leave through the copied command port; NULL calls the service directly. */
orb_home_view_t *orb_home_view_create(lv_obj_t *parent, const orb_home_commands_t *commands);
void orb_home_view_destroy(orb_home_view_t *view);
void orb_home_view_poll(orb_home_view_t *view);
void orb_home_view_select(orb_home_view_t *view, orb_home_page_t page);
void orb_home_view_open(orb_home_view_t *view, const char *id);
void orb_home_view_room(orb_home_view_t *view, const char *id);
void orb_home_view_detail_page(orb_home_view_t *view, unsigned page);
void orb_home_view_set_contrast(orb_home_view_t *view, bool increased);
void orb_home_view_hour(orb_home_view_t *view, unsigned hour);
lv_obj_t *orb_home_view_root(orb_home_view_t *view);
/* Shared small, plain content discs; actual light hue is semantic content. */
lv_color_t orb_home_accessory_colour(const orb_home_accessory_t *accessory, bool online);
/* Disc fill: the active colour, or a visible neutral (grey 4) when inactive. */
lv_color_t orb_home_disc_colour(const orb_home_accessory_t *accessory, bool online);
/* Framework7 pack glyph for the accessory's identity (state only fills it). */
const char *orb_home_accessory_icon(const orb_home_accessory_t *accessory);
const char *orb_home_room_icon(const char *name);
/* One font identity disc for detail and Control Center, with adaptive ink. */
lv_obj_t *orb_home_identity_disc(lv_obj_t *parent, int kind, const orb_home_accessory_t *accessory,
                                 lv_color_t fill);
/* Returned substring retains the canonical name in the model. Trim only when
 * the room is already visible or the full name would wrap. */
const char *orb_home_accessory_name(const orb_home_snapshot_t *snapshot,
                                  const orb_home_accessory_t *accessory, bool trim_room);
lv_color_t orb_home_light_colour(const orb_home_state_t *state);

/* Explicit private fixture; no polling of the live Home store. */
orb_home_view_t *orb_home_view_create_fixture(lv_obj_t *parent, const orb_home_snapshot_t *snapshot);
/* Edge contacts belong to orb_nav. Native Home lists own scroll; only
 * adjustable accessory details register a normalized value owner. */
void orb_home_view_system_navigation(orb_home_view_t *view, bool enabled);
void orb_home_view_rotary(orb_home_view_t *view, float delta);
bool orb_home_view_dial(orb_home_view_t *view, float *value, float *step);
void orb_home_view_rotate(orb_home_view_t *view, float value);
/* True only for an adjustable accessory detail. */
bool orb_home_view_dial_writes(const orb_home_view_t *view);
void orb_home_view_rotary_finish(orb_home_view_t *view, bool commit);
/* Consume a nested Home return, or false when the shell should leave Home. */
bool orb_home_view_back(orb_home_view_t *view);
bool orb_home_view_can_back(const orb_home_view_t *view);
/* Deferred navigation is moving until the destination is rendered. */
bool orb_home_view_moving(const orb_home_view_t *view);
lv_obj_t *orb_home_view_scroll(orb_home_view_t *view);

/* UI-only navigation context for ordinary return and developer measurements. No model
 * state or pending Home write is replayed when the presentation is restored. */
typedef struct {
    char id[33];
    int scroll;
} orb_home_room_position_t;

typedef struct {
    orb_home_page_t surface;
    unsigned offset, detail_page, scene_index;
    /* Landing: 0 = fresh/resting, -1 = native zero (first row at equator). */
    int rooms_scroll, room_scroll, scroll;
    int scene_scroll, sensor_scroll;
    int32_t detail_scroll_y[3];
    unsigned detail_scene_offset;
    char detail_scene_id[33];
    lv_area_t room_token, accessory_token;
    orb_home_room_position_t room_positions[ORB_HOME_MAX_ROOMS];
    bool sections, sensors, scene_page;
    char room[33], selected[33];
} orb_home_view_state_t;
void orb_home_view_save(const orb_home_view_t *view, orb_home_view_state_t *state);
void orb_home_view_restore(orb_home_view_t *view, const orb_home_view_state_t *state);

/* Arm after fresh entry is laid out, before the destination snapshot. Back,
 * restore and rebuild never call this. The navigation host completes it. */
void orb_home_view_arrival_prepare(orb_home_view_t *view);
void orb_home_view_arrival_complete(orb_home_view_t *view, bool fresh);

/* Capture-only start pixels, with no timer/lifecycle admission. */
void orb_home_view_arrival_snapshot(orb_home_view_t *view);
