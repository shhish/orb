#pragma once

#include "orb_home.h"

/* One Home write the presenter decided on after its own physical-input and
 * availability guards. A host may carry it as a typed screen intent; without
 * a host the presenter calls the unchanged service request directly. */
typedef enum {
    ORB_HOME_COMMAND_LEVEL = 1, /* a dial value, the only rim-eligible write */
    ORB_HOME_COMMAND_SET,       /* one discrete field: mode, lock or unlock */
    ORB_HOME_COMMAND_TOGGLE,
    ORB_HOME_COMMAND_SCENE,
    ORB_HOME_COMMAND_ROOM_OFF,
} orb_home_command_kind_t;

typedef struct {
    orb_home_command_kind_t kind;
    const char *id;
    /* LEVEL/SET: exactly one writable field is present. */
    orb_home_state_t values;
    bool deliberate_unlock;
} orb_home_command_t;

typedef struct {
    bool (*send)(void *context, const orb_home_command_t *command);
    void *context;
} orb_home_commands_t;

/* The service result is returned unchanged, so presenter feedback follows
 * the same acceptance it always did. UI thread only. */
bool orb_home_command(const orb_home_commands_t *port, const orb_home_command_t *command);
bool orb_home_command_execute(const orb_home_command_t *command);
