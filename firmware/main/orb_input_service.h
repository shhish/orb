#pragma once

#include "orb_board.h"
#include "orb_idle_policy.h"
#include "orb_shell.h"

typedef struct {
    orb_accel_t gravity;
    int64_t at_us;
    orb_senses_result_t senses;
    bool motion;
    uint32_t next_poll_ms;
} orb_input_motion_t;

/* Main task owns sampling and recognizer state. Changing profile discards
 * gyro bias and gesture accumulation; pick-up uses its independent detector. */
orb_input_motion_t orb_input_motion(orb_idle_policy_t policy, bool screen_on, orb_shell_t *shell);
