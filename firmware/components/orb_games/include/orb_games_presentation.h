#pragma once

#include "lvgl.h"
#include <stdbool.h>

/* Composition-root port. Main still owns simulation, publication and the
 * independent screen. The UI adapter owns only this presentation lifetime.
 * Both callbacks run under LVGL; destroy also follows a failed create. */
typedef struct {
    void *context;
    bool (*create)(lv_obj_t *parent, void *context);
    void (*destroy)(void *context);
} orb_games_view_port_t;

typedef struct {
    void *(*open)(lv_obj_t *parent, const orb_games_view_port_t *view);
    void (*close)(void *presentation);
} orb_games_presentation_t;

/* Register before app entry; a live runtime never changes its presenter. */
bool orb_games_set_presentation(const orb_games_presentation_t *presentation);
