#pragma once

/* Shared between orb_net's files; not part of the API. */

#include <stdbool.h>

#include "esp_err.h"

#define ORB_NET_NVS_NAMESPACE "orb_net"   /* tz (orb_net_time.c), verified (orb_net.c) */

/* orb_net.c: true while the station has an IP address. */
bool orb_net_is_connected(void);

void orb_net_power_wake(void);
void orb_net_power_provisioning(bool active);
void orb_net_power_apply(void);
void orb_net_count_scan(void);
void orb_net_count_join(void);

/* orb_net_time.c: applies the time zone saved by orb_net_set_timezone, so
 * localtime() is right from boot, before the hub says anything. */
void orb_net_time_restore(void);

/* orb_net_time.c: starts SNTP; later calls do nothing. The clock is set when
 * NETIF_SNTP_EVENT arrives. */
void orb_net_time_start(void);
