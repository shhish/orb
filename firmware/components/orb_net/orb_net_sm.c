/* orb_net's connection state machine (orb_net_sm.h). */
#include "orb_net_sm.h"

#include <string.h>

void orb_net_sm_init(orb_net_sm_t *sm, orb_net_state_t state, bool verified)
{
    memset(sm, 0, sizeof(*sm));
    sm->state = state;
    sm->provisioning = state == ORB_NET_PROVISIONING;
    sm->verified = verified;
}

/* The next join is orb_net's: one is due, and it isn't the provisioning
 * manager's (it gave up on credentials that worked, or isn't running). */
static bool joins_ours(const orb_net_sm_t *sm)
{
    return (!sm->provisioning || sm->prov_rejoin) && (sm->state == ORB_NET_CONNECTING || sm->state == ORB_NET_FAILED);
}

static unsigned on_disconnected(orb_net_sm_t *sm, bool refused, bool absent)
{
    if (sm->provisioning && !sm->prov_rejoin) {
        /* The manager retries the credentials itself and reports CRED_FAIL
         * when it gives up. */
        if (sm->state == ORB_NET_CONNECTED) {
            sm->state = ORB_NET_CONNECTING;
        }
        return 0;
    }
    if (sm->state == ORB_NET_CONNECTED) {
        sm->failures = sm->refusals = sm->absent = 0;   /* rejoin at once */
        sm->state = ORB_NET_CONNECTING;
        return ORB_NET_DO_JOIN;
    }
    if (!joins_ours(sm)) {
        return 0;   /* nothing to join */
    }
    sm->failures++;
    sm->absent = absent ? sm->absent + 1 : 0;
    sm->refusals = refused ? sm->refusals + 1 : 0;
    if (sm->refusals >= ORB_NET_REFUSALS_TO_FAIL) {
        sm->state = ORB_NET_FAILED;
    } else if (sm->state == ORB_NET_FAILED) {
        /* This one wasn't a refusal, so the last few no longer say that the
         * password or the network is wrong: back to a plain retry. */
        sm->state = ORB_NET_CONNECTING;
    }
    return ORB_NET_DO_JOIN;
}

static unsigned on_credentials_failed(orb_net_sm_t *sm)
{
    if (!sm->provisioning) {
        return 0;
    }
    if (sm->prov_joined) {
        /* These credentials had an address, so the drops that used up the
         * manager's attempts were the link's. Resetting would erase a
         * working network; keep it and rejoin from here. Every later drop
         * repeats CRED_FAIL after its disconnect, which already rejoins. */
        if (sm->prov_rejoin) {
            return 0;
        }
        sm->prov_rejoin = true;
        sm->failures = sm->refusals = sm->absent = 0;
        sm->state = ORB_NET_CONNECTING;
        return ORB_NET_DO_JOIN;
    }
    /* The client reads why from the status, and new credentials can follow
     * in the same session. */
    sm->state = ORB_NET_FAILED;
    return ORB_NET_DO_RESET_PROV;
}

static unsigned on_prov_end(orb_net_sm_t *sm, bool stored)
{
    if (!sm->provisioning) {
        return 0;
    }
    bool joins_ours = sm->prov_rejoin;
    sm->provisioning = sm->prov_joined = sm->prov_rejoin = false;
    unsigned todo = ORB_NET_DO_END_PROV | ORB_NET_DO_PUBLISH;
    if (!stored) {
        /* The client reset the network after it had worked (prov-ctrl), and
         * the stop timer from that success ran out. Bluetooth is released
         * now, so only a restart gets back to provisioning. */
        sm->state = ORB_NET_UNPROVISIONED;
        return todo | ORB_NET_DO_CANCEL_JOIN | ORB_NET_DO_RESTART;
    }
    if (sm->state == ORB_NET_CONNECTED || joins_ours) {
        return todo;   /* connected, or a join of ours is already scheduled or under way */
    }
    /* The connection made during provisioning dropped before it ended. */
    sm->state = ORB_NET_CONNECTING;
    sm->failures = sm->refusals = sm->absent = 0;
    return todo | ORB_NET_DO_JOIN;
}

unsigned orb_net_sm_step(orb_net_sm_t *sm, const orb_net_sm_ev_t *ev)
{
    const orb_net_state_t before = sm->state;
    unsigned todo = 0;
    switch (ev->type) {
    case ORB_NET_SM_STA_START:
        /* Provisioning starts the station only to scan. A start made by a
         * provisioning attempt that then failed can be queued ahead of the
         * one for the stored network; one join is enough. */
        if (!sm->provisioning && sm->state == ORB_NET_CONNECTING && !sm->started) {
            sm->started = true;
            sm->failures = 0;
            todo = ORB_NET_DO_JOIN;
        }
        break;
    case ORB_NET_SM_DISCONNECTED:
        todo = on_disconnected(sm, ev->refused, ev->absent);
        break;
    case ORB_NET_SM_GOT_IP:
        sm->failures = sm->refusals = sm->absent = 0;
        sm->state = ORB_NET_CONNECTED;
        if (sm->provisioning) {
            sm->prov_joined = true;
        }
        todo = ORB_NET_DO_CANCEL_JOIN | ORB_NET_DO_PUBLISH | ORB_NET_DO_START_TIME;
        if (!sm->verified) {
            sm->verified = true;
            todo |= ORB_NET_DO_SAVE_VERIFIED;
        }
        break;
    case ORB_NET_SM_JOIN_ERROR:
        if (joins_ours(sm)) {
            sm->failures++;
            sm->absent = 0;
            todo = ORB_NET_DO_JOIN;
        }
        break;
    case ORB_NET_SM_CREDENTIALS:
        if (!sm->provisioning) {
            break;
        }
        /* New credentials, joined by the manager. They are stored already
         * and haven't worked yet. */
        sm->prov_joined = sm->prov_rejoin = false;
        sm->failures = sm->refusals = sm->absent = 0;
        sm->state = ORB_NET_CONNECTING;
        todo = ORB_NET_DO_CANCEL_JOIN | ORB_NET_DO_PUBLISH;
        if (sm->verified) {
            sm->verified = false;
            todo |= ORB_NET_DO_SAVE_VERIFIED;
        }
        break;
    case ORB_NET_SM_CREDENTIALS_FAILED:
        todo = on_credentials_failed(sm);
        break;
    case ORB_NET_SM_PROV_END:
        todo = on_prov_end(sm, ev->stored);
        break;
    }
    if (sm->state != before) {
        todo |= ORB_NET_DO_PUBLISH;
    }
    return todo;
}
