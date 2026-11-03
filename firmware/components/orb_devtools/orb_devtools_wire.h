#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "orb_tilt_command.h"

#define ORB_DEVTOOLS_COMMAND_BYTES 96
#define ORB_DEVTOOLS_TEXT_BYTES 2048
#define ORB_DEVTOOLS_MAX_OUTPUT (2u * 1024u * 1024u)
#define ORB_DEVTOOLS_MAX_EVENTS 8192u
#define ORB_DEVTOOLS_DEADLINE_US 90000000LL

/* FILE write callbacks receive spans, not NUL-terminated strings. */
static inline bool orb_devtools_wire_contains(const char *text, size_t size, const char *needle)
{
    size_t length = strlen(needle);
    for (size_t i = 0; length <= size && i <= size - length; i++)
        if (!memcmp(text + i, needle, length)) return true;
    return false;
}

/* This whitelist is deliberately independent of the console's permissive
 * sscanf parser: paired network input must never become arbitrary text. */
static inline bool orb_devtools_wire_integer(const cJSON *arg, int low, int high)
{
    return cJSON_IsNumber(arg) && isfinite(arg->valuedouble) &&
           arg->valuedouble >= low && arg->valuedouble <= high &&
           arg->valuedouble == (double)(int)arg->valuedouble;
}

static inline bool orb_devtools_wire_one_of(const cJSON *arg, const char *const *names)
{
    const char *name = cJSON_GetStringValue(arg);
    if (!name) return false;
    for (unsigned i = 0; names[i]; i++) if (!strcmp(name, names[i])) return true;
    return false;
}

static inline bool orb_devtools_wire_command(const char *method, const cJSON *params,
                                             uint32_t *stream, char command[ORB_DEVTOOLS_COMMAND_BYTES])
{
    static const char *const opens[] = {"play", "dot", "agents", "apps", "back", NULL};
    static const char *const scrolls[] = {"agents", "rooms", "settings", "notifications", "favourites", NULL};
    static const char *const games[] = {"marble", "beads", "orbit", "ring-snake", "pop", NULL};
    static const char *const buttons[] = {"pwr", "boot", NULL};
    static const char *const presses[] = {"short", "double", "hold", NULL};
    static const char *const surfaces[] = {"face", "stack", "apps", "agents", "sessions", "usage", "dot", "home", "time", "play", "notifications", "control", "settings", "sound", "battery", "updates", "onboarding", NULL};
    if (!method || strncmp(method, "devtools.", 9) || !cJSON_IsObject(params) ||
        cJSON_GetArraySize(params) != 2) return false;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "stream");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "args");
    if (!orb_devtools_wire_integer(id, 1, INT32_MAX) || !cJSON_IsArray(args)) return false;
    const char *name = method + 9;
    int count = cJSON_GetArraySize(args), expected = -1;
    bool named = false;
    if (!strcmp(name, "shot") || !strcmp(name, "heap") || !strcmp(name, "top") ||
        !strcmp(name, "power") || !strcmp(name, "buttons") || !strcmp(name, "wake") || !strcmp(name, "sleep") || !strcmp(name, "crash") || !strcmp(name, "usbreattach") || !strcmp(name, "soundstop") ||
        !strcmp(name, "soundstatus") || !strcmp(name, "senses") || !strcmp(name, "micrecord")) expected = 0;
    else if (!strcmp(name, "soundplay") || !strcmp(name, "soundvolume") ||
             !strcmp(name, "soundsilent") || !strcmp(name, "soundalwayson")) {
        expected = 1;
        int maximum = !strcmp(name, "soundplay") ? 18 : !strcmp(name, "soundvolume") ? 100 : 1;
        if (!orb_devtools_wire_integer(cJSON_GetArrayItem(args, 0), 0, maximum)) return false;
    }
    else if (!strcmp(name, "press")) {
        expected = 2;
        if (!orb_devtools_wire_one_of(cJSON_GetArrayItem(args, 0), buttons) ||
            !orb_devtools_wire_one_of(cJSON_GetArrayItem(args, 1), presses)) return false;
    }
    else if (!strcmp(name, "lightsleep")) {
        static const char *const usb[] = {"usb", NULL};
        static const char *const enabled[] = {"on", "off", NULL};
        expected = 2;
        if (!orb_devtools_wire_one_of(cJSON_GetArrayItem(args, 0), usb) ||
            !orb_devtools_wire_one_of(cJSON_GetArrayItem(args, 1), enabled)) return false;
    }
    else if (!strcmp(name, "goto")) {
        expected = 1;
        if (!orb_devtools_wire_one_of(cJSON_GetArrayItem(args, 0), surfaces)) return false;
    }
    else if (!strcmp(name, "tap")) expected = 2;
    else if (!strcmp(name, "hold")) expected = 3;
    else if (!strcmp(name, "swipe")) expected = 5;
    else if (!strcmp(name, "arc")) expected = 4;
    else if (!strcmp(name, "tilt")) {
        expected = 3;
        const cJSON *x = cJSON_GetArrayItem(args, 0), *y = cJSON_GetArrayItem(args, 1);
        if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) ||
            !orb_devtools_tilt_values(x->valuedouble, y->valuedouble) ||
            !orb_devtools_wire_integer(cJSON_GetArrayItem(args, 2), 1, 5000)) return false;
    }
    else if (!strcmp(name, "bench")) expected = 1;
    else if (!strcmp(name, "bench_open") || !strcmp(name, "bench_nav")) {
        expected = 1; named = true;
        if (!orb_devtools_wire_one_of(cJSON_GetArrayItem(args, 0), opens)) return false;
        name = "bench_open";
    } else if (!strcmp(name, "bench_scroll") || !strcmp(name, "bench_game")) {
        expected = 2; named = true;
        if (!orb_devtools_wire_one_of(cJSON_GetArrayItem(args, 0),
                                     !strcmp(name, "bench_game") ? games : scrolls)) return false;
    }
    if (expected < 0 || count != expected) return false;
    bool bench = !strncmp(name, "bench", 5);
    int used = snprintf(command, ORB_DEVTOOLS_COMMAND_BYTES, "%s", name);
    for (int i = 0; i < count; i++) {
        const cJSON *arg = cJSON_GetArrayItem(args, i);
        int wrote;
        if ((named && !i) || !strcmp(name, "press") || !strcmp(name, "lightsleep") || !strcmp(name, "goto")) {
            wrote = snprintf(command + used, ORB_DEVTOOLS_COMMAND_BYTES - used, " %s", arg->valuestring);
        } else {
            if (!strcmp(name, "tilt")) {
                wrote = i < 2 ? snprintf(command + used, ORB_DEVTOOLS_COMMAND_BYTES - used,
                                         " %.17g", arg->valuedouble) :
                                snprintf(command + used, ORB_DEVTOOLS_COMMAND_BYTES - used, " %d", arg->valueint);
            } else if (!strcmp(name, "arc")) {
                int low = i == 0 || i == 3 ? 1 : -3600;
                int high = i == 0 ? 232 : i == 3 ? 5000 : 3600;
                if (!orb_devtools_wire_integer(arg, low, high)) return false;
                wrote = snprintf(command + used, ORB_DEVTOOLS_COMMAND_BYTES - used, " %d", arg->valueint);
            } else {
                bool duration = bench || (!strcmp(name, "hold") && i == 2) || (!strcmp(name, "swipe") && i == 4);
                if (!orb_devtools_wire_integer(arg, duration ? (bench ? 1000 : 1) : 0,
                                              duration ? (bench ? 60000 : 5000) : 465)) return false;
                wrote = snprintf(command + used, ORB_DEVTOOLS_COMMAND_BYTES - used, " %d", arg->valueint);
            }
        }
        if (wrote < 0 || wrote >= ORB_DEVTOOLS_COMMAND_BYTES - used) return false;
        used += wrote;
    }
    *stream = (uint32_t)id->valueint;
    return true;
}
