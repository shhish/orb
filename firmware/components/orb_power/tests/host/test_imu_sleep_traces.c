/* Synthetic 10 Hz traces prove timing tolerances, not board wake reliability. */
#include "orb_power.h"
#include "orb_idle_policy.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

typedef struct { orb_power_controller_t power; uint64_t now; } trace_t;

static trace_t trace(void)
{
    trace_t t = {.now = 2000};
    orb_power_init(&t.power, 0);
    assert(orb_power_request_sleep(&t.power, 1000));
    assert(orb_power_tick(&t.power, 1250));
    assert(t.power.state == ORB_POWER_SLEEP);
    return t;
}

static bool pose(trace_t *t, float degrees, float magnitude, uint32_t interval)
{
    float angle = degrees * 0.01745329251994f;
    t->now += interval;
    return orb_power_imu_sample(&t->power, sinf(angle) * magnitude, 0,
                                cosf(angle) * magnitude, t->now);
}

static void rest(trace_t *t)
{
    for (unsigned i = 0; i < 9; i++) assert(pose(t, 0, 1, 100));
    assert(t->power.motion.armed);
}

static void dwell_at_10hz(void)
{
    trace_t t = trace();
    rest(&t);
    assert(pose(&t, 0, 1.25f, 100));
    assert(t.power.motion.acceleration_candidate && t.power.state == ORB_POWER_SLEEP);
    assert(pose(&t, 0, 1.25f, 100));
    assert(t.power.state == ORB_POWER_SLEEP); /* 100 ms cannot prove 200 ms. */
    assert(pose(&t, 0, 1.25f, 100));
    assert(t.power.state == ORB_POWER_ACTIVE && t.power.redact);

    t = trace(); rest(&t);
    assert(pose(&t, 50, 1, 100));
    assert(fabsf(t.power.motion.filtered[0] - 0.4f * sinf(50 * 0.01745329251994f)) < 0.0001f);
    while (!t.power.motion.candidate) assert(pose(&t, 50, 1, 100));
    assert(pose(&t, 50, 1, 100));
    assert(t.power.state == ORB_POWER_SLEEP); /* Tilt dwell needs >=150 ms. */
    assert(pose(&t, 50, 1, 100));
    assert(t.power.state == ORB_POWER_ACTIVE && t.power.redact);
}

static void short_pulses_and_gap(void)
{
    trace_t t = trace(); rest(&t);
    assert(pose(&t, 0, 1.25f, 100));
    for (unsigned i = 0; i < 12; i++) assert(pose(&t, 0, 1, 100));
    assert(t.power.state == ORB_POWER_SLEEP && !t.power.motion.have_pickup);
    assert(pose(&t, 0, 1.25f, 100));
    assert(pose(&t, 0, 1.25f, 250)); /* The maximum continuous gap is inclusive. */
    assert(t.power.state == ORB_POWER_ACTIVE);

    t = trace(); rest(&t);
    assert(pose(&t, 0, 1.25f, 100));
    assert(!pose(&t, 0, 1.25f, 251));
    assert(!t.power.motion.armed && !t.power.motion.acceleration_candidate);
    assert(t.power.state == ORB_POWER_SLEEP);
    for (unsigned i = 0; i < 10; i++) assert(pose(&t, 0, 1.25f, 100));
    assert(t.power.state == ORB_POWER_SLEEP); /* Missing evidence cannot wake. */
}

static void face_down_budget(void)
{
    trace_t t = trace();
    for (unsigned i = 0; i < 4; i++) assert(pose(&t, 180, 1, 100));
    assert(t.power.motion.face_down && t.power.state == ORB_POWER_SLEEP);
    assert(!pose(&t, 180, 1, 1000)); /* Gap preserves confirmed face-down. */
    assert(t.power.motion.face_down);
    orb_power_snapshot_t power = orb_power_get_snapshot(&t.power);
    orb_idle_policy_t policy = orb_idle_sleep_policy(orb_idle_policy(power, false, false), power, true);
    assert(policy.poll_ms == 1000);
    assert(!pose(&t, 0, 1, 1000));
    assert(t.power.motion.face_down && t.power.motion.up_pending);
    assert(orb_idle_motion_wait(policy, true, 0, 0, 1) == 100);
    assert(pose(&t, 0, 1, 100));
    assert(pose(&t, 0, 1, 100));
    assert(t.power.motion.face_down);
    assert(pose(&t, 0, 1, 100));
    assert(!t.power.motion.face_down && t.power.state == ORB_POWER_SLEEP);
}

int main(void)
{
    dwell_at_10hz();
    short_pulses_and_gap();
    face_down_budget();
    puts("10 Hz sleep IMU: dt filter, 150/200 ms dwells, 250 ms gaps and face-down resume passed");
    return 0;
}
