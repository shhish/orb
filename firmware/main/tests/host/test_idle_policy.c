#include <assert.h>
#include <stdio.h>
#include "orb_idle_policy.h"

static void states(void)
{
    for (unsigned state = ORB_POWER_ACTIVE; state <= ORB_POWER_SLEEP; state++) {
        for (unsigned redact = 0; redact < 2; redact++) {
            orb_power_snapshot_t power = {.state = state, .redact = redact,
                .panel_on = state != ORB_POWER_SLEEP, .brightness_pct = 70};
            orb_idle_policy_t policy = orb_idle_policy(power, false, false);
            bool active = state == ORB_POWER_ACTIVE && !redact;
            assert(policy.gyro == active);
            assert(policy.poll_ms == (active ? 20 : 50));
            assert(policy.ui_ms == (power.panel_on ? 20 : 100));
            assert(policy.taps == power.panel_on);
        }
    }
    orb_power_snapshot_t dark = {.state = ORB_POWER_SLEEP};
    assert(!orb_idle_policy(dark, false, false).taps);
    assert(orb_idle_policy(dark, true, false).taps); /* Stop speech remains local. */
    assert(orb_idle_policy(dark, false, true).taps); /* Finished timer dismissal. */
    dark.state = ORB_POWER_ACTIVE;
    dark.panel_on = true;
    assert(!orb_idle_policy(dark, false, false).taps); /* Owner brightness zero. */
    dark.revision = 4;
    assert(orb_idle_frame_needed(dark, 3));
    assert(!orb_idle_frame_needed(dark, 4));
    dark.state = ORB_POWER_ALWAYS_ON;
    assert(!orb_idle_frame_needed(dark, 3)); /* Static AOD never gains redraws. */
}

static void edge_lifetime(void)
{
    orb_buttons_t buttons;
    orb_buttons_init(&buttons);
    orb_buttons_poll(&buttons, 100, 1);
    assert(orb_idle_button_wait(&buttons, 100) == 30);
    orb_buttons_poll(&buttons, 130, 1);
    orb_button_event_t event;
    assert(orb_buttons_next(&buttons, &event) && event.gesture == ORB_BUTTON_DOWN);
    orb_buttons_consume_press(&buttons, ORB_BUTTON_PWR);
    orb_buttons_poll(&buttons, 145, 0);
    assert(orb_idle_button_wait(&buttons, 145) == 30);
    orb_buttons_poll(&buttons, 175, 0);
    assert(orb_buttons_next(&buttons, &event) && event.gesture == ORB_BUTTON_UP);
    orb_buttons_poll(&buttons, 2500, 0);
    assert(!orb_buttons_next(&buttons, &event)); /* Wake contact never clicks. */

    /* Main was delayed past both ISR edges. Replay the held interval before
     * release: a 35 ms physical press still debounces with 50 ms sensors. */
    orb_buttons_init(&buttons);
    orb_buttons_poll(&buttons, 100, 1);
    orb_buttons_poll(&buttons, 135, 1);
    orb_buttons_poll(&buttons, 135, 0);
    orb_buttons_poll(&buttons, 200, 0);
    assert(orb_buttons_next(&buttons, &event) && event.gesture == ORB_BUTTON_DOWN);
    assert(orb_buttons_next(&buttons, &event) && event.gesture == ORB_BUTTON_UP);
    orb_buttons_poll(&buttons, 465, 0);
    assert(orb_buttons_next(&buttons, &event) && event.gesture == ORB_BUTTON_SHORT);
    assert(!orb_buttons_next(&buttons, &event));

    orb_buttons_init(&buttons);
    orb_buttons_poll(&buttons, 100, 2);
    orb_buttons_poll(&buttons, 130, 2);
    assert(orb_idle_button_wait(&buttons, 460) == 20); /* BOOT hold due480. */
    orb_buttons_poll(&buttons, 480, 2);
    assert(orb_buttons_next(&buttons, &event) && event.gesture == ORB_BUTTON_DOWN);
    assert(orb_buttons_next(&buttons, &event) && event.gesture == ORB_BUTTON_HOLD);
}

static void sleep_budget(void)
{
    orb_power_snapshot_t power = {.state = ORB_POWER_SLEEP};
    orb_idle_policy_t normal = orb_idle_policy(power, false, false);
    orb_idle_policy_t denied = orb_idle_sleep_policy(normal, power, false);
    assert(denied.poll_ms == 50 && denied.ui_ms == 100 && !denied.minimal);
    orb_idle_policy_t eligible = orb_idle_sleep_policy(normal, power, true);
    assert(eligible.poll_ms == 100 && eligible.ui_ms == 1000 && eligible.minimal);
    assert(!eligible.gyro && !eligible.taps && !eligible.face_down);
    assert(orb_idle_motion_wait(eligible, true, 0, 0, 1) == 100);
    power.face_down = true;
    eligible = orb_idle_sleep_policy(normal, power, true);
    assert(eligible.poll_ms == 1000 && eligible.face_down);
    assert(orb_idle_motion_wait(eligible, true, 0, 0, -1) == 1000);
    assert(orb_idle_motion_wait(eligible, true, 0.8f, 0, -0.6f) == 100);
    assert(orb_idle_motion_wait(eligible, true, 0, 0, 1) == 100);
    assert(orb_idle_motion_wait(eligible, false, 0, 0, 1) == 1000);
    assert(orb_idle_motion_wait(eligible, true, 0, 0, 0) == 1000);

    orb_buttons_t buttons;
    orb_buttons_init(&buttons);
    assert(orb_idle_button_wait_limit(&buttons, 100, 1000) == 1000);
    orb_buttons_poll(&buttons, 100, 1);
    assert(orb_idle_button_wait_limit(&buttons, 100, 1000) == ORB_BUTTON_DEBOUNCE_MS);
    orb_buttons_poll(&buttons, 130, 1);
    assert(orb_idle_button_wait_limit(&buttons, 130, 1000) == 1000);
    orb_buttons_consume_press(&buttons, ORB_BUTTON_PWR);
    assert(orb_idle_button_wait_limit(&buttons, 130, 1000) == 1000);
    orb_buttons_poll(&buttons, 160, 0);
    assert(orb_idle_button_wait_limit(&buttons, 160, 1000) == ORB_BUTTON_DEBOUNCE_MS);
}

int main(void)
{
    states();
    edge_lifetime();
    sleep_budget();
    puts("idle state/cadence policy and physical edge deadlines: OK");
    return 0;
}
