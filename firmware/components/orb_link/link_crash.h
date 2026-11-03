#pragma once

#include <stdbool.h>

#include "cJSON.h"

/* Link-task owned; reconnects must not consume the boot's pending crash. */
bool link_crash_report(cJSON *report);
void link_crash_report_sent(void);
