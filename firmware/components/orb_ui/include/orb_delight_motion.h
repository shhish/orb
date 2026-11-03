#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Shared kit and shell feedback math. Plain C keeps the interrupted trajectories testable
 * without depending on the renderer or giving a draw callback its own clock. */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float value;
    float velocity;
    float target;
    float accumulator;
} orb_delight_spring_t;

void orb_delight_spring_reset(orb_delight_spring_t *spring, float value);
/* Retargeting deliberately preserves velocity (HIG §6). */
void orb_delight_spring_target(orb_delight_spring_t *spring, float target);
/* duration .5 / bounce 0: smooth; .5 / .15: paging; .15 / .14: touch.
 * Returns whether the presented value changed. Integrates at 240 Hz; delays
 * longer than 250 ms are bounded so waking never stalls the UI thread. */
bool orb_delight_spring_step(orb_delight_spring_t *spring, float dt_s,
                             float duration_s, float bounce);
bool orb_delight_spring_settled(const orb_delight_spring_t *spring);

/* Signed drag beyond a scroll bound. Approaches extent without reaching it. */
float orb_delight_rubber_band(float distance, float extent);
/* HIG §6: Reduce Motion replaces spatial/depth movement with a 150 ms fade.
 * Pass monotonic elapsed time (unsigned subtraction is safe at tick wrap). */
float orb_delight_crossfade(uint32_t elapsed_ms);

typedef enum {
    ORB_DELIGHT_RED,
    ORB_DELIGHT_ORANGE,
    ORB_DELIGHT_YELLOW,
    ORB_DELIGHT_GREEN,
    ORB_DELIGHT_MINT,
    ORB_DELIGHT_TEAL,
    ORB_DELIGHT_BLUE,
    ORB_DELIGHT_INDIGO,
    ORB_DELIGHT_GRAY,
    ORB_DELIGHT_COLOR_COUNT,
} orb_delight_color_t;

/* HIG §5 exact palette. Use the increased column for Increase Contrast and
 * Reduce Transparency (delight §1); callers still pair colour with a symbol. */
uint32_t orb_delight_color(orb_delight_color_t color, bool increased_contrast);

#ifdef __cplusplus
}
#endif
