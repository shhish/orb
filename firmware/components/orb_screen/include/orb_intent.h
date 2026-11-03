#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "orb_nav.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t orb_screen_id_t;
#define ORB_SCREEN_INVALID_ID ORB_NAV_INVALID_ID

typedef struct {
    uint64_t identity;
    orb_nav_token_t rect;
    bool valid;
} orb_launch_token_t;

typedef struct {
    uint64_t entity_id;
    uint32_t page;
} orb_screen_args_t;

typedef enum {
    ORB_INPUT_PROGRAMMATIC, ORB_INPUT_TOUCH, ORB_INPUT_CROWN,
    ORB_INPUT_BUTTON, ORB_INPUT_RESTORE, ORB_INPUT_PREVIEW,
} orb_input_origin_t;

typedef enum {
    ORB_INTENT_NAVIGATE = 1, ORB_INTENT_HOME_SET_LEVEL,
    ORB_INTENT_SETTING_SET, ORB_INTENT_SEND, ORB_INTENT_APPROVE,
    ORB_INTENT_UNLOCK, ORB_INTENT_APP_COMMAND,
    /* Discrete Home writes and scene runs: never rim-eligible. */
    ORB_INTENT_HOME_COMMAND,
} orb_intent_kind_t;

#define ORB_INTENT_ID_SIZE 33

/* Value-only queue payload: the host copies this before returning. Correlation
 * belongs to service result flow; restoring a context never replays it. */
typedef struct {
    orb_intent_kind_t kind;
    uint64_t correlation;
    orb_input_origin_t origin;
    bool physical;
    /* Source geometry for app commands that open an entity rather than a
     * plain route. The host copies it before the activating row can leave. */
    orb_launch_token_t launch;
    union {
        struct {
            orb_screen_id_t screen;
            orb_screen_args_t args;
            orb_launch_token_t launch;
            /* A screen that owns its local hierarchy asks the host to present
             * it: prepare precedes the screen's own tree change so the source
             * can be captured; reverse returns toward the launch token. */
            bool reverse, prepare;
        } navigate;
        /* HOME_SET_LEVEL and HOME_COMMAND: a stable service alias, the app's
         * action, one writable characteristic bit and its value. */
        struct {
            char id[ORB_INTENT_ID_SIZE];
            uint32_t action, field;
            float value;
            bool deliberate;
        } home;
        struct { uint32_t key; int32_t value; } setting;
        /* APP_COMMAND: an app-defined action with its target and up to two
         * value arguments; extra defaults to zero for one-argument actions. */
        struct { uint64_t target; uint32_t action; int32_t value, extra; } command;
    } value;
} orb_intent_t;

#ifdef __cplusplus
}
#endif
