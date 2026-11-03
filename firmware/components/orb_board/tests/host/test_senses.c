#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "orb_senses.h"

static orb_senses_t s;
static uint64_t s_now;
static int s_detents, s_taps;

static orb_senses_result_t sample(float az, float ax, float gx, float gz, bool touch, bool hardware)
{
    s_now += 20;
    orb_motion_sample_t m = {.ax=ax, .az=az, .gx=gx, .gz=gz, .hardware_double_tap=hardware};
    orb_senses_result_t r = orb_senses_update(&s, &m, touch, s_now);
    s_detents += r.detents;
    s_taps += r.double_tap;
    return r;
}

static void rest(unsigned frames)
{
    for (unsigned i=0; i<frames; i++) sample(1, 0, 0, 0, false, false);
}

static void reset(void)
{
    s = (orb_senses_t){0};
    s_now = 0;
    s_detents = s_taps = 0;
    rest(100); /* the bias seed needs a still second before anything arms */
}

/* Dev board orb-020000abcdef at rest: a constant 4.1, 20.4, -0.1 deg/s. The
 * raw side rate alone exceeded the movement limit, so nothing ever armed. */
static void test_board_bias(void)
{
    s = (orb_senses_t){0}; s_now = 0; s_detents = s_taps = 0;
    orb_motion_sample_t m = {.az = 0.92f, .ax = -0.016f, .ay = 0.086f, .gx = 4.1f, .gy = 20.4f, .gz = -0.1f};
    orb_senses_result_t r = {0};
    for (unsigned i = 0; i < 100; i++) { s_now += 20; r = orb_senses_update(&s, &m, false, s_now); }
    assert(s.bias_valid && fabsf(s.bias_y - 20.4f) < 0.01f && fabsf(s.bias_x - 4.1f) < 0.01f);
    assert(s.desk_armed && r.resting);
    m.gz = -0.1f + 60;
    for (unsigned i = 0; i < 50; i++) { s_now += 20; s_detents += orb_senses_update(&s, &m, false, s_now).detents; }
    assert(s_detents == -5); /* +screen-z is counterclockwise from the face */
    /* A hand-held Orb never seeds: its readings never hold still for a second. */
    s = (orb_senses_t){0}; s_now = 0;
    for (unsigned i = 0; i < 500; i++) {
        s_now += 20;
        orb_motion_sample_t hand = {.az = 0.9f, .ax = 0.1f * (float)(i % 3), .gx = 4.1f + (float)(i % 4) * 2,
                                    .gy = 20.4f, .gz = 30.0f * (float)(i % 2)};
        r = orb_senses_update(&s, &hand, false, s_now);
        assert(!r.detents && !r.double_tap);
    }
    assert(!s.bias_valid && !s.desk_armed);
}

static void test_twist(void)
{
    reset();
    /* Screen +z points toward the viewer: clockwise desk turns produce -gz.
     * The positive UI owner always increases/advances, like the touch rim. */
    for (unsigned i=0; i<50; i++) sample(1, 0, 0, -60, false, false);
    assert(s_detents == 5);
    for (unsigned i=0; i<50; i++) sample(1, 0, 0, 60, false, false);
    assert(s_detents == 0);
    assert(s_taps == 0);
    reset();
    for (unsigned i=0; i<50; i++) sample(1, 0, 0, 60, false, false);
    assert(s_detents == -5);
    /* A slower turn decelerates and pauses before lift, without adding
     * phantom detents while still or changing sign at the dead band. */
    reset();
    for (unsigned i=0; i<50; i++) sample(1, 0, 0, -18, false, false);
    for (unsigned i=0; i<25; i++) sample(1, 0, 0, -8, false, false);
    rest(10);
    assert(s_detents == 1);
    reset();
    for (unsigned i=0; i<500; i++) sample(0.70f, 0.70f, 15, 60, false, false);
    assert(s_detents == 0 && s_taps == 0);
    reset();
    for (unsigned i=0; i<30000; i++) sample(1, 0, 0, 1.8f, false, false);
    assert(s_detents == 0 && s_taps == 0);
    assert(fabsf(s.bias_z - 1.8f) < 0.05f);
    /* No catch-up integration after a stalled owner or an invalid sample. */
    s_now += 1000;
    assert(sample(1, 0, 0, 100, false, false).detents == 0);
    sample(NAN, 0, 0, 0, false, false);
    assert(!s.initialized);
    /* Power-on in an arbitrary pose must recover after being set on a desk. */
    s = (orb_senses_t){0}; s_now=0; s_detents=s_taps=0;
    for (unsigned i=0; i<50; i++) sample(0.25f,0.9f,0,0,false,false);
    rest(100);
    assert(s.desk_armed);
    for (unsigned i=0; i<50; i++) sample(1,0,0,-60,false,false);
    assert(s_detents == 5);
}

static void pair(void)
{
    sample(1.9f, 0, 0, 0, false, false);
    rest(7); /* second shock 160ms after first */
    sample(1.9f, 0, 0, 0, false, false);
    rest(5);
}

static void test_taps(void)
{
    reset(); pair(); assert(s_taps == 1);
    rest(40); pair(); assert(s_taps == 2);
    reset();
    sample(1.9f,0,0,0,false,false); rest(20);
    assert(s_taps == 0); /* isolated desk bump */
    reset();
    for (unsigned i=0; i<5; i++) sample(2.0f,0,0,0,false,false);
    rest(5); pair(); assert(s_taps == 0); /* broad desk bump, plus lockout */
    reset();
    for (unsigned i=0; i<1000; i++) {
        sample(i%3 ? 1.0f : 1.25f,0,0,0,false,false);
    }
    assert(s_taps == 0);
    /* Two strong taps cannot bypass the vibration quiet guard. */
    pair(); assert(s_taps == 0);
    reset();
    for (unsigned i=0; i<2; i++) {
        sample(1.9f,0,0,0,true,true); rest(7);
    }
    rest(5); assert(s_taps == 0);
    reset();
    sample(1,0.5f,20,0,false,true); rest(5); assert(s_taps == 0);
    reset();
    sample(1,0,0,0,false,true); rest(4); assert(s_taps == 1);
    sample(1,0,0,0,false,true); rest(4); assert(s_taps == 1); /* refractory */
    reset();
    sample(1.9f,0,0,0,false,false); rest(1);
    sample(1.9f,0,0,0,false,false); rest(20); assert(s_taps == 0); /* shock ringing */
    /* The actual board audit saw 0.93g at rest; a unit's static scale error
     * must not prevent arming or turn into a phantom shock. */
    s = (orb_senses_t){0}; s_now=0; s_taps=0;
    for (unsigned i=0;i<100;i++) sample(0.93f,0,0,0,false,false);
    sample(1.83f,0,0,0,false,false);
    for (unsigned i=0;i<7;i++) sample(0.93f,0,0,0,false,false);
    sample(1.83f,0,0,0,false,false);
    for (unsigned i=0;i<5;i++) sample(0.93f,0,0,0,false,false);
    assert(s_taps == 1);
}

int main(void)
{
    test_board_bias(); test_twist(); test_taps();
    printf("orb_senses: flat twist, in-hand motion, drift, gaps, double taps, bumps, typing and touch guards passed\n");
    printf("orb_senses state: %zu bytes\n", sizeof(orb_senses_t));
    return 0;
}
