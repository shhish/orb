#pragma once

/*
 * The motion sensor's gravity turned into what the UI uses: where the light
 * comes from (for the glass highlight) and how far each layer shifts
 * (parallax). Plain C, no LVGL or ESP-IDF, so it is tested on the host.
 *
 * Gravity is in g, screen axes: +x right, +y up, +z out of the screen, as the
 * accelerometer reports it at rest (it points up).
 */

#include <stdbool.h>

typedef struct {
    float x, y, z;
} orb_vec3_t;

/* One step of a first-order low-pass with time constant tau_s over dt_s. */
void orb_motion_lowpass(orb_vec3_t *state, orb_vec3_t in, float dt_s, float tau_s);

/*
 * The light for the glass highlight: from above in the room, so on the rim
 * facing up. *deg is an LVGL angle (0 at 3 o'clock, clockwise), kept within
 * max_dev degrees of 12 o'clock so the highlight stays near the top; *amount
 * (0..1) is how far the screen leans out of flat, as a lit edge gets brighter
 * the more the light grazes it.
 */
void orb_motion_light(orb_vec3_t g, float max_dev, float *deg, float *amount);

/*
 * The parallax shift of a layer at `depth` (0: the screen plane, 1: the
 * furthest back) for a tilt: `tilt` is gravity now minus gravity in the pose
 * the device has rested in lately, so any steady pose looks centred and only
 * movement shows depth. Up to max_px (reached at about 20 degrees), towards
 * the side that dips, as a layer behind the glass would seem to move. *px, *py
 * are the shift in effect; they only move when the target is more than 0.6 px
 * away, so sensor noise doesn't redraw the screen. True if they moved.
 */
bool orb_motion_parallax(orb_vec3_t tilt, float depth, float max_px, int *px, int *py);
