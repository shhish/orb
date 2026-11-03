#include "orb_rim_math.h"

#include <math.h>

#define RIM_GAP 24.0f
#define RIM_PI 3.14159265358979323846f

static float clamp(float value, float low, float high)
{
    return fminf(high, fmaxf(low, value));
}

static unsigned gaps(const orb_rim_config_t *config)
{
    return config && config->gaps <= 2 ? config->gaps : 0;
}

static float start(const orb_rim_config_t *config)
{
    return config && config->sweep_angle > 0 ? config->start_angle : ORB_RIM_START;
}

static float sweep(const orb_rim_config_t *config)
{
    return config && config->sweep_angle > 0 ? config->sweep_angle : ORB_RIM_SWEEP;
}

float orb_rim_angle(const orb_rim_config_t *config, float value)
{
    float angle = start(config) + value * (sweep(config) - gaps(config) * 2 * RIM_GAP);
    /* Compress values across navigation gaps, never discard value steps. */
    for (unsigned i = 0; i < gaps(config); ++i) {
        float start = 225 + i * 90 - RIM_GAP;
        if (angle > start) angle += 2 * RIM_GAP;
    }
    return angle;
}

float orb_rim_value(const orb_rim_config_t *config, float angle)
{
    float distance = angle - start(config);
    for (unsigned i = 0; i < gaps(config); ++i)
        distance -= clamp(angle - (225 + i * 90 - RIM_GAP), 0, 2 * RIM_GAP);
    return distance / (sweep(config) - gaps(config) * 2 * RIM_GAP);
}

int orb_rim_detent_index(float value, unsigned steps)
{
    return (int)lroundf(clamp(value, 0, 1) * steps);
}

static float pointer_angle(float x, float y)
{
    float angle = atan2f(y - 233, x - 233) * 180 / RIM_PI;
    return angle < 0 ? angle + 360 : angle;
}

bool orb_rim_drag_begin(const orb_rim_config_t *config, orb_rim_drag_t *drag,
                        float x, float y)
{
    if (!drag) return false;
    drag->active = false;
    if (!isfinite(x) || !isfinite(y)) return false;
    float radius = hypotf(x - 233, y - 233);
    if (radius < 179 || radius > 229) return false;
    float angle = pointer_angle(x, y);
    float logical = angle;
    float low = start(config), high = low + sweep(config);
    while (logical < low - 15) logical += 360;
    while (logical > high + 15) logical -= 360;
    if (sweep(config) < 360 && (logical < low - 15 || logical > high + 15))
        return false;
    drag->active = true;
    drag->angle = logical;
    drag->previous_angle = angle;
    return true;
}

bool orb_rim_drag_begin_relative(const orb_rim_config_t *config, orb_rim_drag_t *drag,
                                 float x, float y, float value)
{
    if (!isfinite(value)) {
        if (drag) drag->active = false;
        return false;
    }
    if (!orb_rim_drag_begin(config, drag, x, y)) return false;
    drag->angle = orb_rim_angle(config, clamp(value, 0, 1));
    return true;
}

bool orb_rim_drag_begin_arc(const orb_rim_config_t *config, orb_rim_drag_t *drag,
                            float x, float y, float radius, float hit_width)
{
    if (!drag) return false;
    drag->active = false;
    if (!isfinite(x) || !isfinite(y) || !isfinite(radius) || !isfinite(hit_width) ||
        radius <= 0 || hit_width <= 0) return false;
    float angle = pointer_angle(x, y);
    float low = start(config), high = low + sweep(config);
    /* Pick the equivalent angle nearest the middle of this continuous arc.
     * A point just outside an end cap snaps there instead of wrapping values. */
    float logical = angle + 360 * roundf(((low + high) * .5f - angle) / 360);
    float nearest = clamp(logical, low, high) * RIM_PI / 180;
    if (hypotf(x - 233 - radius * cosf(nearest),
               y - 233 - radius * sinf(nearest)) > hit_width * .5f) return false;
    drag->active = true;
    drag->angle = logical;
    drag->previous_angle = angle;
    return true;
}

float orb_rim_drag_update(const orb_rim_config_t *config, orb_rim_drag_t *drag,
                          float x, float y, bool reduce_motion)
{
    if (!drag) return 0;
    if (drag->active && isfinite(x) && isfinite(y) && hypotf(x - 233, y - 233) >= 1) {
        float angle = pointer_angle(x, y);
        float delta = angle - drag->previous_angle;
        if (delta > 180) delta -= 360;
        if (delta < -180) delta += 360;
        drag->angle += delta;
        drag->previous_angle = angle;
    }
    float raw = orb_rim_value(config, drag->angle);
    float value = clamp(raw, 0, 1);
    if (!reduce_motion && raw != value) {
        float distance = raw - value;
        value += copysignf(.075f * (fabsf(distance) * .55f / (.075f + fabsf(distance) * .55f)), distance);
    }
    return value;
}

