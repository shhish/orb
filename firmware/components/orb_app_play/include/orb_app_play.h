#pragma once

#include "orb_screen.h"
#include "orb_game_play.h"

#define ORB_APP_PLAY_SCREEN_ID 14
#define ORB_APP_PLAY_GAME_SCREEN_ID 0xff01
enum { ORB_APP_PLAY_LAUNCH = 1 };

typedef struct {
    char progress[ORB_GAME_COUNT][48];
    bool system_input;
    /* Borrowed shell input policy, queried synchronously; never serialized
     * into context. Each preview supplies its own read-only policy. */
    bool (*input_allowed)(void *context);
    void *context;
} orb_app_play_model_t;

/* Register the framework presentation port before main starts app admission. */
bool orb_app_play_init(void);
const orb_screen_descriptor_t *orb_app_play_descriptor(void);
const orb_screen_descriptor_t *orb_app_play_game_descriptor(void);
bool orb_app_play_picker(const orb_screen_instance_t *instance);
orb_game_kind_t orb_app_play_selected(const orb_screen_instance_t *instance);
void orb_app_play_select(orb_screen_instance_t *instance, int delta);
bool orb_app_play_selection_changed(orb_screen_instance_t *instance);
void orb_app_play_rotary_finish(orb_screen_instance_t *instance, bool commit);
