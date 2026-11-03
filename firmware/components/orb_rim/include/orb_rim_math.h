#pragma once

#include <stdbool.h>

/* Extracted from Home's bezel Crown. The default spans 270 degrees clockwise,
 * excluding reserved diagonal navigation circles (hig.md §§3/7). */
enum {
    ORB_RIM_START = 135,
    ORB_RIM_SWEEP = 270,
    ORB_RIM_RADIUS = 212,
    ORB_RIM_WIDTH = 13,
    ORB_RIM_KNOB_RADIUS = 202,
};

typedef struct {
    unsigned gaps; /* 0: none, 1: Back, 2: Back and trailing toolbar control. */
    float start_angle;
    float sweep_angle; /* Zero preserves Home's 135°/270° geometry. */
} orb_rim_config_t;

typedef struct {
    float angle, previous_angle;
    bool active;
} orb_rim_drag_t;

float orb_rim_angle(const orb_rim_config_t *config, float value);
float orb_rim_value(const orb_rim_config_t *config, float angle);
int orb_rim_detent_index(float value, unsigned steps);
/* A changed visual detent is the partner for this quiet sound. orb_sound
 * centrally respects Silent, face-down and quiet hours; no cue is replayed. */
bool orb_rim_detent(float value, unsigned steps, int *selected, bool audible);

/* Coordinates are local to the 466px screen. Begin only claims the existing
 * 179..229px rim band; update unwraps angles across 12 o'clock continuously.
 * Release/cancellation and committing remain the caller's responsibility. */
bool orb_rim_drag_begin(const orb_rim_config_t *config, orb_rim_drag_t *drag,
                        float x, float y);
/* Absolute slider hit test against the visible stroke and round end caps.
 * hit_width is the full touch target, independent of the painted stroke.
 * Angles use LVGL's clockwise-positive screen coordinates (0 = 3 o'clock). */
bool orb_rim_drag_begin_arc(const orb_rim_config_t *config, orb_rim_drag_t *drag,
                            float x, float y, float radius, float hit_width);
/* Anchor a new drag at the current value instead of jumping to the finger.
 * A 360° config accepts the full rim band. Value writes remain the caller's. */
bool orb_rim_drag_begin_relative(const orb_rim_config_t *config, orb_rim_drag_t *drag,
                                 float x, float y, float value);
float orb_rim_drag_update(const orb_rim_config_t *config, orb_rim_drag_t *drag,
                          float x, float y, bool reduce_motion);

