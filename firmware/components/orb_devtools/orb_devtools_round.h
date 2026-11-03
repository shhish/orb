#pragma once

#include "orb_display_cadence.h"
#include "orb_display.h"
#include "orb_ui.h"

typedef bool (*orb_devtools_round_reply_t)(const char *format, ...);

void orb_devtools_round_receipt(const char *surface, const orb_ui_scroll_diagnostics_t *ui,
                                const orb_display_cadence_t *cadence, orb_devtools_round_reply_t reply);

double orb_devtools_percentile_upper_ms(const uint32_t *before, const uint32_t *after,
    unsigned buckets, unsigned bucket_us, unsigned percentile);

/* Same bounded transport as the surrounding benchmark receipt. */
bool orb_devtools_distribution_report(const char *sample, const char *name,
    const orb_display_counters_t *before, const orb_display_counters_t *after,
    orb_devtools_round_reply_t reply);
