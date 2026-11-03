#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Called only after explicit transcript confirmation. Bounded English grammar,
 * no allocation or network: unsupported/ambiguous speech creates nothing and
 * returns safe guidance. Relative times need the service's synchronized clock. */
bool orb_time_reminder_from_voice(const char *text, char *reason, size_t capacity);
