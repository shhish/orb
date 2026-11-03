#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Shared with the real-LVGL host regression: retained damage cannot become
 * rendering or a flush until the requested panel is ready for a safe frame. */
static inline uint32_t orb_display_power_refresh_delay(bool panel_on, bool ready)
{
    return panel_on && ready ? 0 : UINT32_MAX;
}

static inline bool orb_display_te_fresh(int64_t now, int64_t fall, int64_t period, int64_t blank)
{
    int64_t age = now - fall;
    return fall > 0 && period >= 8000 && period <= 40000 && blank > 0 && blank < period &&
           age >= 0 && age < 4 * period;
}
