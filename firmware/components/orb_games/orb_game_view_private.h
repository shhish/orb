#pragma once

/* Private to orb_games: the game view's state, shared by its renderer
 * (orb_game_view.c) and its reading-surface templates (orb_game_chrome.c). */
#include "orb_game_view.h"
#include "orb_game_play.h"
#include "orb_game_native.h"
#include "orb_game_pop.h"
#include "orb_game_orbit.h"
#include "orb_game_beads_draw.h"
#include "orb_ui.h"
#include "orb_kit.h"

#define TAU 6.28318530718f
/* Shared reading-surface geometry (review #28–#33). The start artwork stays
 * at one height for every Dynamic Type size: its 32 px clearance from the
 * toolbar X never shrinks, and the single-line tutorial always fits below. */
#define ART_Y 172
#define TUTORIAL_Y 305
#define START_PRIMARY_Y 380
#define HERO_Y 171
#define RESULT_LINE_Y 247
#define RESULT_PRIMARY_Y 316
#define RESULT_DONE_Y 394
#define CONTROL_GAP 20 /* HIG §3: 20 px between bezelled controls. */
#define STATUS_MS 3000u /* In-play status, then it lives on Pause. */
#define TITLE_CLEAR 192 /* Rim-title ink starts at r200 at every size. */
#define PREVIEW_SCALE (180.0f / 466.0f) /* The whole field in the art disc. */

typedef enum { SCENE_START, SCENE_PLAY, SCENE_PAUSE, SCENE_END,
               SCENE_CLEAR, SCENE_GARDEN } game_scene_t;

typedef orb_game_orbit_ring_row_t ring_row_t;
typedef orb_game_orbit_ring_t ring_cache_t;

struct orb_game_view {
    lv_obj_t *root, *canvas, *title, *detail, *best, *hud_title;
    lv_obj_t *close, *settings, *primary, *finish, *garden;
    lv_obj_t *controls_track, *settings_list, *settings_label, *settings_icon, *art, *stage_dots, *timer_ring;
    lv_obj_t *arrows[4], *score_hit, *score_clip, *score, *old_score;
    lv_obj_t *big_score, *done, *replay, *bonus, *hud_time, *life[3];
    orb_game_kind_t kind;
    orb_game_view_action_cb_t callback;
    void *context;
    orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES], next[ORB_GAME_MAX_SPRITES];
    lv_area_t gate_areas[ORB_MARBLE_GATES_MAX * ORB_MARBLE_GATE_SEGMENTS];
    lv_area_t chrome_damage;
    bool chrome_dirty;
    ring_cache_t *rings[3];
    orb_game_pop_coverage_t pop_coverage[ORB_GAME_POP_COVERAGES];
    orb_game_orbit_t *orbit;
    bool serial_orbit;
    orb_game_native_beads_t bead_masks;
    orb_game_beads_draw_t *beads_draw;
    uint16_t bead_batch_count;
    bool bead_batch_eligible;
    orb_game_state_t preview;
    size_t count;
    uint32_t score_value, best_value, scene_at, roll_at;
    uint8_t lives;
    game_scene_t scene;
    bool first, save_error, controls, repaint_chrome;
    orb_content_size_t content_size;
    bool bold, reduced, opaque;
    uint8_t stage, keepsakes;
    float rim_angle, rim_travel;
    float contact_x, contact_y;
    uint32_t contact_at;
    bool rim_contact;
    int32_t pop_arc; /* Pop rim timer remaining arc, in degrees. */
    bool rewinding, reward_settled;
    bool field_held, field_long_contact;
    bool tilt_calibrating, motion_unavailable, time_valid;
    const char *status; /* Current in-play condition, or NULL. */
    uint32_t status_at;
    int32_t pop_seconds;
    char score_text[24], old_score_text[24], best_text[48], detail_text[64];
    char title_main[40], title_suffix[40], time_text[12];
    char big_score_text[16];
    char bonus_text[16];
    uint16_t bonus_value;

    orb_game_view_stats_t stats;
};


/* Shared helpers (orb_game_chrome.c). */
void game_compact(uint32_t value, char *out, size_t capacity);
void game_visible(lv_obj_t *object, bool show);
void game_centre_text(lv_obj_t *label, int y);
void game_place(lv_obj_t *obj, int x, int y);
bool game_tilt(orb_game_kind_t kind);
lv_color_t game_accent(orb_game_kind_t kind);
void game_hud_create(orb_game_view_t *v);
/* Scene chrome after a scene, preference or content change; never per frame. */
void game_chrome(orb_game_view_t *v, const orb_game_state_t *state, bool save_error);
/* Every update: calibration overlay, transient condition, Marble's rim HUD. */
void game_play_overlay(orb_game_view_t *v, const orb_game_state_t *state, uint32_t now, bool force);
/* orb_game_view.c: replace an action's caption at an idle scene boundary. */
void game_action_content(orb_game_view_t *v, lv_obj_t **slot, const char *caption, int action, bool primary);

/* Content drawing shares geometry with the view without owning scene state. */
void game_sprite_disc(lv_layer_t *layer, int x, int y, int radius, uint32_t colour,
                      uint8_t opacity, int border);
void game_sprite_draw(lv_layer_t *layer, const orb_game_sprite_t *sprite);
