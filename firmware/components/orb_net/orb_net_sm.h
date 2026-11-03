#pragma once

/*
 * orb_net's connection state as plain logic: an event goes in, the state
 * changes and orb_net_sm_step says what orb_net.c has to do. No ESP-IDF here
 * (orb_net.h only needs esp_err_t), so tests/host runs event sequences on the
 * Mac.
 *
 * Who joins: orb_net.c joins a stored network, with backoff. While
 * provisioning, network_provisioning joins the credentials it was given, up
 * to its attempt count, and then reports NETWORK_PROV_WIFI_CRED_FAIL. It
 * keeps counting failed attempts after the credentials got an address, so a
 * link that drops a few times in the 30 s before provisioning stops ends in
 * CRED_FAIL as well. Such credentials are kept, and orb_net rejoins from then
 * on, as the manager no longer does.
 */

#include <stdbool.h>

#include "orb_net.h"

#define ORB_NET_REFUSALS_TO_FAIL 3   /* refused joins in a row before FAILED; retries go on */

typedef enum {
    ORB_NET_SM_STA_START,            /* WIFI_EVENT_STA_START */
    ORB_NET_SM_DISCONNECTED,         /* WIFI_EVENT_STA_DISCONNECTED */
    ORB_NET_SM_GOT_IP,               /* IP_EVENT_STA_GOT_IP */
    ORB_NET_SM_JOIN_ERROR,           /* esp_wifi_connect() failed: no disconnect event follows */
    ORB_NET_SM_CREDENTIALS,          /* NETWORK_PROV_WIFI_CRED_RECV (they are stored by now) */
    ORB_NET_SM_CREDENTIALS_FAILED,   /* NETWORK_PROV_WIFI_CRED_FAIL */
    ORB_NET_SM_PROV_END,             /* NETWORK_PROV_END */
} orb_net_sm_ev_type_t;

typedef struct {
    orb_net_sm_ev_type_t type;
    bool refused;   /* DISCONNECTED: wrong password or no such network, not a flaky link */
    bool absent;    /* DISCONNECTED: NO_AP_FOUND family, independently of refusal policy */
    bool stored;    /* PROV_END: a network is stored */
} orb_net_sm_ev_t;

/* What orb_net.c has to do after a step, as bits, in this order. */
enum {
    ORB_NET_DO_RESET_PROV = 1 << 0,      /* network_prov_mgr_reset_wifi_sm_state_on_failure(): erases the
                                            refused credentials and takes new ones in the same session */
    ORB_NET_DO_END_PROV = 1 << 1,        /* network_prov_mgr_deinit(); clear service_name and pop */
    ORB_NET_DO_SAVE_VERIFIED = 1 << 2,   /* store `verified` in NVS */
    ORB_NET_DO_CANCEL_JOIN = 1 << 3,     /* drop the scheduled join */
    ORB_NET_DO_JOIN = 1 << 4,            /* join after orb_net_backoff_ms(failures), 0 being at once;
                                            replaces a scheduled join */
    ORB_NET_DO_PUBLISH = 1 << 5,         /* the status changed: tell the app */
    ORB_NET_DO_START_TIME = 1 << 6,      /* start SNTP */
    ORB_NET_DO_RESTART = 1 << 7,         /* nothing stored and Bluetooth gone: restart into provisioning */
};

typedef struct {
    orb_net_state_t state;
    bool provisioning;   /* network_provisioning runs (BLE is up) */
    bool prov_joined;    /* provisioning: the credentials received have had an address */
    bool prov_rejoin;    /* provisioning: the manager gave up on credentials that worked; joins are ours */
    bool started;        /* the stored network was joined when the station started */
    bool verified;       /* the stored network has had an address (kept in NVS for the next boot) */
    unsigned failures;   /* failed joins in a row */
    unsigned refusals;   /* refused joins in a row */
    unsigned absent;     /* consecutive NO_AP_FOUND family failures */
} orb_net_sm_t;

/* state is where boot ended up: CONNECTING (a stored network, joined once the
 * station starts), PROVISIONING or UNPROVISIONED. verified as read from NVS. */
void orb_net_sm_init(orb_net_sm_t *sm, orb_net_state_t state, bool verified);

/* Returns ORB_NET_DO_* bits. */
unsigned orb_net_sm_step(orb_net_sm_t *sm, const orb_net_sm_ev_t *ev);
