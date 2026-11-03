/* Display-owner regression: level-only revisions preserve the safe pixels.
 * The shell's frame coalescing is exercised separately by gesture_controls. */
#include <stdio.h>
#include <stdlib.h>

#include "orb_power_board.h"

static unsigned s_checks, s_writes, s_panel_events;
static int s_level;
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "power brightness:%d: %s\n", __LINE__, #value); exit(1); \
} } while (0)

void orb_board_panel_brightness(int percent)
{
    s_level = (int)percent;
    ++s_writes;
}

static bool panel_awake(void *context, bool awake)
{
    ++s_panel_events;
    return true;
}

static void drag(unsigned port_kind)
{
    orb_power_board_adapter_t adapter;
    orb_power_board_adapter_init(&adapter);
    orb_power_snapshot_t frame = {
        .state = ORB_POWER_ACTIVE, .revision = 1, .brightness_pct = 50, .panel_on = true,
    };
    /* No panel hook is the firmware's actual fallback. Supplying a zeroed
     * port also must not regress the already-lit-frame brightness path. */
    orb_power_port_t port = {.panel_awake = port_kind == 2 ? panel_awake : NULL};
    const orb_power_port_t *platform = port_kind ? &port : NULL;
    CHECK(orb_power_board_apply_snapshot(&frame, &adapter, platform, 1) == ORB_POWER_APPLIED);
    CHECK(s_level == 50 && adapter.shown_safe);
    unsigned awake_events = s_panel_events;
    for (int direction = -1; direction <= 1; direction += 2) {
        int first = direction < 0 ? 49 : 2;
        int last = direction < 0 ? 1 : 100;
        for (int value = first; direction < 0 ? value >= last : value <= last; value += direction) {
            ++frame.revision;
            frame.brightness_pct = (uint8_t)value;
            unsigned before = s_writes;
            CHECK(orb_power_board_apply_snapshot(&frame, &adapter, platform, 0) == ORB_POWER_WAIT_SAFE_FRAME);
            CHECK(s_writes == before + 1 && s_level == value && adapter.shown_safe);
            CHECK(s_panel_events == awake_events); /* never cycle panel power on a detent */
            /* Repeating a policy turn never repeats a panel command. */
            CHECK(orb_power_board_apply_snapshot(&frame, &adapter, platform, 0) == ORB_POWER_WAIT_SAFE_FRAME);
            CHECK(s_writes == before + 1);
        }
    }
    /* The already visible private frame stays lit until redaction drains. */
    ++frame.revision;
    frame.redact = true;
    CHECK(orb_power_board_apply_snapshot(&frame, &adapter, platform, 0) == ORB_POWER_WAIT_SAFE_FRAME);
    CHECK(s_level == 100 && adapter.shown_safe);
    CHECK(orb_power_board_apply_snapshot(&frame, &adapter, platform, frame.revision) == ORB_POWER_APPLIED);
    CHECK(s_level == 100 && adapter.shown_safe);
}

int main(void)
{
    drag(0);
    drag(1);
    drag(2);
    printf("power brightness: %u checks, monotonic frame levels with no black safe-frame writes\n", s_checks);
    return 0;
}
