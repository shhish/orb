#include "orb_light_sleep_policy.h"

#include <assert.h>
#include <stddef.h>

static orb_light_sleep_inputs_t sleeping(void)
{
    return (orb_light_sleep_inputs_t){
        .power = {.state = ORB_POWER_SLEEP}, .power_valid = true, .panel_off = true,
    };
}

static orb_light_sleep_reason_t reason(orb_light_sleep_inputs_t in)
{
    return orb_light_sleep_policy(in, true, true, false, false, false);
}

static void eligibility(void)
{
    orb_light_sleep_inputs_t in = sleeping();
    assert(reason(in) == ORB_LIGHT_SLEEP_ALLOWED);
    assert(orb_light_sleep_policy(in, false, true, false, false, false) == ORB_LIGHT_SLEEP_DISABLED);
    assert(orb_light_sleep_policy(in, true, false, false, false, false) == ORB_LIGHT_SLEEP_WAKE_UNREADY);
    assert(orb_light_sleep_policy(in, true, true, true, false, false) == ORB_LIGHT_SLEEP_WAKE_FAILED);
    assert(orb_light_sleep_policy(in, true, true, false, false, true) == ORB_LIGHT_SLEEP_CONTACT);
    in.power_valid = false;
    assert(reason(in) == ORB_LIGHT_SLEEP_POWER_UNKNOWN);
    for (unsigned state = ORB_POWER_ACTIVE; state < ORB_POWER_SLEEP; state++) {
        in = sleeping(); in.power.state = state;
        assert(reason(in) == ORB_LIGHT_SLEEP_NOT_SLEEP);
    }
    in = sleeping(); in.panel_off = false;
    assert(reason(in) == ORB_LIGHT_SLEEP_PANEL_ON);
    in = sleeping(); in.power.panel_on = true;
    assert(reason(in) == ORB_LIGHT_SLEEP_PANEL_ON);
    for (unsigned bit = 0; bit < 32; bit++) {
        in = sleeping(); in.inhibitors = 1u << bit;
        assert(reason(in) == ORB_LIGHT_SLEEP_INHIBITED);
    }
    in = sleeping(); in.power.usb = true;
    assert(reason(in) == ORB_LIGHT_SLEEP_USB);
    assert(orb_light_sleep_policy(in, true, true, false, true, false) == ORB_LIGHT_SLEEP_ALLOWED);
    in.ota = true;
    assert(orb_light_sleep_policy(in, true, true, false, true, false) == ORB_LIGHT_SLEEP_OTA);
    const size_t offsets[] = {
        offsetof(orb_light_sleep_inputs_t, voice), offsetof(orb_light_sleep_inputs_t, ota),
        offsetof(orb_light_sleep_inputs_t, alarm), offsetof(orb_light_sleep_inputs_t, benchmark),
        offsetof(orb_light_sleep_inputs_t, game),
    };
    const orb_light_sleep_reason_t blockers[] = {
        ORB_LIGHT_SLEEP_VOICE, ORB_LIGHT_SLEEP_OTA, ORB_LIGHT_SLEEP_ALARM,
        ORB_LIGHT_SLEEP_BENCHMARK, ORB_LIGHT_SLEEP_GAME,
    };
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        in = sleeping();
        *(bool *)((char *)&in + offsets[i]) = true;
        assert(reason(in) == blockers[i]);
    }
}

static void lock_transitions(void)
{
    orb_light_sleep_lock_t lock = {.held = true};
    for (unsigned i = 0; i < 100; i++)
        assert(orb_light_sleep_lock_step(&lock, ORB_LIGHT_SLEEP_USB) == ORB_LIGHT_SLEEP_LOCK_KEEP);
    assert(orb_light_sleep_lock_step(&lock, ORB_LIGHT_SLEEP_ALLOWED) == ORB_LIGHT_SLEEP_LOCK_RELEASE);
    for (unsigned i = 0; i < 100; i++)
        assert(orb_light_sleep_lock_step(&lock, ORB_LIGHT_SLEEP_ALLOWED) == ORB_LIGHT_SLEEP_LOCK_KEEP);
    assert(orb_light_sleep_lock_step(&lock, ORB_LIGHT_SLEEP_VOICE) == ORB_LIGHT_SLEEP_LOCK_ACQUIRE);
    assert(orb_light_sleep_lock_step(&lock, ORB_LIGHT_SLEEP_USB) == ORB_LIGHT_SLEEP_LOCK_KEEP);
}

int main(void)
{
    eligibility();
    lock_transitions();
    return 0;
}
