#pragma once

#include "orb_power.h"
#include "orb_idle_policy.h"
#include "orb_shell.h"

/* Follows the display power state from the UI task: radio power hints, work
 * that must keep the radio prompt, and the cell-protection cutoff. */
void orb_power_services_apply(orb_power_snapshot_t power, bool speech);
/* Main owner updates the fail-closed runtime gate from live service facts. */
orb_idle_policy_t orb_power_services_policy(orb_shell_t *shell, bool power_valid, bool usb);
/* Poll only Time while eligible, retaining its exact monotonic/wall deadlines. */
int64_t orb_power_services_deadline(int64_t now);
