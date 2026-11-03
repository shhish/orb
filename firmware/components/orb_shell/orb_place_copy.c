#include "orb_place_copy.h"

#include <stddef.h>

#include "orb_icons.h"

/* The bolt means charging; the plug means USB power is connected. Words carry
 * every state and colour only repeats them (HIG §5); the subtitle never
 * repeats the place's title, and Low says so. */
orb_charging_copy_t orb_charging_copy(int battery_pct, bool usb, bool charging)
{
    if (charging) return (orb_charging_copy_t){ORB_ICON_LIGHTNING_FILL, "Charging", ORB_DELIGHT_GREEN};
    if (usb && battery_pct == 100) return (orb_charging_copy_t){ORB_ICON_PLUG, "Fully charged", ORB_DELIGHT_GREEN};
    if (usb) return (orb_charging_copy_t){ORB_ICON_PLUG, "Not charging", ORB_DELIGHT_GRAY};
    if (battery_pct < 0) return (orb_charging_copy_t){ORB_ICON_BATTERY_WARNING, "Battery unavailable", ORB_DELIGHT_GRAY};
    if (battery_pct <= 20) return (orb_charging_copy_t){ORB_ICON_BATTERY_LOW, "Low battery", ORB_DELIGHT_RED};
    return (orb_charging_copy_t){NULL, "On battery", ORB_DELIGHT_GRAY};
}

/* One glyph per meaning and the glossary's "Couldn’t" error pattern. An
 * offline Mac is never also "Loading…"; only More and Try again act. */

