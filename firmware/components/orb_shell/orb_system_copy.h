#pragma once

#include "orb_shell.h"
#include "orb_nav.h"
#include "orb_delight_motion.h"

/* Settings > Mac copy derived only from the public status snapshot (polish
 * review #5/#6): values are states, never verbs; each disconnected state has
 * its own status and fix; status is NULL once Wi-Fi and the Mac are online. */
typedef struct {
    const char *wifi, *mac, *status, *fix;
} orb_system_connection_copy_t;

orb_system_connection_copy_t orb_system_connection_copy(const orb_shell_snapshot_t *m);

/* About uses a compact summary, including the model's synchronization state.
 * Keep that vocabulary separate from the detailed Mac diagnostics above. */
const char *orb_system_connection_summary(const orb_shell_snapshot_t *status, bool online, bool synced);

void orb_system_software_version(char *out, size_t cap, const char *build);

/* Only visible About status changes retire its cached composition. */
bool orb_system_about_changed(const orb_shell_snapshot_t *a, const orb_shell_snapshot_t *b);

/* Parse the existing local clock caption for content/preview projections. */
bool orb_system_clock_parts(const char *clock, unsigned *hour, unsigned *minute);

/* Exact legacy identity mapping; the material/content palette stays caller-owned. */
orb_delight_color_t orb_system_surface_accent(orb_shell_surface_t surface);
const char *orb_system_surface_icon(orb_shell_surface_t surface);
const char *orb_system_surface_title(orb_shell_surface_t surface);
bool orb_system_surface_uses_system_chrome(orb_shell_surface_t surface);

/* Compass selection maps only to the four existing named places. */
orb_shell_surface_t orb_system_compass_destination(orb_nav_compass_selection_t selection);

/* Preserve Home daypart fallback at noon while the local clock is unknown. */
unsigned orb_system_home_hour(const char *clock);
