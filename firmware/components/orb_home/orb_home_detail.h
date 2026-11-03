#pragma once

#include "orb_home_command.h"
#include "lvgl.h"

typedef struct orb_home_detail orb_home_detail_t;
typedef struct {
    unsigned page, scene_offset;
    int32_t scroll_y[3];
    char scene_id[33];
} orb_home_detail_state_t;

/* UI-thread only. BOOT supplies Back; dials use a continuous gauge.
 * Deleting the root also frees the view and pauses its timer. */
/* commands is the parent view's port; it outlives this detail. */
orb_home_detail_t *orb_home_detail_create(lv_obj_t *parent, const char *id, const orb_home_commands_t *commands);
void orb_home_detail_destroy(orb_home_detail_t *view);
lv_obj_t *orb_home_detail_root(const orb_home_detail_t *view);
void orb_home_detail_poll(orb_home_detail_t *view);
/* Light: 0 brightness, 1 colour/white, 2 scenes. Thermostat: 1 mode picker. */
void orb_home_detail_page(orb_home_detail_t *view, unsigned page);
unsigned orb_home_detail_page_index(const orb_home_detail_t *view);
void orb_home_detail_save(const orb_home_detail_t *view, orb_home_detail_state_t *state);
void orb_home_detail_restore(orb_home_detail_t *view, const orb_home_detail_state_t *state);
/* The visible detail list owns the rim while a mode/scene page is open. */
lv_obj_t *orb_home_detail_scroll(const orb_home_detail_t *view);
/* Accessibility/environment delivered by the shell; no board I/O here. */
void orb_home_detail_set_contrast(orb_home_detail_t *view, bool increased);
void orb_home_detail_system_navigation(orb_home_detail_t *view, bool enabled);
bool orb_home_detail_dial(orb_home_detail_t *view, float *value, float *step);
void orb_home_detail_rotate(orb_home_detail_t *view, float value);
void orb_home_detail_rotary_finish(orb_home_detail_t *view, bool commit);

/* Fresh forward detail only; a restored/back detail is already final. */
void orb_home_detail_arrival(orb_home_detail_t *view);
