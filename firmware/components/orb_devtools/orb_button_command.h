#pragma once

#include <stdio.h>
#include <string.h>
#include "orb_buttons.h"

/* Exact tokens only: neither the console nor network may turn a developer
 * injection into a raw GPIO, a trusted input or an arbitrary command. */
static inline bool orb_devtools_button_command(const char *command, orb_button_t *button,
                                                orb_button_gesture_t *gesture)
{
    if (!command || !button || !gesture) return false;
    char name[8], kind[8];
    int end = 0;
    if (sscanf(command, "press %7s %7s%n", name, kind, &end) != 2 || command[end]) return false;
    if (!strcmp(name, "pwr")) *button = ORB_BUTTON_PWR;
    else if (!strcmp(name, "boot")) *button = ORB_BUTTON_BOOT;
    else return false;
    if (!strcmp(kind, "short")) *gesture = ORB_BUTTON_SHORT;
    else if (!strcmp(kind, "double")) *gesture = ORB_BUTTON_DOUBLE;
    else if (!strcmp(kind, "hold")) *gesture = ORB_BUTTON_HOLD;
    else return false;
    char normalized[32];
    snprintf(normalized, sizeof(normalized), "press %s %s", name, kind);
    if (strcmp(command, normalized)) return false;
    return true;
}
