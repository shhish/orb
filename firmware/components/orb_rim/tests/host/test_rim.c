#include "orb_rim_math.h"
#include "orb_sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned s_checks, s_cues;
static orb_sound_cue_t s_last_cue;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); } } while (0)
#define NEAR(a, b) CHECK(fabsf((a) - (b)) < .0001f)

bool orb_sound_play(orb_sound_cue_t cue)
{
    ++s_cues;
    s_last_cue = cue;
    return true;
}

static float coordinate(float angle, bool y)
{
    float radians = angle * 3.14159265358979323846f / 180;
    return 233 + 212 * (y ? sinf(radians) : cosf(radians));
}

static void move(const orb_rim_config_t *config, orb_rim_drag_t *drag,
                  float angle, bool reduce_motion, float *value)
{
    *value = orb_rim_drag_update(config, drag, coordinate(angle, false), coordinate(angle, true), reduce_motion);
}

int main(void)
{
    /* Visible brightness and volume strokes are absolute sliders. Test their
     * touch targets independently of LVGL's square widget hit geometry. */
    const orb_rim_config_t arcs[] = {
        {.start_angle = 50, .sweep_angle = 80},
        {.start_angle = 135, .sweep_angle = 270},
    };
    for (unsigned arc = 0; arc < 2; arc++) {
        orb_rim_drag_t drag;
        for (unsigned i = 0; i <= 100; i++) {
            float angle = orb_rim_angle(&arcs[arc], i / 100.f);
            CHECK(orb_rim_drag_begin_arc(&arcs[arc], &drag,
                coordinate(angle, false), coordinate(angle, true), 212, 46));
            NEAR(orb_rim_drag_update(&arcs[arc], &drag,
                coordinate(angle, false), coordinate(angle, true), true), i / 100.f);
            if (i < 99) {
                float value;
                move(&arcs[arc], &drag, angle + 1, true, &value);
                CHECK(value > i / 100.f); /* clockwise fills forward */
                move(&arcs[arc], &drag, angle, true, &value);
                NEAR(value, i / 100.f);
            }
        }
        CHECK(!orb_rim_drag_begin_arc(&arcs[arc], &drag, 233, 233, 212, 46));
        CHECK(!orb_rim_drag_begin_arc(&arcs[arc], &drag, 233, 21, 212, 46) || arc == 1);
        CHECK(!orb_rim_drag_begin_arc(&arcs[arc], &drag, NAN, 233, 212, 46));
    }
    orb_rim_drag_t cap;
    CHECK(orb_rim_drag_begin_arc(&arcs[0], &cap, coordinate(45, false), coordinate(45, true), 212, 46));
    NEAR(orb_rim_drag_update(&arcs[0], &cap, coordinate(45, false), coordinate(45, true), true), 0);
    CHECK(!orb_rim_drag_begin_arc(&arcs[0], &cap, coordinate(40, false), coordinate(40, true), 212, 46));
    for (unsigned gaps = 0; gaps <= 2; ++gaps) {
        orb_rim_config_t config = {.gaps = gaps};
        NEAR(orb_rim_angle(&config, 0), 135);
        NEAR(orb_rim_angle(&config, 1), 405);
        float previous = 0;
        for (unsigned i = 0; i <= 1000; ++i) {
            float value = (float)i / 1000;
            float angle = orb_rim_angle(&config, value);
            CHECK(angle >= previous);
            NEAR(orb_rim_value(&config, angle), value);
            previous = angle;
        }
        if (gaps) {
            NEAR(orb_rim_value(&config, 201), orb_rim_value(&config, 225));
            NEAR(orb_rim_value(&config, 225), orb_rim_value(&config, 249));
        }
        if (gaps == 2) NEAR(orb_rim_value(&config, 291), orb_rim_value(&config, 339));
        orb_rim_drag_t drag;
        CHECK(!orb_rim_drag_begin(&config, &drag, 233, 233));
        CHECK(!orb_rim_drag_begin(&config, &drag, 465, 233));
        CHECK(!orb_rim_drag_begin(&config, &drag, 233, 445));
        CHECK(!orb_rim_drag_begin(&config, &drag, NAN, 233));
        CHECK(!orb_rim_drag_begin(&config, NULL, 233, 21));
        CHECK(orb_rim_drag_begin(&config, &drag, coordinate(350, false), coordinate(350, true)));
        float value;
        move(&config, &drag, 359, true, &value);
        NEAR(drag.angle, 359);
        move(&config, &drag, 1, true, &value);
        NEAR(drag.angle, 361);
        NEAR(value, orb_rim_value(&config, 361));
        move(&config, &drag, 350, true, &value);
        NEAR(drag.angle, 350);
        move(&config, &drag, 90, true, &value);
        NEAR(value, 1);
        move(&config, &drag, 100, false, &value);
        CHECK(value > 1 && value < 1.075f);
        CHECK(orb_rim_drag_begin(&config, &drag, coordinate(140, false), coordinate(140, true)));
        move(&config, &drag, 100, true, &value);
        NEAR(value, 0);
        move(&config, &drag, 90, false, &value);
        CHECK(value < 0 && value > -.075f);
        drag.active = false;
        float before = drag.angle;
        move(&config, &drag, 270, true, &value);
        NEAR(before, drag.angle);
    }
    NEAR(orb_rim_angle(NULL, 1), 405);
    /* Time's full ring begins at twelve, with clockwise values and no jump
     * when the owner grabs a digit group's rim at an arbitrary position. */
    orb_rim_config_t full = {.start_angle = 270, .sweep_angle = 360};
    NEAR(orb_rim_angle(&full, 0), 270);
    NEAR(orb_rim_angle(&full, .25f), 360);
    NEAR(orb_rim_angle(&full, 1), 630);
    orb_rim_drag_t relative;
    for (unsigned angle = 0; angle < 360; ++angle) {
        CHECK(orb_rim_drag_begin(&full, &relative, coordinate(angle, false), coordinate(angle, true)));
        CHECK(orb_rim_drag_begin_relative(&full, &relative,
            coordinate(angle, false), coordinate(angle, true), .5f));
        NEAR(relative.angle, 450);
        float value;
        move(&full, &relative, (float)angle + 6, true, &value);
        NEAR(value, .5f + 6.0f / 360);
        move(&full, &relative, (float)angle - 6, true, &value);
        NEAR(value, .5f - 6.0f / 360);
    }
    CHECK(!orb_rim_drag_begin_relative(&full, &relative, 233, 233, .5f));
    CHECK(!orb_rim_drag_begin_relative(&full, &relative, 233, 21, NAN));
    for (unsigned i = 0; i <= 1000; ++i) {
        float value = (float)i / 1000;
        NEAR(orb_rim_value(&full, orb_rim_angle(&full, value)), value);
    }
    int selected = 0;
    CHECK(!orb_rim_detent(0, 60, &selected, true));
    CHECK(s_cues == 0);
    CHECK(orb_rim_detent(.5f, 60, &selected, true));
    CHECK(selected == 30 && s_cues == 1 && s_last_cue == ORB_SOUND_DETENT);
    CHECK(!orb_rim_detent(.501f, 60, &selected, true));
    CHECK(s_cues == 1);
    CHECK(orb_rim_detent(2, 60, &selected, true));
    CHECK(selected == 60 && s_cues == 2 && s_last_cue == ORB_SOUND_DETENT);
    CHECK(orb_rim_detent(-1, 60, &selected, false));
    CHECK(selected == 0 && s_cues == 2);
    CHECK(!orb_rim_detent(0, 60, NULL, true));
    printf("orb_rim: %u checks passed\n", s_checks);
    return 0;
}
