#pragma once

#include "lvgl.h"
#include "orb_game_logic.h"

/* Design 1.0 Play picker: one centred game at a time with a miniature of its
 * arena, name, progress line, a tinted Play button and five page dots. The two
 * neighbours peek at the left and right edges; a swipe or rim turn changes the
 * selected game. The view never launches a game itself: it reports the chosen
 * kind through the callback, and the shell/runtime owns the request. */
typedef struct orb_game_play_view orb_game_play_view_t;
/* Shared Start/picker artwork: a fixed-seed real pose, prepared while idle.
 * This disposable preview never enters the running game's input path. */
void orb_game_preview(orb_game_state_t *state, orb_game_kind_t kind);

/* Called with the selected game when the user taps Play. */
typedef void (*orb_game_play_action_cb_t)(orb_game_kind_t kind, void *context);

/* `progress` supplies one short progress line per game, in enum order; NULL
 * or empty entries omit unknown progress. The array is copied, so it may be a temporary. */
orb_game_play_view_t *orb_game_play_view_create(lv_obj_t *parent,
                                                orb_game_play_action_cb_t callback,
                                                void *context,
                                                const char *const progress[ORB_GAME_COUNT]);
void orb_game_play_view_destroy(orb_game_play_view_t *view);
/* Select a game without animation (fixtures and tests). */
void orb_game_play_view_select(orb_game_play_view_t *view, orb_game_kind_t kind);

/* The shell owns gesture arbitration when embedded; standalone previews may
 * retain LVGL swipes. Selection survives app launch/return and shell rebuilds. */
void orb_game_play_view_set_system_input(orb_game_play_view_t *view, bool enabled);
orb_game_kind_t orb_game_play_view_selected(const orb_game_play_view_t *view);

/* Embedders may reject taps already owned by a shell navigation gesture. */
void orb_game_play_view_set_input_guard(orb_game_play_view_t *view,
                                         bool (*allowed)(void *context));
