/* Deterministic policy deadlines, not measured battery life or board current. */
#include "orb_power.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks, s_cases;
#define CHECK(c) do { s_checks++; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static orb_power_snapshot_t snap(orb_power_controller_t *p) { return orb_power_get_snapshot(p); }
static void tick(orb_power_controller_t *p, uint64_t ms) { CHECK(orb_power_tick(p, ms)); }
static void wake(orb_power_controller_t *p, orb_power_wake_t source, uint64_t ms)
{
    CHECK(orb_power_wake(p, source, ms).accepted);
}
static void battery(orb_power_controller_t *p, int pct, bool usb, uint64_t ms)
{
    CHECK(orb_power_set_battery(p, pct, usb, false, ms));
}
static void ignored(orb_power_controller_t *p, uint64_t ms)
{
    wake(p, ORB_POWER_WAKE_PICKUP, ms);
    tick(p, ms + 9000);
    CHECK(snap(p).state == ORB_POWER_ALWAYS_ON || snap(p).state == ORB_POWER_SLEEP);
}

static void glance(void)
{
    orb_power_controller_t p;
    orb_power_init(&p, 1000);
    wake(&p, ORB_POWER_WAKE_TOUCH, 1000);
    tick(&p, 31000);
    wake(&p, ORB_POWER_WAKE_PICKUP, 50000);
    CHECK(p.activity_ms == 1000 && snap(&p).redact);
    tick(&p, 50400); CHECK(snap(&p).next_deadline_ms == 56000);
    tick(&p, 55999); CHECK(snap(&p).state == ORB_POWER_ACTIVE);
    tick(&p, 56000); CHECK(snap(&p).state == ORB_POWER_DIMMED && snap(&p).redact);
    tick(&p, 56250); CHECK(snap(&p).next_deadline_ms == 59000);
    tick(&p, 58999); CHECK(snap(&p).state == ORB_POWER_DIMMED);
    tick(&p, 59000); CHECK(snap(&p).state == ORB_POWER_ALWAYS_ON);
    CHECK(p.ignored_pickups == 1 && p.activity_ms == 1000);
    tick(&p, 120999); CHECK(snap(&p).state == ORB_POWER_ALWAYS_ON);
    tick(&p, 121000); CHECK(snap(&p).state == ORB_POWER_SLEEP);
    /* A glance near the original end cannot renew Always On. */
    orb_power_init(&p, 0);
    tick(&p, 119000); ignored(&p, 119000);
    CHECK(snap(&p).state == ORB_POWER_SLEEP);
    /* Dimmed and already asleep both return to Sleep after the glance. */
    orb_power_init(&p, 0); tick(&p, 20000); ignored(&p, 21000);
    CHECK(snap(&p).state == ORB_POWER_SLEEP);
    tick(&p, 30250); ignored(&p, 40000);
    CHECK(snap(&p).state == ORB_POWER_SLEEP && snap(&p).moment == ORB_POWER_MOMENT_SLEEP);
    tick(&p, 49250); CHECK(!snap(&p).panel_on);
    /* Late scheduling skips both dim and the entire fade. */
    wake(&p, ORB_POWER_WAKE_PICKUP, 60000);
    tick(&p, 90000); CHECK(!snap(&p).panel_on && p.ignored_pickups == 3);
}

static void deliberate(void)
{
    for (int reason = ORB_POWER_WAKE_TOUCH; reason <= ORB_POWER_WAKE_PWR; reason++) {
        orb_power_controller_t p;
        orb_power_init(&p, 0); tick(&p, 30000);
        wake(&p, ORB_POWER_WAKE_PICKUP, 31000);
        CHECK(orb_power_wake(&p, (orb_power_wake_t)reason, 38000).consume_input);
        CHECK(!p.automatic && p.activity_ms == 38000 && !snap(&p).redact);
        CHECK(!orb_power_wake(&p, (orb_power_wake_t)reason, 38001).consume_input);
        tick(&p, 58000); CHECK(snap(&p).state == ORB_POWER_ACTIVE);
        tick(&p, 58001); CHECK(snap(&p).state == ORB_POWER_DIMMED);
        tick(&p, 68001); CHECK(snap(&p).state == ORB_POWER_ALWAYS_ON && p.ignored_pickups == 0);
        tick(&p, 158001); CHECK(snap(&p).state == ORB_POWER_SLEEP);
    }
}

static void ignored_budget(void)
{
    orb_power_controller_t p;
    orb_power_init(&p, 0); tick(&p, 30000);
    ignored(&p, 31000); ignored(&p, 41000); ignored(&p, 51000);
    CHECK(p.ignored_pickups == 3 && p.pickup_suppressed_until_ms == 360000);
    CHECK(!orb_power_wake(&p, ORB_POWER_WAKE_PICKUP, 70000).accepted);
    wake(&p, ORB_POWER_WAKE_ATTENTION, 70000);
    tick(&p, 100000); CHECK(p.ignored_pickups == 3);
    CHECK(!orb_power_wake(&p, ORB_POWER_WAKE_PICKUP, 359999).accepted);
    const uint32_t durations[] = {600000, 1200000, 2400000, 3600000, 3600000};
    uint64_t next = 360000;
    for (unsigned i = 0; i < sizeof(durations) / sizeof(durations[0]); i++) {
        ignored(&p, next);
        CHECK(p.pickup_suppressed_until_ms == next + 9000 + durations[i]);
        CHECK(!orb_power_wake(&p, ORB_POWER_WAKE_PICKUP, p.pickup_suppressed_until_ms - 1).accepted);
        next = p.pickup_suppressed_until_ms;
    }
    wake(&p, ORB_POWER_WAKE_PWR, next - 1);
    CHECK(p.ignored_pickups == 0 && p.pickup_suppressed_until_ms == 0);
    tick(&p, next + 30000); ignored(&p, next + 30000);
    CHECK(p.ignored_pickups == 1);
    /* A delayed tick anchors suppression to the actual end, not poll time. */
    orb_power_init(&p, 0); tick(&p, 30000);
    ignored(&p, 31000); ignored(&p, 41000);
    wake(&p, ORB_POWER_WAKE_PICKUP, 51000); tick(&p, 500000);
    CHECK(p.pickup_suppressed_until_ms == 360000);
    wake(&p, ORB_POWER_WAKE_PICKUP, 500000);
}

static void usb_budget(void)
{
    orb_power_controller_t p;
    orb_power_init(&p, 0); tick(&p, 30000);
    ignored(&p, 31000); ignored(&p, 41000); ignored(&p, 51000);
    battery(&p, 80, true, 60001);
    CHECK(p.ignored_pickups == 0 && p.pickup_suppressed_until_ms == 0);
    CHECK(snap(&p).redact && snap(&p).state == ORB_POWER_ALWAYS_ON && p.activity_ms == 0);
    for (uint64_t ms = 70000; ms <= 110000; ms += 10000) ignored(&p, ms);
    CHECK(p.ignored_pickups == 0);
    wake(&p, ORB_POWER_WAKE_PICKUP, 120000);
    battery(&p, 80, false, 121000); tick(&p, 129000);
    CHECK(p.ignored_pickups == 0 && snap(&p).state == ORB_POWER_SLEEP);
    wake(&p, ORB_POWER_WAKE_PICKUP, 140000);
    battery(&p, 80, true, 141000); battery(&p, 80, false, 142000); tick(&p, 149000);
    CHECK(p.ignored_pickups == 0);
}

static void invisible_budget(void)
{
    orb_power_controller_t p;
    orb_power_init(&p, 0);
    orb_power_preferences_t preferences = p.preferences;
    preferences.brightness_pct = 0;
    CHECK(orb_power_set_preferences(&p, &preferences, 0));
    tick(&p, 120000);
    for (uint64_t ms = 130000; ms <= 170000; ms += 10000) ignored(&p, ms);
    CHECK(p.ignored_pickups == 0 && p.pickup_suppressed_until_ms == 0);
    preferences.brightness_pct = 70;
    CHECK(orb_power_set_preferences(&p, &preferences, 179001));
    ignored(&p, 180000);
    CHECK(p.ignored_pickups == 1);
}

static void budget_boundary(void)
{
    orb_power_controller_t p;
    uint64_t start = UINT64_MAX - 100000;
    orb_power_init(&p, start); tick(&p, start + 30000);
    ignored(&p, start + 31000); ignored(&p, start + 41000); ignored(&p, start + 51000);
    CHECK(p.ignored_pickups == 3 && p.pickup_suppressed_until_ms <= p.now_ms);
    wake(&p, ORB_POWER_WAKE_PICKUP, start + 61000);
    tick(&p, UINT64_MAX);
    CHECK(p.pickup_suppressed_until_ms != UINT64_MAX);
    wake(&p, ORB_POWER_WAKE_PICKUP, UINT64_MAX);
}

static void attention(void)
{
    orb_power_controller_t p;
    orb_power_init(&p, 0); tick(&p, 30000);
    wake(&p, ORB_POWER_WAKE_PICKUP, 31000);
    wake(&p, ORB_POWER_WAKE_ATTENTION, 38000); /* Supersedes the dimmed pickup. */
    tick(&p, 57999); CHECK(snap(&p).state == ORB_POWER_ACTIVE);
    tick(&p, 58000); CHECK(snap(&p).state == ORB_POWER_DIMMED);
    tick(&p, 68000); CHECK(snap(&p).state == ORB_POWER_ALWAYS_ON && p.ignored_pickups == 1);
    CHECK(p.activity_ms == 0);
    tick(&p, 120000); CHECK(snap(&p).state == ORB_POWER_SLEEP);
    /* A card during the short Active glance still needs its full 20s. */
    orb_power_init(&p, 0); tick(&p, 30000);
    wake(&p, ORB_POWER_WAKE_PICKUP, 31000);
    tick(&p, 31400); CHECK(snap(&p).moment == ORB_POWER_MOMENT_NONE);
    wake(&p, ORB_POWER_WAKE_ATTENTION, 32000);
    CHECK(p.ignored_pickups == 1 && p.activity_ms == 0 && snap(&p).moment == ORB_POWER_MOMENT_NONE);
    CHECK(!orb_power_wake(&p, ORB_POWER_WAKE_ATTENTION, 33000).accepted);
    CHECK(!orb_power_wake(&p, ORB_POWER_WAKE_PICKUP, 42000).accepted);
    tick(&p, 51999); CHECK(snap(&p).state == ORB_POWER_ACTIVE);
    tick(&p, 52000); CHECK(snap(&p).state == ORB_POWER_DIMMED);
    tick(&p, 62000); CHECK(snap(&p).state == ORB_POWER_ALWAYS_ON && p.ignored_pickups == 1);
    /* A low-battery card still gets 20 seconds to be noticed. */
    orb_power_init(&p, 0); battery(&p, 15, false, 0); tick(&p, 15000);
    wake(&p, ORB_POWER_WAKE_ATTENTION, 16000);
    tick(&p, 35999); CHECK(snap(&p).state == ORB_POWER_ACTIVE && snap(&p).brightness_pct == 30);
    tick(&p, 36000); CHECK(snap(&p).state == ORB_POWER_DIMMED);
    tick(&p, 46000); CHECK(snap(&p).state == ORB_POWER_ALWAYS_ON);
    tick(&p, 60000); CHECK(snap(&p).state == ORB_POWER_SLEEP);
    /* A pickup replacing an attention's Dimmed phase woke from Dimmed,
     * not the attention's earlier Always On origin. */
    orb_power_init(&p, 0); tick(&p, 30000);
    wake(&p, ORB_POWER_WAKE_ATTENTION, 30000);
    tick(&p, 50000); CHECK(snap(&p).state == ORB_POWER_DIMMED);
    ignored(&p, 55000);
    CHECK(snap(&p).state == ORB_POWER_SLEEP && p.activity_ms == 0);
}

static void forced_sleep(void)
{
    orb_power_controller_t p;
    orb_power_init(&p, 0); tick(&p, 30000);
    ignored(&p, 31000); ignored(&p, 41000);
    wake(&p, ORB_POWER_WAKE_PICKUP, 51000);
    CHECK(orb_power_request_sleep(&p, 500000));
    CHECK(p.ignored_pickups == 3 && p.pickup_suppressed_until_ms == 360000);
    /* A critical PMU update arriving late has the same monotonic anchor. */
    orb_power_init(&p, 0); tick(&p, 30000);
    ignored(&p, 31000); ignored(&p, 41000);
    wake(&p, ORB_POWER_WAKE_PICKUP, 51000);
    battery(&p, 5, false, 500000);
    CHECK(p.ignored_pickups == 3 && p.pickup_suppressed_until_ms == 360000);
}

static void alert_activity(void)
{
    orb_power_controller_t p, retained;
    orb_power_init(&p, 0); tick(&p, 30000);
    ignored(&p, 31000); ignored(&p, 41000); ignored(&p, 51000);
    CHECK(p.ignored_pickups == 3);
    CHECK(orb_power_set_quiet(&p, true, 61000));
    retained = p;
    CHECK(!orb_power_note_activity(&p, 60999));
    CHECK(!memcmp(&p, &retained, sizeof(p)));
    CHECK(orb_power_note_activity(&p, 61000));
    CHECK(p.ignored_pickups == 0 && p.pickup_suppressed_until_ms == 0 && p.activity_ms == 61000);
    CHECK(p.state == retained.state && p.redact == retained.redact && p.quiet == retained.quiet);
    CHECK(p.moment == retained.moment && p.moment_ms == retained.moment_ms);
    orb_power_init(&p, 0); tick(&p, 30000);
    wake(&p, ORB_POWER_WAKE_PICKUP, 31000);
    CHECK(orb_power_note_activity(&p, 32000));
    CHECK(!p.automatic && snap(&p).redact && snap(&p).state == ORB_POWER_ACTIVE);
    tick(&p, 51999); CHECK(snap(&p).state == ORB_POWER_ACTIVE && p.ignored_pickups == 0);
    tick(&p, 52000); CHECK(snap(&p).state == ORB_POWER_DIMMED);
    CHECK(orb_power_request_sleep(&p, 53000));
    CHECK(orb_power_note_activity(&p, 54000));
    CHECK(snap(&p).state == ORB_POWER_SLEEP && snap(&p).redact);
}

static void work_leases(void)
{
    const uint32_t leases[] = {ORB_POWER_INHIBIT_VOICE, ORB_POWER_INHIBIT_PLAYBACK,
        ORB_POWER_INHIBIT_OTA, ORB_POWER_INHIBIT_APP};
    for (unsigned i = 0; i < sizeof(leases) / sizeof(leases[0]); i++) {
        orb_power_controller_t p;
        orb_power_init(&p, 0); tick(&p, 30000);
        wake(&p, ORB_POWER_WAKE_PICKUP, 31000);
        CHECK(orb_power_set_inhibitors(&p, leases[i], 32000));
        tick(&p, 50000); CHECK(snap(&p).state == ORB_POWER_ACTIVE && p.ignored_pickups == 1);
        CHECK(orb_power_set_inhibitors(&p, 0, 50000));
        tick(&p, 69999); CHECK(snap(&p).state == ORB_POWER_ACTIVE);
        tick(&p, 70000); CHECK(snap(&p).state == ORB_POWER_DIMMED && snap(&p).redact);
        /* An alert/voice released before 9s still leaves a fresh ordinary budget. */
        orb_power_init(&p, 0); tick(&p, 30000);
        wake(&p, ORB_POWER_WAKE_PICKUP, 31000);
        CHECK(orb_power_set_inhibitors(&p, leases[i], 32000));
        CHECK(orb_power_set_inhibitors(&p, 0, 33000));
        tick(&p, 52999); CHECK(snap(&p).state == ORB_POWER_ACTIVE);
        tick(&p, 53000); CHECK(snap(&p).state == ORB_POWER_DIMMED);
        /* Acquisition owns the present instant, even after a scheduler gap. */
        orb_power_init(&p, 0);
        CHECK(orb_power_set_inhibitors(&p, leases[i], 120001));
        CHECK(snap(&p).state == ORB_POWER_ACTIVE);
        orb_power_init(&p, 0); tick(&p, 30000);
        wake(&p, ORB_POWER_WAKE_PICKUP, 31000);
        CHECK(orb_power_set_inhibitors(&p, leases[i], 50000));
        CHECK(snap(&p).state == ORB_POWER_ACTIVE && p.ignored_pickups == 1);
        /* Explicit Quiet/lock darkens the display without discarding the
         * system-sleep blocker owned by the active service. */
        orb_power_init(&p, 0);
        CHECK(orb_power_set_inhibitors(&p, leases[i], 0));
        CHECK(orb_power_set_quiet(&p, true, 1000));
        tick(&p, 120250);
        CHECK(snap(&p).state == ORB_POWER_SLEEP && p.inhibitors == leases[i]);
    }
}

static void cell_protection(void)
{
    orb_power_controller_t p, retained;
    orb_power_init(&p, 0);
    battery(&p, -1, false, 0); tick(&p, 1000000); CHECK(!snap(&p).shutdown_requested);
    battery(&p, 2, false, 1000000);
    CHECK(snap(&p).battery_mode == ORB_POWER_BATTERY_CRITICAL && !snap(&p).panel_on);
    CHECK(snap(&p).next_deadline_ms == 1060000);
    tick(&p, 1059999); CHECK(!snap(&p).shutdown_requested);
    uint32_t revision = snap(&p).revision;
    tick(&p, 1060000); CHECK(snap(&p).shutdown_requested && snap(&p).revision != revision);
    retained = p;
    CHECK(!orb_power_set_battery(&p, 4, false, false, 1059999));
    CHECK(!memcmp(&retained, &p, sizeof(p)));
    battery(&p, 3, false, 1060000); CHECK(snap(&p).shutdown_requested);
    battery(&p, 4, false, 1060001); CHECK(!snap(&p).shutdown_requested);
    battery(&p, 3, false, 1060002); tick(&p, 1160002); CHECK(!snap(&p).shutdown_requested);
    battery(&p, 2, false, 1160003); battery(&p, 3, false, 1190003);
    tick(&p, 1220003); CHECK(snap(&p).shutdown_requested); /* 2/3 jitter preserves evidence. */
    battery(&p, 1, true, 1220004); CHECK(!snap(&p).shutdown_requested);
    tick(&p, 1320004); CHECK(!snap(&p).shutdown_requested);
    battery(&p, 1, false, 1320005); battery(&p, -1, false, 1350005);
    tick(&p, 1420005); CHECK(!snap(&p).shutdown_requested && !p.cutoff_pending);
    battery(&p, 3, false, 1420006); tick(&p, 1520006); CHECK(!p.cutoff_pending);
    battery(&p, 0, false, 1520007); tick(&p, 1580006); CHECK(!snap(&p).shutdown_requested);
    tick(&p, 1580007); CHECK(snap(&p).shutdown_requested);
    battery(&p, -1, false, 1580008); CHECK(!snap(&p).shutdown_requested);
    /* Work does not erase the advisory; the consumer decides when rails are safe. */
    orb_power_init(&p, 0); battery(&p, 2, false, 0);
    CHECK(orb_power_set_inhibitors(&p, ORB_POWER_INHIBIT_OTA, 0));
    tick(&p, 60000); CHECK(snap(&p).shutdown_requested);
    orb_power_init(&p, UINT64_MAX - 10000); battery(&p, 2, false, UINT64_MAX - 10000);
    tick(&p, UINT64_MAX); CHECK(!snap(&p).shutdown_requested);
}

int main(int argc, char **argv)
{
    const struct { const char *name; void (*run)(void); } cases[] = {
        {"glance", glance}, {"deliberate", deliberate}, {"budget", ignored_budget},
        {"usb", usb_budget}, {"attention", attention}, {"forced", forced_sleep},
        {"invisible", invisible_budget}, {"boundary", budget_boundary},
        {"alert", alert_activity}, {"work", work_leases}, {"cell", cell_protection},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], cases[i].name)) continue;
        cases[i].run(); s_cases++;
    }
    CHECK(s_cases > 0);
    printf("orb_power policy: %u cases, %u checks passed (ASan/UBSan)\n", s_cases, s_checks);
    return 0;
}
