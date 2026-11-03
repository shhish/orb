#pragma once

#include <stdbool.h>

#include "orb_delight_motion.h"
#include "orb_agents_copy.h"

/* Battery state copy (polish review #20), pure so each
 * state's words, glyph and colour are chosen in one place. */
typedef struct {
    const char *glyph, *caption; /* glyph NULL: no symbol above the figure */
    orb_delight_color_t accent;
} orb_charging_copy_t;
orb_charging_copy_t orb_charging_copy(int battery_pct, bool usb, bool charging);
