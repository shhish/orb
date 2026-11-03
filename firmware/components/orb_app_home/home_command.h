#pragma once

#include "orb_home_command.h"

/* The presenter's command port: context is the owning screen instance. */
bool home_command_send(void *context, const orb_home_command_t *command);
