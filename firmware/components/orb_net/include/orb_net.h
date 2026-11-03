#pragma once

/*
 * Wi-Fi for Orb: BLE provisioning when there are no credentials, then a
 * station that keeps itself connected, mDNS (to find the hub) and SNTP. See
 * docs/protocol.md, "Discovery" and "Wi-Fi setup".
 *
 * Needs nvs_flash_init() and esp_event_loop_create_default() first.
 * Callbacks run on orb_net's own task; they must not block and must take
 * lv_lock() before touching the UI.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ORB_NET_UNPROVISIONED,   /* no credentials and not advertising (BLE off) */
    ORB_NET_PROVISIONING,    /* advertising over BLE; service_name and pop are set */
    ORB_NET_CONNECTING,      /* credentials stored, joining or rejoining */
    ORB_NET_CONNECTED,       /* associated with an IP address */
    ORB_NET_FAILED,          /* credentials rejected (wrong password / no such network); while
                                provisioning, service_name and pop stay set for another try */
} orb_net_state_t;

typedef struct {
    orb_net_state_t state;
    char ssid[33];
    char ip[16];
    int rssi;                /* dBm, 0 when not connected */
    char service_name[16];   /* "Orb-XXXX", while provisioning */
    char pop[9];             /* 8 digits to show on screen, while provisioning */
    bool time_synced;        /* SNTP has set the clock */
} orb_net_status_t;

typedef void (*orb_net_cb_t)(const orb_net_status_t *status, void *ctx);

/* Starts Wi-Fi: joins with stored credentials, or starts BLE provisioning if
 * there are none. cb is called on every state change. */
esp_err_t orb_net_start(orb_net_cb_t cb, void *ctx);

esp_err_t orb_net_status(orb_net_status_t *out);

typedef enum {
    ORB_NET_POWER_INTERACTIVE,
    ORB_NET_POWER_BACKGROUND,
} orb_net_power_hint_t;

/* Non-blocking, task-safe, including before start. The net task applies modem
 * policy; provisioning and active transfers override BACKGROUND to INTERACTIVE. */
void orb_net_set_power_hint(orb_net_power_hint_t hint);
orb_net_power_hint_t orb_net_get_power_hint(void);
/* Monotonic transition token: retain prompt intent across rapid hint changes. */
uint32_t orb_net_interactive_generation(void);

enum { ORB_NET_WORK_OTA = 1 << 0, ORB_NET_WORK_VOICE = 1 << 1 };
void orb_net_set_work_active(unsigned work, bool active);

/* One link-owned observer. Called outside locks on the changing task; it must
 * only wake a waiter, never block. Read the getter after waking. */
typedef void (*orb_net_power_cb_t)(void *ctx);
void orb_net_on_power_hint(orb_net_power_cb_t cb, void *ctx);

typedef struct {
    uint64_t scans;        /* join scan attempts plus explicit SCAN_DONE events */
    uint64_t reconnects;   /* successful addresses after the first one */
} orb_net_counters_t;
void orb_net_get_counters(orb_net_counters_t *out);

/* Forgets the stored network and starts provisioning again. Bluetooth's memory
 * is released once there is a network, so this restarts the device (0.5 s
 * after reporting UNPROVISIONED). */
esp_err_t orb_net_forget(void);

/* "orb-" + the station MAC as 12 lowercase hex digits. */
const char *orb_net_device_id(void);

typedef struct {
    char name[64];       /* mDNS instance name, e.g. "Mac mini" */
    char host[64];       /* "orb.local" */
    char ip[16];         /* resolved IPv4 address */
    uint16_t port;       /* TLS port (8443) */
    uint16_t pair_port;  /* TXT pair= (8080) */
    char fp[17];         /* TXT fp=: first 16 hex chars of the certificate SHA-256 */
} orb_hub_addr_t;

/*
 * Browses for _orb-hub._tcp (docs/protocol.md, "Discovery"). Initialises mDNS
 * only for the duration of the query and frees it right after. Serialised so
 * concurrent callers cannot race. Fills up to max results and sets *count.
 * ESP_ERR_NOT_FOUND if none answered within timeout_ms. Only while connected.
 */
esp_err_t orb_net_find_hubs(orb_hub_addr_t *out, int max, int *count, int timeout_ms);

/* Sets the POSIX TZ used by localtime() from an IANA name ("America/New_York"):
 * the built-in table first, then the hub's POSIX rule (NULL if it sent none),
 * then UTC. Both are kept in NVS and applied at the next boot. */
void orb_net_set_timezone(const char *iana, const char *tz_posix);
