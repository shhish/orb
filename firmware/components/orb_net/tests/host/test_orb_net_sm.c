/*
 * Host tests for orb_net's connection state machine (orb_net_sm.c): the event
 * sequences ESP-IDF and network_provisioning 1.4.0 produce, and random ones
 * against invariants. Run with `make` in this directory.
 */
#include <stdlib.h>

#include "check.h"
#include "orb_net_sm.h"

static orb_net_sm_t sm;

static unsigned ev(orb_net_sm_ev_type_t type)
{
    const orb_net_sm_ev_t e = { .type = type };
    return orb_net_sm_step(&sm, &e);
}

static unsigned drop(bool refused)
{
    const orb_net_sm_ev_t e = { .type = ORB_NET_SM_DISCONNECTED, .refused = refused };
    return orb_net_sm_step(&sm, &e);
}

static unsigned prov_end(bool stored)
{
    const orb_net_sm_ev_t e = { .type = ORB_NET_SM_PROV_END, .stored = stored };
    return orb_net_sm_step(&sm, &e);
}

/* A stored network at boot, joined once the station starts. */
static void boot_stored(void)
{
    orb_net_sm_init(&sm, ORB_NET_CONNECTING, true);
    CHECK(ev(ORB_NET_SM_STA_START) == ORB_NET_DO_JOIN);
}

static void test_boot_stored(void)
{
    boot_stored();
    CHECK(sm.state == ORB_NET_CONNECTING && sm.failures == 0);
    CHECK(ev(ORB_NET_SM_STA_START) == 0);   /* one join per boot */
    CHECK(ev(ORB_NET_SM_GOT_IP) == (ORB_NET_DO_CANCEL_JOIN | ORB_NET_DO_PUBLISH | ORB_NET_DO_START_TIME));
    CHECK(sm.state == ORB_NET_CONNECTED);
    CHECK(ev(ORB_NET_SM_CREDENTIALS) == 0);   /* only while provisioning */
    CHECK(ev(ORB_NET_SM_CREDENTIALS_FAILED) == 0);
    CHECK(prov_end(true) == 0);
}

static void test_refusals(void)
{
    boot_stored();
    CHECK(drop(true) == ORB_NET_DO_JOIN);
    CHECK(drop(true) == ORB_NET_DO_JOIN);
    CHECK(sm.state == ORB_NET_CONNECTING && sm.failures == 2);
    CHECK(drop(true) == (ORB_NET_DO_JOIN | ORB_NET_DO_PUBLISH));
    CHECK(sm.state == ORB_NET_FAILED);
    CHECK(drop(true) == ORB_NET_DO_JOIN);   /* retries go on, the app hears nothing new */
    CHECK(sm.state == ORB_NET_FAILED && sm.failures == 4);
    CHECK(ev(ORB_NET_SM_GOT_IP) & ORB_NET_DO_PUBLISH);
    CHECK(sm.state == ORB_NET_CONNECTED && sm.failures == 0 && sm.refusals == 0);
}

static void test_absent_streak(void)
{
    boot_stored();
    const orb_net_sm_ev_t absent = {
        .type = ORB_NET_SM_DISCONNECTED, .refused = true, .absent = true,
    };
    for (unsigned i = 1; i <= 9; i++) {
        CHECK(orb_net_sm_step(&sm, &absent) & ORB_NET_DO_JOIN);
        CHECK(sm.absent == i && sm.failures == i);
    }
    CHECK(sm.state == ORB_NET_FAILED && sm.refusals == 9);
    CHECK(drop(true) == ORB_NET_DO_JOIN);   /* auth failure retains refusal semantics */
    CHECK(sm.absent == 0 && sm.refusals == 10);
    CHECK(orb_net_sm_step(&sm, &absent) & ORB_NET_DO_JOIN);
    CHECK(sm.absent == 1);
    CHECK(ev(ORB_NET_SM_JOIN_ERROR) & ORB_NET_DO_JOIN);
    CHECK(sm.absent == 0);
    CHECK(orb_net_sm_step(&sm, &absent) & ORB_NET_DO_JOIN);
    CHECK(ev(ORB_NET_SM_GOT_IP) & ORB_NET_DO_CANCEL_JOIN);
    CHECK(sm.absent == 0 && sm.failures == 0 && sm.refusals == 0);
    CHECK(orb_net_sm_step(&sm, &absent) & ORB_NET_DO_JOIN);
    CHECK(sm.absent == 0 && sm.failures == 0);   /* link loss itself retries immediately */

    orb_net_sm_init(&sm, ORB_NET_PROVISIONING, false);
    ev(ORB_NET_SM_CREDENTIALS);
    CHECK(orb_net_sm_step(&sm, &absent) == 0);   /* manager owns setup retries */
    CHECK(sm.absent == 0);
    ev(ORB_NET_SM_GOT_IP);
    CHECK(sm.absent == 0);
}

static void test_mixed_reasons(void)
{
    /* A flaky link with the odd refusal among other reasons: never FAILED. */
    boot_stored();
    for (int i = 0; i < 10; i++) {
        CHECK(drop(true) == ORB_NET_DO_JOIN);
        CHECK(drop(false) == ORB_NET_DO_JOIN);
        CHECK(sm.state == ORB_NET_CONNECTING);
    }
    CHECK(sm.failures == 20);   /* the backoff keeps growing all the same */
    CHECK(drop(true) == ORB_NET_DO_JOIN);
    CHECK(drop(true) == ORB_NET_DO_JOIN);
    CHECK(drop(false) == ORB_NET_DO_JOIN);   /* breaks the run */
    CHECK(drop(true) == ORB_NET_DO_JOIN);
    CHECK(drop(true) == ORB_NET_DO_JOIN);
    CHECK(sm.state == ORB_NET_CONNECTING);
    CHECK(drop(true) == (ORB_NET_DO_JOIN | ORB_NET_DO_PUBLISH));
    CHECK(sm.state == ORB_NET_FAILED);
    /* Any other failure means the network is there and isn't refusing. */
    CHECK(drop(false) == (ORB_NET_DO_JOIN | ORB_NET_DO_PUBLISH));
    CHECK(sm.state == ORB_NET_CONNECTING && sm.refusals == 0);
}

static void test_link_loss(void)
{
    boot_stored();
    ev(ORB_NET_SM_GOT_IP);
    /* Whatever the reason, losing a working network is a rejoin at once. */
    CHECK(drop(true) == (ORB_NET_DO_JOIN | ORB_NET_DO_PUBLISH));
    CHECK(sm.state == ORB_NET_CONNECTING && sm.failures == 0 && sm.refusals == 0);
    CHECK(drop(false) == ORB_NET_DO_JOIN);
    CHECK(sm.failures == 1);
    CHECK(ev(ORB_NET_SM_GOT_IP) & ORB_NET_DO_CANCEL_JOIN);
    CHECK(sm.state == ORB_NET_CONNECTED);
}

static void test_join_error(void)
{
    boot_stored();
    CHECK(ev(ORB_NET_SM_JOIN_ERROR) == ORB_NET_DO_JOIN);
    CHECK(sm.failures == 1 && sm.state == ORB_NET_CONNECTING);

    orb_net_sm_init(&sm, ORB_NET_UNPROVISIONED, false);
    CHECK(ev(ORB_NET_SM_JOIN_ERROR) == 0);

    orb_net_sm_init(&sm, ORB_NET_PROVISIONING, false);
    ev(ORB_NET_SM_CREDENTIALS);
    CHECK(ev(ORB_NET_SM_JOIN_ERROR) == 0);   /* the manager's joins, not ours */
}

static void test_provisioning(void)
{
    orb_net_sm_init(&sm, ORB_NET_PROVISIONING, false);
    CHECK(sm.provisioning);
    CHECK(ev(ORB_NET_SM_STA_START) == 0);   /* started for the scan */

    /* Wrong password: the manager tries 3 times, then the client may retry. */
    CHECK(ev(ORB_NET_SM_CREDENTIALS) == (ORB_NET_DO_CANCEL_JOIN | ORB_NET_DO_PUBLISH));
    CHECK(sm.state == ORB_NET_CONNECTING);
    for (int i = 0; i < 3; i++) {
        CHECK(drop(true) == 0);
    }
    CHECK(sm.state == ORB_NET_CONNECTING);
    CHECK(ev(ORB_NET_SM_CREDENTIALS_FAILED) == (ORB_NET_DO_RESET_PROV | ORB_NET_DO_PUBLISH));
    CHECK(sm.state == ORB_NET_FAILED && sm.provisioning);

    /* The right one, on the second attempt. */
    CHECK(ev(ORB_NET_SM_CREDENTIALS) == (ORB_NET_DO_CANCEL_JOIN | ORB_NET_DO_PUBLISH));
    CHECK(drop(true) == 0);
    CHECK(ev(ORB_NET_SM_GOT_IP) ==
          (ORB_NET_DO_CANCEL_JOIN | ORB_NET_DO_PUBLISH | ORB_NET_DO_START_TIME | ORB_NET_DO_SAVE_VERIFIED));
    CHECK(sm.verified && sm.prov_joined);

    /* The client read "connected"; provisioning stops. */
    CHECK(prov_end(true) == (ORB_NET_DO_END_PROV | ORB_NET_DO_PUBLISH));
    CHECK(!sm.provisioning && sm.state == ORB_NET_CONNECTED);
    CHECK(prov_end(true) == 0);

    /* From here on the joins are ours. */
    CHECK(drop(false) == (ORB_NET_DO_JOIN | ORB_NET_DO_PUBLISH));
}

static void test_cred_fail_after_success(void)
{
    /* Two failed joins, then an address: the manager has counted 2 of its 3
     * attempts and doesn't reset the count on success. */
    orb_net_sm_init(&sm, ORB_NET_PROVISIONING, false);
    ev(ORB_NET_SM_CREDENTIALS);
    CHECK(drop(true) == 0);
    CHECK(drop(true) == 0);
    ev(ORB_NET_SM_GOT_IP);
    CHECK(sm.state == ORB_NET_CONNECTED);

    /* One drop before provisioning stops: the manager says CRED_FAIL. */
    CHECK(drop(false) == ORB_NET_DO_PUBLISH);
    CHECK(sm.state == ORB_NET_CONNECTING);
    unsigned todo = ev(ORB_NET_SM_CREDENTIALS_FAILED);
    CHECK(!(todo & ORB_NET_DO_RESET_PROV));   /* the working network stays stored */
    CHECK(todo == ORB_NET_DO_JOIN && sm.failures == 0);
    CHECK(sm.state == ORB_NET_CONNECTING && sm.prov_rejoin);

    /* Every later drop: a disconnect (ours to rejoin), then CRED_FAIL again. */
    CHECK(drop(false) == ORB_NET_DO_JOIN);
    CHECK(sm.failures == 1);
    CHECK(ev(ORB_NET_SM_CREDENTIALS_FAILED) == 0);
    CHECK(ev(ORB_NET_SM_GOT_IP) == (ORB_NET_DO_CANCEL_JOIN | ORB_NET_DO_PUBLISH | ORB_NET_DO_START_TIME));
    CHECK(drop(false) == (ORB_NET_DO_JOIN | ORB_NET_DO_PUBLISH));
    CHECK(ev(ORB_NET_SM_CREDENTIALS_FAILED) == 0);

    /* The stop timer from the first success: our join is already scheduled. */
    CHECK(prov_end(true) == (ORB_NET_DO_END_PROV | ORB_NET_DO_PUBLISH));
    CHECK(!sm.provisioning && !sm.prov_rejoin && sm.state == ORB_NET_CONNECTING);
    CHECK(drop(false) == ORB_NET_DO_JOIN);
}

static void test_prov_end_while_disconnected(void)
{
    /* The manager was still retrying when provisioning stopped. */
    orb_net_sm_init(&sm, ORB_NET_PROVISIONING, false);
    ev(ORB_NET_SM_CREDENTIALS);
    ev(ORB_NET_SM_GOT_IP);
    drop(false);
    CHECK(prov_end(true) == (ORB_NET_DO_END_PROV | ORB_NET_DO_PUBLISH | ORB_NET_DO_JOIN));
    CHECK(sm.state == ORB_NET_CONNECTING && sm.failures == 0);
}

static void test_prov_end_nothing_stored(void)
{
    /* The network worked, the link dropped, and the client sent prov-ctrl
     * reset, which erased it; then the stop timer ran out. */
    orb_net_sm_init(&sm, ORB_NET_PROVISIONING, false);
    ev(ORB_NET_SM_CREDENTIALS);
    ev(ORB_NET_SM_GOT_IP);
    drop(false);
    ev(ORB_NET_SM_CREDENTIALS_FAILED);
    CHECK(prov_end(false) == (ORB_NET_DO_END_PROV | ORB_NET_DO_PUBLISH | ORB_NET_DO_CANCEL_JOIN | ORB_NET_DO_RESTART));
    CHECK(sm.state == ORB_NET_UNPROVISIONED && !sm.provisioning);
}

static void test_prov_start_failed(void)
{
    /* No network and no provisioning: nothing joins, whatever Wi-Fi says. */
    orb_net_sm_init(&sm, ORB_NET_UNPROVISIONED, false);
    CHECK(ev(ORB_NET_SM_STA_START) == 0);
    CHECK(drop(false) == 0);
    CHECK(drop(true) == 0);
    CHECK(ev(ORB_NET_SM_CREDENTIALS) == 0);
    CHECK(ev(ORB_NET_SM_CREDENTIALS_FAILED) == 0);
    CHECK(prov_end(false) == 0);
    CHECK(sm.state == ORB_NET_UNPROVISIONED);

    /* A stored network that never worked, and provisioning failed: try it.
     * The failed attempt's station start is queued ahead of ours. */
    orb_net_sm_init(&sm, ORB_NET_CONNECTING, false);
    CHECK(ev(ORB_NET_SM_STA_START) == ORB_NET_DO_JOIN);
    CHECK(ev(ORB_NET_SM_STA_START) == 0);
    CHECK(ev(ORB_NET_SM_GOT_IP) & ORB_NET_DO_SAVE_VERIFIED);
    CHECK(sm.verified);
}

static void test_verified(void)
{
    orb_net_sm_init(&sm, ORB_NET_PROVISIONING, false);
    CHECK(!(ev(ORB_NET_SM_CREDENTIALS) & ORB_NET_DO_SAVE_VERIFIED));
    CHECK(ev(ORB_NET_SM_GOT_IP) & ORB_NET_DO_SAVE_VERIFIED);
    CHECK(sm.verified);
    drop(false);
    CHECK(!(ev(ORB_NET_SM_GOT_IP) & ORB_NET_DO_SAVE_VERIFIED));   /* written once */
    /* Reset by the client and new credentials in the same session: untried again. */
    CHECK(ev(ORB_NET_SM_CREDENTIALS) & ORB_NET_DO_SAVE_VERIFIED);
    CHECK(!sm.verified);
}

static bool joins_ours(const orb_net_sm_t *s)
{
    return (!s->provisioning || s->prov_rejoin) && (s->state == ORB_NET_CONNECTING || s->state == ORB_NET_FAILED);
}

/* Random events, possible or not, against what must always hold. */
static void test_random(void)
{
    static const orb_net_state_t starts[] = { ORB_NET_CONNECTING, ORB_NET_PROVISIONING, ORB_NET_UNPROVISIONED };
    srand(1775);
    int bad = 0;
    for (int run = 0; run < 20000 && !bad; run++) {
        orb_net_sm_init(&sm, starts[rand() % 3], rand() & 1);
        for (int i = 0; i < 64 && !bad; i++) {
            const orb_net_sm_t was = sm;
            const orb_net_sm_ev_t e = {
                .type = (orb_net_sm_ev_type_t)(rand() % (ORB_NET_SM_PROV_END + 1)),
                .refused = rand() & 1,
                .stored = rand() % 4 != 0,
            };
            const unsigned todo = orb_net_sm_step(&sm, &e);
            bool ok = true;
            ok &= sm.state != ORB_NET_PROVISIONING || sm.provisioning;
            ok &= sm.provisioning || (!sm.prov_joined && !sm.prov_rejoin);
            ok &= !sm.prov_rejoin || sm.prov_joined;
            ok &= sm.state == was.state || (todo & ORB_NET_DO_PUBLISH);
            ok &= !(todo & ORB_NET_DO_JOIN) || joins_ours(&sm);
            ok &= !((todo & ORB_NET_DO_JOIN) && (todo & ORB_NET_DO_CANCEL_JOIN));
            ok &= !(todo & ORB_NET_DO_RESET_PROV) ||
                  (e.type == ORB_NET_SM_CREDENTIALS_FAILED && sm.provisioning && !sm.prov_joined &&
                   sm.state == ORB_NET_FAILED);
            ok &= !(todo & ORB_NET_DO_RESTART) ||
                  (e.type == ORB_NET_SM_PROV_END && !e.stored && sm.state == ORB_NET_UNPROVISIONED);
            ok &= !(todo & ORB_NET_DO_END_PROV) || (was.provisioning && !sm.provisioning);
            ok &= !!(todo & ORB_NET_DO_SAVE_VERIFIED) == (sm.verified != was.verified);
            /* FAILED outside the manager's own attempts means 3 refusals in a row. */
            ok &= sm.state != ORB_NET_FAILED || (sm.provisioning && !sm.prov_rejoin) ||
                  sm.refusals >= ORB_NET_REFUSALS_TO_FAIL;
            if (!ok) {
                fprintf(stderr, "run %d step %d: event %d from state %d -> state %d, todo 0x%x\n", run, i, e.type,
                        was.state, sm.state, todo);
                bad = 1;
            }
        }
    }
    CHECK(!bad);
}

int main(void)
{
    test_boot_stored();
    test_refusals();
    test_absent_streak();
    test_mixed_reasons();
    test_link_loss();
    test_join_error();
    test_provisioning();
    test_cred_fail_after_success();
    test_prov_end_while_disconnected();
    test_prov_end_nothing_stored();
    test_prov_start_failed();
    test_verified();
    test_random();
    return check_report();
}
