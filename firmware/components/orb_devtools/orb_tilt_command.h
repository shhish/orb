#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    float x, y;
    unsigned duration_ms;
} orb_devtools_tilt_t;

typedef struct {
    float x, y, z;
    int64_t until_us;
} orb_devtools_tilt_override_t;

static inline bool orb_devtools_tilt_values(double x, double y)
{
    return isfinite(x) && isfinite(y) && x >= -1 && x <= 1 && y >= -1 && y <= 1 &&
           x * x + y * y <= 1;
}

static inline bool orb_devtools_tilt_command(const char *command, orb_devtools_tilt_t *tilt)
{
    double x, y, duration;
    int end = 0;
    if (!command || !tilt || strncmp(command, "tilt ", 5) ||
        sscanf(command, "tilt %lf %lf %lf%n", &x, &y, &duration, &end) != 3 || command[end] != '\0' ||
        !orb_devtools_tilt_values(x, y) || !isfinite(duration) || duration < 1 || duration > 5000 ||
        duration != floor(duration)) return false;
    *tilt = (orb_devtools_tilt_t){.x = (float)x, .y = (float)y, .duration_ms = (unsigned)duration};
    return true;
}

static inline void orb_devtools_tilt_begin(orb_devtools_tilt_override_t *state,
                                           const orb_devtools_tilt_t *tilt, int64_t now_us)
{
    *state = (orb_devtools_tilt_override_t){.x = tilt->x, .y = tilt->y,
        .z = sqrtf(fmaxf(0, 1 - tilt->x * tilt->x - tilt->y * tilt->y)),
        .until_us = now_us + (int64_t)tilt->duration_ms * 1000};
}

static inline bool orb_devtools_tilt_sample(orb_devtools_tilt_override_t *state,
                                            int64_t now_us, float *x, float *y, float *z)
{
    /* Expiry is checked at consumption: no timer, allocation, or stale override
     * survives a paused app or a delayed service task. */
    if (now_us < 0 || now_us >= state->until_us) {
        state->until_us = 0;
        return false;
    }
    *x = state->x; *y = state->y; *z = state->z;
    return true;
}
