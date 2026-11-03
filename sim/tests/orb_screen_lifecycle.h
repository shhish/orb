#pragma once

#include "orb_screen.h"

/* Applications supply projections and semantic assertions. The driver owns
 * ordering so every descriptor faces the same lifetime/restore contract. */
typedef enum {
    ORB_LIFECYCLE_INITIAL,
    ORB_LIFECYCLE_IDEMPOTENT,
    ORB_LIFECYCLE_CHANGED,
    ORB_LIFECYCLE_RESTORED,
    ORB_LIFECYCLE_REMOVED_ID,
    ORB_LIFECYCLE_EMPTY,
} orb_lifecycle_phase_t;

typedef struct {
    const orb_screen_descriptor_t *descriptor;
    const orb_screen_args_t *args;
    const orb_screen_update_t *initial;
    const orb_screen_update_t *changed;
    const orb_screen_update_t *removed;
    const orb_screen_update_t *empty;
    void (*exercise)(orb_screen_instance_t *instance);
    void (*verify)(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase);
    void (*quiescent)(void);
} orb_lifecycle_case_t;

/* Call with an isolated UI host and an empty context store. Tests use semantic
 * state/IDs only; the shared driver never indexes a presenter's LVGL children. */
void orb_test_screen_lifecycle(orb_screen_host_t *host, const orb_lifecycle_case_t *test);
