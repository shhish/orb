#include "orb_motion_math.h"

#include <math.h>

#define HYSTERESIS_PX 0.6f
#define FULL_TILT_G 0.35f   /* sin(20 degrees): the tilt that gives the full shift */

void orb_motion_lowpass(orb_vec3_t *state, orb_vec3_t in, float dt_s, float tau_s)
{
    float k = dt_s / (tau_s + dt_s);
    state->x += (in.x - state->x) * k;
    state->y += (in.y - state->y) * k;
    state->z += (in.z - state->z) * k;
}

void orb_motion_light(orb_vec3_t g, float max_dev, float *deg, float *amount)
{
    /* Up, projected onto the screen, as an LVGL angle (screen y grows downwards). */
    float lean = sqrtf(g.x * g.x + g.y * g.y);
    float up = atan2f(-g.y, g.x) * 180.0f / (float)M_PI;
    float dev = fmodf(up - 270 + 540, 360) - 180;
    dev = dev < -max_dev ? -max_dev : dev > max_dev ? max_dev : dev;
    *deg = 270 + dev;
    *amount = lean > 1 ? 1 : lean;
}

static bool settle(float target, int *px)
{
    if (fabsf(target - *px) <= HYSTERESIS_PX) {
        return false;
    }
    *px = (int)lrintf(target);
    return true;
}

static float unit(float v)
{
    v /= FULL_TILT_G;
    return v > 1 ? 1 : v < -1 ? -1 : v;
}

bool orb_motion_parallax(orb_vec3_t tilt, float depth, float max_px, int *px, int *py)
{
    /* Downhill in screen coordinates: against the change in the up vector, y flipped. */
    bool moved = settle(-unit(tilt.x) * depth * max_px, px);
    moved |= settle(unit(tilt.y) * depth * max_px, py);
    return moved;
}
