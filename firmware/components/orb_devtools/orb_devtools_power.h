#pragma once
#include <stdbool.h>
#include <stdio.h>

typedef int (*orb_devtools_power_printf_fn)(const char *, ...);
typedef bool (*orb_devtools_power_reply_fn)(const char *, ...);
void orb_devtools_power_report(FILE *out, orb_devtools_power_printf_fn output);
bool orb_devtools_lightsleep_command(const char *cmd, orb_devtools_power_reply_fn output);
void orb_devtools_crash_report(orb_devtools_power_printf_fn output);
