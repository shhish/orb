#pragma once

#include "orb_shell.h"

/* Service values are sampled before main merges its lock-protected mailbox. */
void orb_main_status_project(orb_shell_snapshot_t *view, int64_t now,
    const char *service_error, const char *action_error, int64_t action_error_until, bool demo);
