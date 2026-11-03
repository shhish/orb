#pragma once

#include "lvgl.h"
#include "orb_game_logic.h"

#define ORB_GAME_NATIVE_BODY_D (ORB_GAME_BEAD_DRAW_RADIUS * 2u + 1u)
#define ORB_GAME_NATIVE_PHASE_BITS 3
#define ORB_GAME_NATIVE_PHASES (1 << ORB_GAME_NATIVE_PHASE_BITS)

typedef struct {
    uint8_t first, last, opaque_first, opaque_last;
} orb_game_native_row_t;

typedef struct {
    const lv_opa_t *pixels;
    unsigned diameter;
    orb_game_native_row_t rows[ORB_GAME_NATIVE_BODY_D];
} orb_game_native_mask_t;

typedef struct {
    orb_game_native_mask_t body;
} orb_game_native_beads_t;

/* UI-owned, synchronous renderer. The caller embeds row metadata in its
 * budgeted PSRAM view. Coverage is immutable after cold setup; playback only
 * writes the supplied layer/clip and allocates no memory. */
void orb_game_native_beads_prepare(orb_game_native_beads_t *masks,
                                    const lv_opa_t *body);
void orb_game_native_beads(lv_layer_t *layer, const orb_game_native_beads_t *masks,
                            size_t count, const int16_t *x, const int16_t *y,
                            const uint8_t *opacity, const orb_game_sprite_t *sprites);

/* Identical row kernel for immutable compact poses. colour_stride is bytes;
 * each colour field is uint32_t-aligned and remains immutable for the call. */
void orb_game_native_beads_colours(lv_layer_t *layer, const orb_game_native_beads_t *masks,
                                    size_t count, const int16_t *x, const int16_t *y,
                                    const uint8_t *opacity, const uint32_t *colours,
                                    size_t colour_stride);

/* General full-opacity circular coverage for other immutable game sources.
 * circle fills `pixels` ((2r+1)^2 bytes) with LVGL's exact radius-mask
 * coverage; rows summarises each row's ink and opaque runs. stamp returns
 * false for non-RGB565 layers so the caller keeps its LVGL path. */
bool orb_game_native_circle(lv_opa_t *pixels, unsigned radius);
void orb_game_native_rows(orb_game_native_row_t *rows, const lv_opa_t *pixels, unsigned diameter);
bool orb_game_native_stamp(lv_layer_t *layer, const lv_area_t *clip, int x, int y,
                           const lv_opa_t *pixels, const orb_game_native_row_t *rows,
                           unsigned diameter, uint32_t colour);

/* Pass 5 named Pop motion patch: place an integer coverage mask (as from
 * orb_game_native_circle) at a centre (qx, qy) in 1/ORB_GAME_NATIVE_PHASES px. Phase zero keeps
 * LVGL's coverage exactly; other phases resample it bilinearly. `fill` fades
 * as LVGL's fill (mask x opa / 255), otherwise as its border (mask x opa >> 8),
 * whose row `full_row` is an unmasked straight segment. `rows` summarises
 * `mask` (orb_game_native_rows) so empty columns are skipped. Returns false for
 * layers other than RGB565, which keep their LVGL path. */
bool orb_game_native_phase_stamp(lv_layer_t *layer, const lv_area_t *clip, int32_t qx, int32_t qy,
                                 const lv_opa_t *mask, const orb_game_native_row_t *rows, unsigned diameter,
                                 uint32_t colour, lv_opa_t opa, bool fill, int full_row);
