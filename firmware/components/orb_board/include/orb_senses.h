#pragma once

/* Motion decisions are independent of ESP-IDF and never execute UI actions.
 * The consumer must route detents to its current rotary owner and double taps
 * through the shell's safe-action gate (SoundSenses, HIG sections 7 and 8). */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float ax, ay, az; /* specific force, g, screen axes */
    float gx, gy, gz; /* angular velocity, degrees/second, same axes */
    bool hardware_double_tap;
} orb_motion_sample_t;

typedef struct {
    uint64_t previous_ms, flat_since_ms, quiet_since_ms, guard_until_ms;
    uint64_t first_tap_ms, peak_since_ms, settle_since_ms, refractory_until_ms;
    float previous_z, baseline_z, bias_z, angle;
    /* Zero-rate bias for all three axes. It survives gesture resets: one
     * board read a constant 20 deg/s on y at rest, larger than "movement". */
    float bias_x, bias_y, previous_ax, previous_ay, previous_g[3], seed_sum[3];
    uint64_t seed_since_ms;
    unsigned seed_count;
    bool initialized, desk_armed, peak, second_peak, pending_double, bias_valid;
} orb_senses_t;

typedef struct {
    int detents;
    bool double_tap;
    bool resting;
} orb_senses_result_t;

/* Call from one owner at 50 Hz or faster; missed intervals >100 ms reset all
 * gesture accumulation. Zero-initialize state. Positive detents are clockwise
 * viewed from the face (negative screen-axis gz), matching the touch rim. */
orb_senses_result_t orb_senses_update(orb_senses_t *state, const orb_motion_sample_t *sample,
                                     bool touching, uint64_t now_ms);
