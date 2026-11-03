/* Relevance/ordering contract, independent of rendering and fabricated data. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_stack.h"

static orb_model_snapshot_t s_model;
static orb_home_snapshot_t s_home;
static orb_time_snapshot_t s_time;
static orb_stack_entry_t s_entries[ORB_STACK_MAX];

static unsigned build(int hour, bool date)
{
    return orb_stack_build(&s_model, &s_time, &s_home, hour, date, s_entries, ORB_STACK_MAX);
}
static int position(unsigned count, orb_stack_kind_t kind)
{
    for (unsigned i = 0; i < count; i++) if (s_entries[i].kind == kind) return (int)i;
    return -1;
}
static void defaults(void)
{
    memset(&s_model, 0, sizeof(s_model));
    memset(&s_home, 0, sizeof(s_home));
    memset(&s_time, 0, sizeof(s_time));
    s_model.online = s_model.synced = true;
    s_model.prefs.sources[0] = s_model.prefs.sources[1] = true;
}
int main(void)
{
    /* Local-day comparisons are deterministic across developer machines. */
    assert(setenv("TZ", "UTC", 1) == 0); tzset();
    defaults();
    assert(build(-1, false) == 0); /* No clock, no telemetry, no invention. */
    assert(build(12, true) == 1 && s_entries[0].kind == ORB_STACK_TIME_FLOOR);
    s_home.online = s_home.synced = s_home.available = true;
    s_home.accessory_count = 1;
    s_home.accessories[0] = (orb_home_accessory_t){.kind = ORB_HOME_LIGHT, .reachable = true,
        .state = {.present = ORB_HOME_ON, .on = true}};
    assert(build(12, true) == 2 && s_entries[0].kind == ORB_STACK_HOME);
    s_home.accessories[0].state.present = 0;
    assert(build(12, true) == 1); /* Stale on bit without a known property. */
    s_home.accessories[0].state.present = ORB_HOME_ON;
    s_home.available = false; assert(build(12, true) == 1); s_home.available = true;
    s_home.accessories[0].reachable = false; assert(build(12, true) == 1);
    s_home.accessories[0].reachable = true;
    s_model.nowplaying_valid = true;
    assert(build(12, true) == 2); /* Snapshot alone does not grant capability. */
    s_model.nowplaying_supported = true;
    unsigned count = build(12, true);
    assert(count == 3 && s_entries[0].kind == ORB_STACK_NOWPLAYING && s_entries[1].kind == ORB_STACK_HOME);
    count = build(22, true);
    assert(s_entries[0].kind == ORB_STACK_HOME && s_entries[1].kind == ORB_STACK_NOWPLAYING);
    s_time.stopwatch_state = ORB_TIME_RUNNING;
    count = build(12, true);
    assert(count == 4 && s_entries[0].kind == ORB_STACK_TIME_ACTIVE);
    s_time.timer_state = ORB_TIME_RUNNING;
    assert(build(12, true) == count); /* One live Time card, even with both. */
    s_time.timer_state = ORB_TIME_PAUSED; s_time.stopwatch_state = ORB_TIME_IDLE;
    count = build(22, true);
    assert(s_entries[0].kind == ORB_STACK_HOME && s_entries[1].kind == ORB_STACK_NOWPLAYING &&
        s_entries[2].kind == ORB_STACK_TIME_ACTIVE); /* Paused remains below live context. */
    s_time.timer_state = ORB_TIME_RUNNING;
    s_model.card_count = 3;
    s_model.cards[0].priority = ORB_PRIORITY_LOW;
    s_model.cards[1].priority = ORB_PRIORITY_HIGH;
    s_model.cards[2].priority = ORB_PRIORITY_NORMAL;
    count = build(12, true);
    assert(count == 7 && s_entries[0].index == 1 && s_entries[1].index == 2 && s_entries[2].index == 0);
    s_model.cards[1].answered = true;
    assert(build(12, true) == 6 && s_entries[0].index == 2);
    defaults();
    s_time.clock_valid = true; s_time.clock_now = 1793188800; /* 2026-10-28 12:00 UTC. */
    s_time.alarm_enabled = true; s_time.alarm_next = s_time.clock_now + 3600;
    assert(orb_stack_alarm_today(&s_time));
    count = build(12, true);
    assert(count == 2 && s_entries[0].kind == ORB_STACK_TIME_ALARM && s_entries[1].kind == ORB_STACK_TIME_FLOOR);
    s_time.alarm_next += 86400;
    assert(!orb_stack_alarm_today(&s_time));
    assert(build(12, true) == 1 && s_entries[0].kind == ORB_STACK_TIME_FLOOR);
    s_time.alarm_next = s_time.clock_now - 1;
    assert(!orb_stack_alarm_today(&s_time));
    s_model.usage.count = 3;
    s_model.usage.windows[0] = (orb_usage_window_t){.provider = ORB_AGENT_CLAUDE,
        .used_percent = 50, .source = ORB_USAGE_STATUSLINE, .resets_at = 1793206800};
    s_model.usage.windows[1] = (orb_usage_window_t){.provider = ORB_AGENT_CLAUDE,
        .used_percent = 75, .source = ORB_USAGE_STATUSLINE};
    s_model.usage.windows[2] = (orb_usage_window_t){.provider = ORB_AGENT_CODEX,
        .used_percent = 20, .source = ORB_USAGE_APP_SERVER, .resets_at = 1793190600};
    count = build(12, true);
    assert(count == 3 && s_entries[0].kind == ORB_STACK_USAGE && s_entries[0].index == 1 && s_entries[1].index == 2);
    s_model.usage.windows[1].used_percent = 69.9;
    count = build(12, true);
    assert(count == 2 && s_entries[0].index == 2); /* Near reset, despite low use. */
    s_model.usage.windows[2].resets_at++;
    assert(build(12, true) == 1); /* Half-hour boundary is deliberate. */
    s_model.usage.windows[1].used_percent = 70;
    s_model.usage.windows[1].source = ORB_USAGE_UNKNOWN;
    assert(build(12, true) == 1);
    s_model.usage.windows[1].source = ORB_USAGE_STATUSLINE;
    s_model.usage.windows[1].stale = true;
    assert(build(12, true) == 1);
    s_model.usage.windows[1].stale = false; s_model.usage.unknown[ORB_AGENT_CLAUDE] = true;
    assert(build(12, true) == 1);
    s_model.usage.unknown[ORB_AGENT_CLAUDE] = false;
    s_model.usage.windows[1].resets_at = (uint64_t)s_time.clock_now;
    assert(build(12, true) == 1); /* The previous window has already reset. */
    s_model.usage.windows[1].resets_at = 0; s_model.usage.windows[1].used_percent = NAN;
    assert(build(12, true) == 1);
    s_model.usage.windows[1].used_percent = 70;
    s_model.online = false;
    assert(build(12, true) == 1); /* Offline usage cannot claim current pressure. */
    s_model.online = true; s_time.clock_valid = false;
    count = build(-1, false);
    assert(count == 1 && s_entries[0].kind == ORB_STACK_USAGE); /* High use needs no invented epoch. */
    assert(position(count, ORB_STACK_TIME_FLOOR) < 0);
    orb_stack_entry_t bounded[2] = {{0}, {ORB_STACK_NONE, 1234}};
    assert(orb_stack_build(&s_model, &s_time, &s_home, 12, true, bounded, 1) == 2);
    assert(bounded[1].index == 1234); /* Reports full count without overrunning. */
    puts("Stack relevance: daytime/night order, live time/alarm, capability, usage and honest floor passed");
    return 0;
}
