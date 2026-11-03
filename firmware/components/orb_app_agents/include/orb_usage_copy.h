#pragma once

#include <stdbool.h>

#include "orb_model.h"

/* One provider's Usage page, derived only from the model snapshot. Its text
 * and both rings read this one selection (polish review #4): the outer ring
 * is main, the focal value (5 hours, another duration, or the week when that
 * is all the provider reports); week_ring is a separate week, else NULL. */
typedef struct {
    const orb_usage_window_t *main, *week_ring;
    char value[96], reset[64], weekly[96], week_reset[32];
    const char *credits;
    bool stale;
} orb_usage_page_t;

void orb_usage_page(const orb_usage_snapshot_t *usage, unsigned provider, bool ready, bool ax3,
                    orb_usage_page_t *out);
/* "5 hours" -> "5 hr": glossary §1.6 compact units for a window's name. */
void orb_usage_window_name(char *out, size_t cap, const char *label);
/* "Wed 11:59 PM" (day) or "11:59 PM", or "—" when unknown. */
void orb_usage_time(char *out, size_t cap, uint64_t at, bool day);
