#pragma once

/* First-crossing internal-heap attribution, compiled in only when the build
 * enables CONFIG_HEAP_USE_HOOKS; otherwise the report prints nothing. */
typedef int (*orb_devtools_heaptrace_reply_t)(const char *format, ...);

void orb_devtools_heaptrace_report(orb_devtools_heaptrace_reply_t reply);
