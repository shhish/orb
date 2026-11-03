/* SYNTHETIC 20 Hz screen-axis traces, not recordings or reliability evidence.
 * Each fixture starts in Always On and tests the public controller wake path. */
#include "orb_power.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned s_checks, s_cases;
#define CHECK(c) do { s_checks++; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
    orb_power_controller_t power;
    uint64_t next_ms;
    unsigned wakes;
} trace_t;

static trace_t trace(void)
{
    trace_t result = { .next_ms = 30000 };
    orb_power_init(&result.power, 0);
    CHECK(orb_power_tick(&result.power, result.next_ms));
    CHECK(result.power.state == ORB_POWER_ALWAYS_ON);
    return result;
}

static void sample(trace_t *t, float x, float y, float z)
{
    bool had_pickup = t->power.motion.have_pickup;
    uint64_t last_pickup = t->power.motion.pickup_ms;
    CHECK(orb_power_imu_sample(&t->power, x, y, z, t->next_ms));
    if (t->power.motion.have_pickup && (!had_pickup || last_pickup != t->power.motion.pickup_ms)) t->wakes++;
    t->next_ms += 50;
}

static void pose(trace_t *t, float degrees, float magnitude)
{
    float angle = degrees * 0.01745329251994f;
    sample(t, sinf(angle) * magnitude, 0, cosf(angle) * magnitude);
}

static void hold(trace_t *t, float degrees, float magnitude, unsigned samples)
{
    for (unsigned i = 0; i < samples; i++) pose(t, degrees, magnitude);
}

static void rest(trace_t *t, float degrees)
{
    hold(t, degrees, 1, 21);
    CHECK(t->power.motion.armed);
    CHECK(t->wakes == 0 && t->power.state == ORB_POWER_ALWAYS_ON);
}

static void tilt(trace_t *t, float from, float to)
{
    for (unsigned i = 1; i <= 6; i++) pose(t, from + (to - from) * (float)i / 6, 1);
    hold(t, to, 1, 12);
}

static void pickup_from_rest(float degrees, float picked_up)
{
    trace_t t = trace();
    rest(&t, degrees);
    tilt(&t, degrees, picked_up);
    CHECK(t.wakes == 1 && t.power.state == ORB_POWER_ACTIVE);
    CHECK(orb_power_get_snapshot(&t.power).redact);
    hold(&t, picked_up, 1, 40);
    CHECK(t.wakes == 1);
    s_cases++;
}

static void straight_lift(float degrees, float magnitude)
{
    trace_t t = trace();
    rest(&t, degrees);
    hold(&t, degrees, magnitude, 4); /* 150 ms from the first changed sample. */
    CHECK(t.wakes == 0);
    pose(&t, degrees, magnitude); /* 200 ms sustained acceleration. */
    CHECK(t.wakes == 1 && t.power.state == ORB_POWER_ACTIVE);
    s_cases++;
}

static void desk_bump(float degrees)
{
    trace_t t = trace();
    rest(&t, degrees);
    hold(&t, degrees, 1.25f, 2); /* A 100 ms pulse must not wake. */
    hold(&t, degrees, 1, 16);
    hold(&t, degrees, 1.65f, 2); /* Even a strong, brief bump remains bounded. */
    hold(&t, degrees, 1, 16);
    hold(&t, degrees + 40, 1, 2); /* Short angular cable/desk kick. */
    hold(&t, degrees, 1, 16);
    for (unsigned i = 0; i < 20; i++) pose(&t, degrees, (i % 2) ? 1.22f : 0.78f);
    hold(&t, degrees, 1, 20);
    CHECK(t.wakes == 0 && t.power.state == ORB_POWER_ALWAYS_ON);
    tilt(&t, degrees, degrees > 30 ? degrees - 35 : degrees + 35);
    CHECK(t.wakes == 1); /* Rejected bumps did not permanently disable wake. */
    s_cases++;
}

static void typing(float degrees)
{
    trace_t t = trace();
    rest(&t, degrees);
    for (unsigned i = 0; i < 400; i++) {
        float angle = (degrees + ((i % 2) ? 1.5f : -1.5f)) * 0.01745329251994f;
        float norm = (i % 3) ? 1.025f : 0.975f;
        sample(&t, sinf(angle) * norm, (i % 2) ? 0.012f : -0.012f, cosf(angle) * norm);
    }
    CHECK(t.wakes == 0 && t.power.state == ORB_POWER_ALWAYS_ON);
    tilt(&t, degrees, degrees > 30 ? degrees - 35 : degrees + 35);
    CHECK(t.wakes == 1);
    s_cases++;
}

static void hysteresis_and_cooldown(void)
{
    trace_t t = trace();
    rest(&t, 45);
    pose(&t, 45, 1.23f); /* Enter at 0.18 g; hold above 0.10 g. */
    hold(&t, 45, 1.12f, 4);
    CHECK(t.wakes == 1);
    hold(&t, 45, 1, 30);
    CHECK(t.power.motion.armed);
    tilt(&t, 45, 5);
    CHECK(t.wakes == 1 && !t.power.motion.armed);
    hold(&t, 5, 1, 220);
    CHECK(t.power.motion.armed);
    tilt(&t, 5, 45);
    CHECK(t.wakes == 2);
    s_cases++;
}

static void face_down(void)
{
    trace_t t = trace();
    hold(&t, 180, 1, 6);
    CHECK(!t.power.motion.face_down && t.wakes == 0);
    pose(&t, 180, 1);
    orb_power_snapshot_t s = orb_power_get_snapshot(&t.power);
    CHECK(s.face_down && s.silent && !s.panel_on && s.state == ORB_POWER_SLEEP);
    CHECK(!orb_power_wake(&t.power, ORB_POWER_WAKE_TOUCH, t.next_ms).accepted);
    CHECK(!orb_power_imu_sample(&t.power, NAN, 0, 0, t.next_ms));
    t.next_ms += 50;
    CHECK(t.power.motion.face_down);
    hold(&t, 135, 1, 20); /* z=-0.707 remains inside exit hysteresis. */
    CHECK(t.power.motion.face_down);
    hold(&t, 60, 1, 36);
    CHECK(!t.power.motion.face_down && t.power.state == ORB_POWER_SLEEP && t.wakes == 0);
    CHECK(t.power.motion.armed);
    tilt(&t, 60, 25);
    CHECK(t.wakes == 1 && t.power.state == ORB_POWER_ACTIVE);
    s_cases++;
}

static void not_face_down(void)
{
    trace_t t = trace();
    hold(&t, 135, 1, 30); /* z=-0.707 is an arbitrary rest, not face-down. */
    CHECK(!t.power.motion.face_down && t.power.motion.armed);
    CHECK(t.power.state == ORB_POWER_ALWAYS_ON && t.wakes == 0);
    tilt(&t, 135, 50); /* Arbitrary rest may arm; the endpoint must be viewable. */
    CHECK(t.wakes == 1);
    s_cases++;
}

static void short_face_down(void)
{
    trace_t t = trace();
    rest(&t, 60);
    hold(&t, 180, 1, 3);
    hold(&t, 60, 1, 20);
    CHECK(!t.power.motion.face_down && t.wakes == 0);
    s_cases++;
}

static void face_down_threshold(void)
{
    trace_t t = trace();
    for (unsigned i = 0; i < 25; i++) sample(&t, sqrtf(1 - 0.79f*0.79f), 0, -0.79f);
    CHECK(!t.power.motion.face_down && t.wakes == 0);
    /* Only a true face-down pose can start the independent 300 ms dwell. */
    t = trace();
    for (unsigned i = 0; i < 6; i++) sample(&t, 0.6f, 0, -0.8f);
    CHECK(!t.power.motion.face_down);
    sample(&t, 0.6f, 0, -0.8f);
    CHECK(t.power.motion.face_down && t.power.state == ORB_POWER_SLEEP && t.wakes == 0);
    s_cases++;
}

static void tilt_hysteresis(void)
{
    trace_t t = trace();
    rest(&t, 0);
    for (unsigned i = 0; i < 20 && !t.power.motion.candidate; i++) pose(&t, 30, 1);
    CHECK(t.power.motion.candidate && t.wakes == 0);
    hold(&t, 22, 1, 4); /* Below entry, above release: dwell continues. */
    CHECK(t.wakes == 1);
    s_cases++;
}

static void slow_motion_is_not_rest(void)
{
    trace_t t = trace();
    for (unsigned i = 0; i < 35; i++) pose(&t, (float)i * 1.5f, 1);
    CHECK(!t.power.motion.armed && t.wakes == 0);
    s_cases++;
}

static void discontinuity(void)
{
    trace_t t = trace();
    rest(&t, 60);
    t.next_ms += 300;
    CHECK(!orb_power_imu_sample(&t.power, 0.5f, 0, 0.8660254f, t.next_ms));
    t.next_ms += 50;
    CHECK(!t.power.motion.armed);
    tilt(&t, 30, 70);
    CHECK(t.wakes == 0);
    s_cases++;
}

static void pocket_walk(void)
{
    trace_t t = trace();
    rest(&t, 90); /* Vertical in a pocket can arm at rest. */
    for (unsigned i = 0; i < 400; i++) {
        float degrees = 90 + 8 * sinf((float)i * 0.45f);
        pose(&t, degrees, (i / 8 % 2) ? 0.75f : 1.25f);
        CHECK(t.wakes == 0); /* Sustained steps still end with z near zero. */
    }
    CHECK(t.power.state == ORB_POWER_ALWAYS_ON);
    s_cases++;
}

static void bag_jostle(void)
{
    trace_t t = trace();
    rest(&t, 120);
    for (unsigned i = 0; i < 400; i++) {
        pose(&t, 120 + 12 * sinf((float)i * 0.3f), (i / 7 % 2) ? 0.70f : 1.30f);
        CHECK(t.wakes == 0); /* Neither tilt nor sustained jostle faces a viewer. */
    }
    CHECK(t.power.state == ORB_POWER_ALWAYS_ON && !t.power.motion.face_down);
    s_cases++;
}

static void hand_carry_face_up(void)
{
    trace_t t = trace();
    rest(&t, 0);
    hold(&t, 0, 1.25f, 5);
    CHECK(t.wakes == 1 && t.power.state == ORB_POWER_ACTIVE);
    /* Face-up carrying is indistinguishable from a straight viewing lift.
     * Movement alone cannot re-arm; the controller bounds ignored glances. */
    for (unsigned i = 0; i < 400; i++) pose(&t, 10 * sinf((float)i * 0.3f), (i / 8 % 2) ? 0.75f : 1.25f);
    CHECK(t.wakes == 1 && !t.power.motion.armed);
    s_cases++;
}

static void lift_toward_face_down(void)
{
    trace_t t = trace();
    rest(&t, 0);
    tilt(&t, 0, 120);
    CHECK(t.wakes == 0 && t.power.state == ORB_POWER_ALWAYS_ON);
    hold(&t, 180, 1, 30);
    CHECK(t.wakes == 0 && t.power.motion.face_down && t.power.state == ORB_POWER_SLEEP);
    s_cases++;
}

static void viewing_threshold(float z, unsigned expected)
{
    trace_t t = trace();
    float x = sqrtf(1 - z*z);
    for (unsigned i = 0; i < 21; i++) sample(&t, x, 0, z);
    CHECK(t.power.motion.armed && t.wakes == 0);
    for (unsigned i = 0; i < 5; i++) sample(&t, x * 1.25f, 0, z * 1.25f);
    CHECK(t.wakes == expected);
    CHECK(t.power.motion.armed == (expected == 0));
    s_cases++;
}

static void viewing_pose_agreement(void)
{
    trace_t t = trace();
    rest(&t, 0);
    for (unsigned i = 0; i < 20 && !t.power.motion.candidate; i++) pose(&t, 50, 1);
    CHECK(t.power.motion.candidate && t.wakes == 0);
    hold(&t, 50, 1, 2);
    pose(&t, 90, 1); /* Filter still faces up, but raw endpoint is vertical. */
    CHECK(t.wakes == 0);
    hold(&t, 90, 1, 10);
    CHECK(t.wakes == 0);

    t = trace();
    rest(&t, 90);
    hold(&t, 90, 1.25f, 4);
    pose(&t, 60, 1.25f); /* Raw faces up before filtered gravity agrees. */
    CHECK(t.wakes == 0);
    hold(&t, 60, 1.25f, 6);
    CHECK(t.wakes == 1);
    s_cases++;
}

int main(void)
{
    pickup_from_rest(0, 45); /* Flat desk. */
    pickup_from_rest(30, 65);
    pickup_from_rest(45, 5); /* 80 degrees is no longer a viewing endpoint. */
    pickup_from_rest(60, 25);
    pickup_from_rest(70, 35);
    pickup_from_rest(90, 50); /* Edge-on stand. */
    pickup_from_rest(120, 50);
    straight_lift(0, 1.23f);
    straight_lift(45, 1.23f);
    straight_lift(60, 0.77f);
    straight_lift(0, 1.65f); /* Vigorous lifts remain valid evidence. */
    straight_lift(45, 1.65f);
    straight_lift(60, 0.45f);
    desk_bump(0);
    desk_bump(45);
    desk_bump(60);
    typing(0);
    typing(45);
    typing(60);
    hysteresis_and_cooldown();
    face_down();
    not_face_down();
    short_face_down();
    face_down_threshold();
    tilt_hysteresis();
    slow_motion_is_not_rest();
    discontinuity();
    pocket_walk();
    bag_jostle();
    hand_carry_face_up();
    lift_toward_face_down();
    viewing_threshold(0.299f, 0);
    viewing_threshold(0.30f, 1);
    viewing_threshold(0.301f, 1);
    viewing_pose_agreement();
    printf("orb_power synthetic IMU traces: %u cases, %u checks passed\n", s_cases, s_checks);
    return 0;
}
