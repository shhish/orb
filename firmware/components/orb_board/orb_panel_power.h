#pragma once

#include <stdbool.h>
#include <stdint.h>

/* CO5300 DCS ordering, independent of SPI and wall-clock time. Commands become
 * state only after their drained transfer succeeds. Reversing a request keeps
 * the last sleep command's 120 ms exclusion window. */
typedef struct {
    bool requested, sleeping, display_on;
    int64_t ready_us;
} orb_panel_power_t;

static inline bool orb_panel_power_ready(const orb_panel_power_t *panel, bool awake)
{
    return panel->requested == awake && (awake ? !panel->sleeping && panel->display_on :
                                                               panel->sleeping && !panel->display_on);
}

static inline uint8_t orb_panel_power_next(const orb_panel_power_t *panel, int64_t now)
{
    if (!panel->requested && panel->display_on) return 0x28; /* DISPOFF first. */
    if (now < panel->ready_us) return 0;
    if (panel->requested && panel->sleeping) return 0x11;
    if (!panel->requested && !panel->sleeping) return 0x10;
    if (panel->requested && !panel->display_on) return 0x29;
    return 0;
}

static inline void orb_panel_power_done(orb_panel_power_t *panel, uint8_t command, int64_t now)
{
    if (command == 0x10 || command == 0x11) {
        panel->sleeping = command == 0x10;
        panel->ready_us = now + 120000;
    } else {
        panel->display_on = command == 0x29;
    }
}
