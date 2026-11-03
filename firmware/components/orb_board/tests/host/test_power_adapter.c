#include "orb_power_board.h"

#include <stdlib.h>
#include <stdio.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)

typedef struct {
    bool accept, complete, requested, reject_brightness;
    unsigned sleeps, wakes;
} panel_t;

static unsigned s_brightness_calls;
static int s_brightness;

void orb_board_panel_brightness(int pct)
{
    CHECK(pct >= 0 && pct <= 100);
    s_brightness = pct;
    s_brightness_calls++;
}

static bool request(void *context, bool awake)
{
    panel_t *panel = context;
    CHECK(s_brightness == 0);
    if (awake) panel->wakes++;
    else panel->sleeps++;
    if (!panel->accept) return false;
    panel->requested = awake;
    return true;
}

static bool ready(void *context, bool awake)
{
    const panel_t *panel = context;
    return panel->requested == awake && panel->complete;
}

static bool brightness(void *context, int pct)
{
    panel_t *panel = context;
    if (panel->reject_brightness) return false;
    orb_board_panel_brightness(pct);
    return true;
}

static orb_power_snapshot_t snapshot(uint32_t revision, bool awake)
{
    return (orb_power_snapshot_t) {
        .revision = revision, .panel_on = awake, .brightness_pct = awake ? 70 : 0,
        .redact = true, .state = awake ? ORB_POWER_ACTIVE : ORB_POWER_SLEEP,
    };
}

static void asynchronous_sleep_wake(void)
{
    panel_t panel = { .accept = true };
    orb_power_port_t port = { .context = &panel, .panel_awake = request, .panel_ready = ready };
    orb_power_board_adapter_t adapter;
    orb_power_board_adapter_init(&adapter);
    orb_power_snapshot_t sleep = snapshot(1, false), wake = snapshot(2, true);
    CHECK(orb_power_board_apply_snapshot(&sleep, &adapter, &port, 1) == ORB_POWER_WAIT_PANEL_READY);
    CHECK(panel.sleeps == 1 && !adapter.panel_known && adapter.panel_pending);
    CHECK(orb_power_board_apply_snapshot(&sleep, &adapter, &port, 1) == ORB_POWER_WAIT_PANEL_READY);
    CHECK(panel.sleeps == 1);
    panel.complete = true;
    CHECK(orb_power_board_apply_snapshot(&sleep, &adapter, &port, 1) == ORB_POWER_APPLIED);
    CHECK(adapter.panel_known && !adapter.panel_on && !adapter.panel_pending);
    panel.complete = false;
    /* Sleep RAM cannot have received this token: wake preparation precedes it. */
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 2) == ORB_POWER_WAIT_PANEL_READY);
    CHECK(panel.wakes == 1 && s_brightness == 0 && adapter.acknowledged_revision == 0);
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 0) == ORB_POWER_WAIT_PANEL_READY);
    CHECK(panel.wakes == 1);
    panel.complete = true;
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 0) == ORB_POWER_WAIT_SAFE_FRAME);
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 1) == ORB_POWER_WAIT_SAFE_FRAME);
    CHECK(s_brightness == 0);
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 2) == ORB_POWER_APPLIED);
    unsigned calls = s_brightness_calls;
    for (unsigned i = 0; i < 10; ++i)
        CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 2) == ORB_POWER_APPLIED);
    CHECK(panel.wakes == 1 && panel.sleeps == 1 && s_brightness == 70 && calls == s_brightness_calls);
    /* A lit handoff keeps its level, but not the older acknowledgement. */
    wake.revision = 3;
    wake.state = ORB_POWER_DIMMED;
    wake.brightness_pct = 15;
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 2) == ORB_POWER_WAIT_SAFE_FRAME);
    CHECK(s_brightness == 70 && adapter.acknowledged_revision == 2);
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 3) == ORB_POWER_APPLIED);
    CHECK(s_brightness == 15 && panel.wakes == 1);
}

static void failed_commands_and_reversal(void)
{
    panel_t panel = {0};
    orb_power_port_t port = { .context = &panel, .panel_awake = request, .panel_ready = ready };
    orb_power_board_adapter_t adapter;
    orb_power_board_adapter_init(&adapter);
    orb_power_snapshot_t sleep = snapshot(10, false), wake = snapshot(11, true);
    CHECK(orb_power_board_apply_snapshot(&sleep, &adapter, &port, 10) == ORB_POWER_PORT_FAILED);
    CHECK(!adapter.panel_known && s_brightness == 0);
    panel.accept = true;
    CHECK(orb_power_board_apply_snapshot(&sleep, &adapter, &port, 10) == ORB_POWER_WAIT_PANEL_READY);
    CHECK(panel.sleeps == 2 && adapter.panel_pending);
    /* Input arriving during the sleep deadline reverses the accepted direction. */
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 0) == ORB_POWER_WAIT_PANEL_READY);
    CHECK(panel.wakes == 1 && panel.requested && adapter.panel_requested);
    panel.complete = true;
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 11) == ORB_POWER_WAIT_SAFE_FRAME);
    CHECK(s_brightness == 0); /* The token presented during readiness was discarded. */
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 11) == ORB_POWER_APPLIED);
    sleep.revision = 12;
    panel.complete = false;
    CHECK(orb_power_board_apply_snapshot(&sleep, &adapter, &port, 12) == ORB_POWER_WAIT_PANEL_READY);
    panel.accept = false;
    wake.revision = 13;
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 13) == ORB_POWER_PORT_FAILED);
    CHECK(!adapter.panel_known && !adapter.shown_safe && s_brightness == 0);
    panel.accept = panel.complete = true;
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 13) == ORB_POWER_WAIT_SAFE_FRAME);
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 13) == ORB_POWER_APPLIED);
    CHECK(panel.wakes == 3 && panel.sleeps == 3 && s_brightness == 70);
}

static void serialized_brightness_failures(void)
{
    panel_t panel = { .accept = true, .complete = true, .reject_brightness = true };
    orb_power_port_t port = { .context = &panel, .panel_awake = request,
        .panel_ready = ready, .brightness = brightness };
    orb_power_board_adapter_t adapter;
    orb_power_board_adapter_init(&adapter);
    orb_power_snapshot_t sleep = snapshot(20, false), wake = snapshot(21, true);
    CHECK(orb_power_board_apply_snapshot(&sleep, &adapter, &port, 20) == ORB_POWER_PORT_FAILED);
    CHECK(panel.sleeps == 0 && adapter.brightness_pct == -1 && !adapter.shown_safe);
    panel.reject_brightness = false;
    CHECK(orb_power_board_apply_snapshot(&sleep, &adapter, &port, 20) == ORB_POWER_APPLIED);
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 21) == ORB_POWER_WAIT_SAFE_FRAME);
    panel.reject_brightness = true;
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 21) == ORB_POWER_PORT_FAILED);
    CHECK(s_brightness == 0 && !adapter.shown_safe && adapter.brightness_pct == -1);
    panel.reject_brightness = false;
    CHECK(orb_power_board_apply_snapshot(&wake, &adapter, &port, 21) == ORB_POWER_APPLIED);
    CHECK(s_brightness == 70 && panel.wakes == 1);
    panel.reject_brightness = true;
    CHECK(orb_power_board_apply_snapshot(&sleep, &adapter, &port, 20) == ORB_POWER_PORT_FAILED);
    CHECK(panel.sleeps == 1); /* A failed dark command cannot enter panel sleep. */
}

int main(void)
{
    CHECK(orb_power_board_poll_period() == 20);
    orb_power_board_set_poll_period(50);
    CHECK(orb_power_board_poll_period() == 50);
    asynchronous_sleep_wake();
    failed_commands_and_reversal();
    serialized_brightness_failures();
    puts("power adapter asynchronous transitions: passed");
    return 0;
}
