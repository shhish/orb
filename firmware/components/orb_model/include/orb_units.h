#pragma once

#include <math.h>

/* UNKNOWN also represents "auto" in a preference patch. Home wire temperatures
 * stay Celsius; Weather carries its explicit resolved unit. Rounding belongs
 * only at the display/control boundary. */
typedef enum { ORB_TEMPERATURE_UNKNOWN, ORB_TEMPERATURE_C, ORB_TEMPERATURE_F } orb_temperature_t;

static inline float orb_temperature_display(float celsius, orb_temperature_t unit)
{
    return roundf(unit == ORB_TEMPERATURE_F ? celsius * 1.8f + 32 : celsius);
}

static inline float orb_temperature_target(float position, orb_temperature_t unit)
{
    position = fminf(1, fmaxf(0, position));
    return unit == ORB_TEMPERATURE_F ? 50 + roundf(position * 40) : 10 + roundf(position * 40) / 2;
}

static inline float orb_temperature_celsius(float target, orb_temperature_t unit)
{
    return unit == ORB_TEMPERATURE_F ? roundf((target - 32) / 1.8f * 10) / 10 : target;
}

static inline float orb_temperature_position(float celsius, orb_temperature_t unit)
{
    return unit == ORB_TEMPERATURE_F ? (orb_temperature_display(celsius, unit) - 50) / 40 : (celsius - 10) / 20;
}
