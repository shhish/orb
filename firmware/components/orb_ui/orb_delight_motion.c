#include "orb_color_tokens.h"
#include "orb_delight_motion.h"

#include <math.h>

#define SUBSTEP_S (1.0f / 240.0f)
#define POSITION_EPSILON 0.001f
#define VELOCITY_EPSILON 0.005f

static float clamp(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

void orb_delight_spring_reset(orb_delight_spring_t *spring, float value)
{
    if (!spring || !isfinite(value)) return;
    *spring = (orb_delight_spring_t){.value = value, .target = value};
}

void orb_delight_spring_target(orb_delight_spring_t *spring, float target)
{
    if (spring && isfinite(target)) spring->target = target;
}

bool orb_delight_spring_settled(const orb_delight_spring_t *spring)
{
    return !spring || (fabsf(spring->target - spring->value) < POSITION_EPSILON &&
                       fabsf(spring->velocity) < VELOCITY_EPSILON);
}

bool orb_delight_spring_step(orb_delight_spring_t *spring, float dt_s,
                             float duration_s, float bounce)
{
    if (!spring || !isfinite(dt_s) || dt_s <= 0 || !isfinite(duration_s) ||
        !isfinite(bounce)) return false;
    float before = spring->value;
    if (orb_delight_spring_settled(spring)) {
        spring->value = spring->target;
        spring->velocity = 0;
        spring->accumulator = 0;
        return spring->value != before;
    }
    /* HIG §6 equations. Semi-implicit Euler at fixed 240 Hz preserves velocity
     * when the owner changes targets between two display frames. */
    float duration = clamp(duration_s, 0.08f, 2.0f);
    bounce = clamp(bounce, -1.0f, 0.4f);
    float omega = (2.0f * (float)M_PI) / duration;
    float zeta = bounce >= 0 ? 1.0f - bounce :
                 bounce <= -1.0f ? 2.0f : 1.0f / (1.0f + bounce);
    zeta = fminf(zeta, 2.0f);
    float stiffness = omega * omega;
    float damping = 2.0f * zeta * omega;
    spring->accumulator += fminf(dt_s, 0.25f);
    while (spring->accumulator >= SUBSTEP_S) {
        float acceleration = stiffness * (spring->target - spring->value) -
                             damping * spring->velocity;
        spring->velocity += acceleration * SUBSTEP_S;
        spring->value += spring->velocity * SUBSTEP_S;
        spring->accumulator -= SUBSTEP_S;
    }
    if (orb_delight_spring_settled(spring)) {
        spring->value = spring->target;
        spring->velocity = 0;
        spring->accumulator = 0;
    }
    return spring->value != before;
}

float orb_delight_rubber_band(float distance, float extent)
{
    if (!isfinite(distance) || !isfinite(extent) || extent <= 0) return 0;
    /* A diminishing response makes the bound tangible without allowing a row
     * to escape the round viewport. There is no bounce under Reduce Motion:
     * the caller tracks the finger directly and clamps to its scroll bounds. */
    float scaled = fabsf(distance) * 0.55f;
    return copysignf(extent * (scaled / (extent + scaled)), distance);
}

float orb_delight_crossfade(uint32_t elapsed_ms)
{
    return elapsed_ms >= 150 ? 1.0f : (float)elapsed_ms / 150.0f;
}

uint32_t orb_delight_color(orb_delight_color_t color, bool increased_contrast)
{
    static const uint32_t s_palette[ORB_DELIGHT_COLOR_COUNT][2] = {
        [ORB_DELIGHT_RED] = {ORB_RGB_POWER_LOW, ORB_RGB_POWER_LOW_CONTRAST},
        [ORB_DELIGHT_ORANGE] = {ORB_RGB_LEGACY_ORANGE, ORB_RGB_ORANGE_CONTRAST},
        [ORB_DELIGHT_YELLOW] = {ORB_RGB_LEGACY_YELLOW, ORB_RGB_YELLOW_CONTRAST},
        [ORB_DELIGHT_GREEN] = {ORB_RGB_GREEN, ORB_RGB_GREEN_CONTRAST},
        [ORB_DELIGHT_MINT] = {ORB_RGB_MINT, ORB_RGB_MINT_CONTRAST},
        [ORB_DELIGHT_TEAL] = {ORB_RGB_LEGACY_TEAL, ORB_RGB_TEAL_CONTRAST},
        [ORB_DELIGHT_BLUE] = {ORB_RGB_LEGACY_BLUE, ORB_RGB_BLUE_CONTRAST},
        [ORB_DELIGHT_INDIGO] = {ORB_RGB_LEGACY_INDIGO, ORB_RGB_INDIGO_CONTRAST},
        [ORB_DELIGHT_GRAY] = {ORB_RGB_GRAY, ORB_RGB_GRAY_CONTRAST},
    };
    if ((unsigned)color >= ORB_DELIGHT_COLOR_COUNT) color = ORB_DELIGHT_GRAY;
    return s_palette[color][increased_contrast ? 1 : 0];
}
