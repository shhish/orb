#pragma once

#include "lvgl.h"
#include "orb_game_logic.h"
#include "orb_game_native.h"

/* Pass 5 named visual patch (Pop motion). Live bubbles drift less than a pixel
 * per step, so integer placement moved them in irregular one-pixel jumps,
 * about 39 changes a second. Play stamps each bubble's body and rim at
 * 1/ORB_GAME_NATIVE_PHASES px from LVGL's own coverage, which a whole-pixel
 * centre reproduces exactly. Coverage lives in PSRAM on the games budget. */
#define ORB_GAME_POP_COVERAGES 4u

typedef struct {
    unsigned radius;
    lv_opa_t *body, *rim;
    orb_game_native_row_t *body_rows, *rim_rows;
} orb_game_pop_coverage_t;

/* Update boundary: capture each new live radius once; others stay on LVGL. */
void orb_game_pop_coverage_prepare(orb_game_pop_coverage_t coverage[ORB_GAME_POP_COVERAGES],
                                   const orb_game_sprite_t *sprites, size_t count);
void orb_game_pop_coverage_release(orb_game_pop_coverage_t coverage[ORB_GAME_POP_COVERAGES]);

/* Stamp after this layer's earlier tasks finish; false (no coverage or not
 * RGB565) means the caller draws the integer LVGL bubble instead. */
bool orb_game_pop_draw(const orb_game_pop_coverage_t coverage[ORB_GAME_POP_COVERAGES], lv_layer_t *layer,
                       const orb_game_sprite_t *sprite);
#ifndef ESP_PLATFORM
/* Simulator oracle: draw live bubbles through LVGL at integer pixels. */
void orb_game_pop_test_integer(bool integer);
#endif
