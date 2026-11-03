#pragma once

#include "orb_time_command.h"

/* The presenter's command port: context is the owning screen instance. */
bool time_command_send(void *context, const orb_time_command_t *command);
