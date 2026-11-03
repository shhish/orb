#include "orb_delight_motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned s_checks;
#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

int main(void)
{
    orb_delight_spring_t spring;
    orb_delight_spring_reset(&spring, 0);
    CHECK(orb_delight_spring_settled(&spring));
    orb_delight_spring_target(&spring, 1);
    CHECK(!orb_delight_spring_settled(&spring));
    float previous = spring.value;
    for (unsigned i = 0; i < 120; i++) {
        orb_delight_spring_step(&spring, 1.0f / 60, 0.5f, 0);
        CHECK(spring.value >= previous);
        CHECK(spring.value <= 1);
        previous = spring.value;
    }
    CHECK(orb_delight_spring_settled(&spring));
    CHECK(spring.value == 1);
    CHECK(spring.velocity == 0);

    /* A new destination never discards the motion already under the finger. */
    orb_delight_spring_reset(&spring, 0);
    orb_delight_spring_target(&spring, 1);
    orb_delight_spring_step(&spring, .05f, .5f, .15f);
    float velocity = spring.velocity;
    float position = spring.value;
    CHECK(velocity > 0);
    orb_delight_spring_target(&spring, -1);
    CHECK(spring.velocity == velocity);
    CHECK(spring.value == position);
    for (unsigned i = 0; i < 180; i++)
        orb_delight_spring_step(&spring, 1.0f / 60, .5f, .15f);
    CHECK(spring.value == -1);
    CHECK(orb_delight_spring_settled(&spring));

    /* Equal time split across different display rates yields the same path. */
    orb_delight_spring_t a, b;
    orb_delight_spring_reset(&a, 0);
    orb_delight_spring_reset(&b, 0);
    orb_delight_spring_target(&a, 1);
    orb_delight_spring_target(&b, 1);
    for (unsigned i = 0; i < 24; i++)
        orb_delight_spring_step(&a, 1.0f / 120, .5f, .15f);
    for (unsigned i = 0; i < 6; i++)
        orb_delight_spring_step(&b, 1.0f / 30, .5f, .15f);
    CHECK(fabsf(a.value - b.value) < .00001f);
    CHECK(fabsf(a.velocity - b.velocity) < .00001f);
    position = a.value;
    CHECK(!orb_delight_spring_step(&a, NAN, .5f, 0));
    CHECK(!orb_delight_spring_step(&a, -1, .5f, 0));
    CHECK(a.value == position);
    orb_delight_spring_target(&a, INFINITY);
    CHECK(a.target == 1);
    orb_delight_spring_reset(&a, NAN);
    CHECK(a.value == position);

    float last = 0;
    for (unsigned i = 0; i < 1000; i++) {
        float offset = orb_delight_rubber_band(i, 72);
        CHECK(offset >= last);
        CHECK(offset < 72);
        CHECK(fabsf(offset + orb_delight_rubber_band(-(float)i, 72)) < .00001f);
        last = offset;
    }
    CHECK(orb_delight_rubber_band(0, 72) == 0);
    CHECK(orb_delight_rubber_band(12, 0) == 0);
    CHECK(orb_delight_rubber_band(NAN, 72) == 0);
    CHECK(orb_delight_crossfade(0) == 0);
    CHECK(orb_delight_crossfade(75) == .5f);
    CHECK(orb_delight_crossfade(150) == 1);
    CHECK(orb_delight_crossfade(UINT32_MAX) == 1);
    CHECK(orb_delight_color(ORB_DELIGHT_ORANGE, false) == 0xFF9230);
    CHECK(orb_delight_color(ORB_DELIGHT_ORANGE, true) == 0xFFA056);
    CHECK(orb_delight_color(ORB_DELIGHT_MINT, false) == 0x00DAC3);
    CHECK(orb_delight_color(ORB_DELIGHT_TEAL, false) == 0x00D2E0);
    CHECK(orb_delight_color(ORB_DELIGHT_TEAL, true) == 0x3BDDEC);
    CHECK(orb_delight_color(ORB_DELIGHT_INDIGO, true) == 0xA7AAFF);
    CHECK(orb_delight_color((orb_delight_color_t)-1, false) == 0x8E8E93);
    printf("delight motion: %u checks passed\n", s_checks);
    return 0;
}
