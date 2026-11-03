#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int radius, start_degrees, end_degrees, duration_ms;
} orb_devtools_arc_t;

static inline bool orb_devtools_arc_command(const char *command, orb_devtools_arc_t *arc)
{
    int end = 0;
    if (!command || !arc || strncmp(command, "arc ", 4) ||
        sscanf(command, "arc %d %d %d %d%n", &arc->radius, &arc->start_degrees,
               &arc->end_degrees, &arc->duration_ms, &end) != 4 || command[end] != '\0') return false;
    return arc->radius >= 1 && arc->radius <= 232 &&
           arc->start_degrees >= -3600 && arc->start_degrees <= 3600 &&
           arc->end_degrees >= -3600 && arc->end_degrees <= 3600 &&
           arc->duration_ms >= 1 && arc->duration_ms <= 5000;
}

static inline void orb_devtools_arc_point(const orb_devtools_arc_t *arc, float fraction, int *x, int *y)
{
    /* Match Design 1.0's polar convention; unwrapped angles preserve the
     * requested direction when the trace crosses twelve o'clock. */
    float radians = (arc->start_degrees + (arc->end_degrees - arc->start_degrees) * fraction) *
                    0.017453292519943295f;
    *x = 233 + (int)lroundf(arc->radius * sinf(radians));
    *y = 233 - (int)lroundf(arc->radius * cosf(radians));
}
