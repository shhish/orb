#pragma once

#include "lvgl.h"
#include "orb_game_logic.h"

typedef struct {
    uint8_t first, last, opaque_first, opaque_last;
    uint16_t alpha_offset;
} orb_game_orbit_ring_row_t;

typedef struct {
    uint16_t radius, width;
    orb_game_orbit_ring_row_t rows[];
} orb_game_orbit_ring_t;

typedef struct orb_game_orbit orb_game_orbit_t;
orb_game_orbit_ring_t *orb_game_ring_prepare(unsigned radius, unsigned width, uint32_t *bytes);

/* Creation transfers immutable ring ownership only on success. Both source
 * and command pose are charged to the unchanged games PSRAM budget. */
orb_game_orbit_t *orb_game_orbit_create(orb_game_orbit_ring_t *const rings[3]);
void orb_game_orbit_release(orb_game_orbit_t *source);
bool orb_game_orbit_dispatch(orb_game_orbit_t *source, lv_layer_t *layer,
                              const orb_game_sprite_t *sprites, size_t count,
                              uint64_t revision, uint32_t ring_colour);
