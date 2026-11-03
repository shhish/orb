#pragma once

#include "orb_link.h"

#include "link_proto.h"

void link_device_info(link_device_t *device, char build[8]);

const char *link_device_display_state_name(orb_link_display_state_t state);
