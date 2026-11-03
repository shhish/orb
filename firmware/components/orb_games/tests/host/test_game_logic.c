#include "orb_game_logic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

static float arc_distance(const orb_game_sprite_t *arc, float x, float y)
{
    float angle = atan2f(y - arc->y, x - arc->x);
    float middle = (arc->arc_start + arc->arc_end) * 0.5f;
    while (angle - middle > 3.14159265f) angle -= 6.28318531f;
    while (angle - middle < -3.14159265f) angle += 6.28318531f;
    angle = fmaxf(arc->arc_start, fminf(arc->arc_end, angle));
    return hypotf(x - arc->x - cosf(angle) * arc->arc_radius,
                  y - arc->y - sinf(angle) * arc->arc_radius);
}

static void step_n(orb_game_state_t *state, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        /* Physics-only fixtures explicitly supply quiet sensor time. The
         * cadence/noise traces below exercise the real motion classifier. */
        if (state->kind == ORB_GAME_BEADS) {
            orb_game_tilt_t rest = {.ready = true, .gyro_observed = true,
                .still_us = ((uint32_t)state->game.beads.settle_ticks + 1u) * ORB_GAME_STEP_US};
            orb_game_set_motion(state, &rest);
        }
        orb_game_step(state, 0, 0, false, false);
    }
}

static void identity(void)
{
    CHECK(sizeof(orb_game_state_t) <= 16384); /* Dense clock and pool live in PSRAM. */
    CHECK(sizeof(orb_game_state_t) >= 4808); /* Beads' fixed particle field. */
    CHECK(sizeof(orb_marble_sample_t) == 8);
    for (unsigned i = 0; i < ORB_GAME_COUNT; ++i) {
        orb_game_kind_t kind = ORB_GAME_COUNT;
        CHECK(orb_game_kind_from_id(orb_game_id((orb_game_kind_t)i), &kind));
        CHECK(kind == (orb_game_kind_t)i);
        CHECK(strlen(orb_game_name(kind)) < 15);
    }
    CHECK(!orb_game_kind_from_id("unknown", &(orb_game_kind_t){0}));
    CHECK(!orb_game_kind_from_id(NULL, &(orb_game_kind_t){0}));
    CHECK(!orb_game_kind_from_id("marble", NULL));
    CHECK(!orb_game_id(ORB_GAME_COUNT));
    CHECK(!orb_game_name((orb_game_kind_t)-1));
    orb_game_init(NULL, ORB_GAME_MARBLE, 0);
    orb_game_step(NULL, 0, 0, false, false);
    orb_game_action(NULL, ORB_GAME_PRIMARY, 0, 0);
    CHECK(orb_game_sprites(NULL, NULL, 0) == 0);
    orb_game_state_t invalid;
    orb_game_init(&invalid, ORB_GAME_COUNT, 0);
    CHECK(invalid.game_over);
}

static void deterministic_and_bounds(void)
{
    for (unsigned kind = 0; kind < ORB_GAME_COUNT; ++kind) {
        orb_game_state_t a, b;
        orb_game_init(&a, (orb_game_kind_t)kind, 451);
        orb_game_init(&b, (orb_game_kind_t)kind, 451);
        for (unsigned i = 0; i < 12000; ++i) {
            if (i % 37 == 0) {
                orb_game_action(&a, ORB_GAME_PRIMARY, 233, 245);
                orb_game_action(&b, ORB_GAME_PRIMARY, 233, 245);
            }
            if (a.game_over) {
                orb_game_action(&a, ORB_GAME_RESTART, 0, 0);
                orb_game_action(&b, ORB_GAME_RESTART, 0, 0);
            }
            /* Synthetic screen-frame accelerometer readings, |a_xy| <= 1 g. */
            float ax = 0.7f * sinf((float)i * 0.008f), ay = 0.7f * cosf((float)i * 0.013f);
            orb_game_step(&a, ax, ay, true, false);
            orb_game_step(&b, ax, ay, true, true);
            orb_game_state_t essential = a;
            essential.reduce_motion = b.reduce_motion;
            essential.light_x = b.light_x;
            essential.light_y = b.light_y;
            CHECK(!memcmp(&essential, &b, sizeof(a))); /* Same rules and RNG under Reduce Motion. */
            orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
            size_t count = orb_game_sprites(&a, sprites, ORB_GAME_MAX_SPRITES);
            CHECK(count <= ORB_GAME_MAX_SPRITES);
            for (size_t n = 0; n < count; ++n) {
                CHECK(isfinite(sprites[n].x) && isfinite(sprites[n].y));
                CHECK(sprites[n].radius <= 48);
                if (!sprites[n].radius) continue;
                float distance = hypotf(sprites[n].x - ORB_GAME_CX, sprites[n].y - ORB_GAME_CY);
                CHECK(distance + sprites[n].radius <=
                    (kind == ORB_GAME_MARBLE ? ORB_MARBLE_RADIUS : ORB_GAME_RADIUS) + 0.01f);
                if (kind == ORB_GAME_MARBLE && sprites[n].shape == ORB_GAME_GATE)
                    CHECK(arc_distance(&sprites[n], sprites[0].x, sprites[0].y) >= 17.9f);
            }
            sprites[1] = (orb_game_sprite_t){.x = -9, .y = -8, .radius = 7, .colour = 6, .shape = 5};
            CHECK(orb_game_sprites(&a, sprites, 1) <= 1);
            CHECK(sprites[1].x == -9 && sprites[1].y == -8 && sprites[1].radius == 7);
            CHECK(orb_game_sprites(&a, sprites, 0) == 0);
        }
    }
}

static void marble(void)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    CHECK(hypotf(state.game.marble.x, state.game.marble.y) > 208);
    CHECK(orb_game_position_clear(state.game.marble.x, state.game.marble.y,
        ORB_MARBLE_BALL_RADIUS, state.kind, true));
    CHECK(state.game.marble.history_count == 1);
    step_n(&state, 30);
    CHECK(state.score == 0 && !state.game.marble.stage_cleared);
    CHECK(state.game.marble.history_count == 16);
    /* Right side and bottom tilted down reads ax < 0, ay > 0. */
    orb_game_step(&state, -0.5f, 0.5f, true, false);
    CHECK(state.game.marble.vx > 0 && state.game.marble.vy > 0);
    for (unsigned i = 0; i < 500; ++i) orb_game_step(&state, -0.5f, 0.5f, true, false);
    CHECK(hypotf(state.game.marble.x, state.game.marble.y) <= ORB_MARBLE_RADIUS - 12 + 0.001f);
    CHECK(hypotf(state.game.marble.vx, state.game.marble.vy) <= ORB_MARBLE_MAX_SPEED + 0.001f);
    state.game.marble.x = ORB_MARBLE_RADIUS - 12;
    state.game.marble.y = 0;
    state.game.marble.vx = 100;
    state.game.marble.vy = 0;
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.game.marble.vx < 0 && state.game.marble.x <= ORB_MARBLE_RADIUS - 12);
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    orb_game_action(&state, ORB_GAME_LEFT, 0, 0);
    orb_game_action(&state, ORB_GAME_UP, 0, 0);
    CHECK(state.game.marble.vx < 0 && state.game.marble.vy < 0);
    orb_game_action(&state, ORB_GAME_TAP, NAN, INFINITY);
    orb_game_step(&state, NAN, INFINITY, true, false);
    CHECK(isfinite(state.game.marble.x) && isfinite(state.game.marble.y));
    orb_game_state_t expected;
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    expected = state;
    orb_game_step(&state, 1e9f, -1e9f, true, false);
    orb_game_step(&expected, 1, -1, true, false);
    CHECK(!memcmp(&state, &expected, sizeof(state)));
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    expected = state;
    orb_game_step(&state, 1, 1, false, false);
    orb_game_step(&expected, 0, 0, true, false);
    CHECK(!memcmp(&state, &expected, sizeof(state)));

    /* Fixed simulation time, not renderer calls, produces the bounded path. */
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    step_n(&state, 900);
    CHECK(state.game.marble.history_count == ORB_MARBLE_HISTORY_MAX);
    CHECK(state.game.marble.history_head < ORB_MARBLE_HISTORY_MAX);
    orb_game_action(&state, ORB_GAME_RIGHT, 0, 0);
    step_n(&state, 20);
    orb_marble_sample_t path[12];
    for (unsigned i = 0; i < 12; ++i) {
        unsigned at = (state.game.marble.history_head + ORB_MARBLE_HISTORY_MAX - 2 - i) % ORB_MARBLE_HISTORY_MAX;
        path[i] = state.game.marble.history[at];
    }
    float gate_before = state.game.marble.gates[0].angle;
    orb_game_action(&state, ORB_GAME_REWIND_START, 0, 0);
    uint16_t count = state.game.marble.history_count;
    for (unsigned i = 0; i < 12; ++i) {
        step_n(&state, 2);
        CHECK(state.game.marble.x == (float)path[i].x / 16);
        CHECK(state.game.marble.y == (float)path[i].y / 16);
        CHECK(state.game.marble.history_count == count - i - 1);
        CHECK(state.score == 0);
    }
    CHECK(state.game.marble.gates[0].angle != gate_before);
    orb_game_action(&state, ORB_GAME_REWIND_STOP, 0, 0);
    CHECK(!state.game.marble.rewind_active && state.game.marble.vx == 0 && state.game.marble.vy == 0);
    CHECK(orb_game_position_clear(state.game.marble.x, state.game.marble.y, 12, state.kind, false));
    step_n(&state, 2);
    CHECK(state.game.marble.history_count == count - 11); /* New future replaces discarded samples. */
    orb_game_action(&state, ORB_GAME_REWIND_START, 0, 0);
    step_n(&state, 1000);
    CHECK(state.game.marble.rewind_active && state.game.marble.history_count == 1);
    float held_x = state.game.marble.x, held_y = state.game.marble.y;
    step_n(&state, 10);
    CHECK(state.game.marble.x == held_x && state.game.marble.y == held_y);
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    CHECK(state.user_paused && !state.game.marble.rewind_active);
    orb_game_action(&state, ORB_GAME_REWIND_STOP, 0, 0);
    CHECK(!state.game.marble.rewind_active);

    /* The non-motion control variant ignores real IMU inputs. */
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    expected = state;
    orb_game_step(&state, 1, -1, true, true);
    orb_game_step(&expected, 0, 0, false, true);
    CHECK(!memcmp(&state, &expected, sizeof(state)));

    for (unsigned stage = 0; stage < ORB_MARBLE_STAGES; ++stage) {
        state.game.marble.x = state.game.marble.y = 0;
        state.game.marble.vx = state.game.marble.vy = 0;
        step_n(&state, 1);
        CHECK(state.game.marble.stage_cleared && !state.game_over);
        CHECK(state.score == (stage + 1) * 100);
        CHECK(state.game.marble.keepsake_count == stage + 1);
        CHECK(state.game.marble.stages_cleared == stage + 1);
        gate_before = state.game.marble.gates[0].angle;
        step_n(&state, 10);
        CHECK(state.game.marble.gates[0].angle == gate_before);
        CHECK(state.game.marble.keepsake_count == stage + 1);
        orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
        CHECK(stage + 1 == ORB_MARBLE_STAGES ? state.game_over : state.game.marble.stage == stage + 1);
    }
    orb_game_action(&state, ORB_GAME_GARDEN, 0, 0);
    CHECK(state.game.marble.garden_open);
    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    CHECK(state.game.marble.keepsake_count == 3 && state.game.marble.stages_cleared == 3);
    CHECK(state.score == 0 && state.game.marble.stage == 0 && !state.game.marble.garden_open);
    state.started = false;
    orb_game_action(&state, ORB_GAME_GARDEN, 0, 0);
    CHECK(state.game.marble.garden_open);
    orb_game_action(&state, ORB_GAME_START, 0, 0);
    CHECK(state.started && !state.game.marble.garden_open);

    /* A full garden replaces its oldest planting in place, never drops a reward. */
    for (unsigned round = 0; round < 5; ++round) {
        for (unsigned stage = 0; stage < ORB_MARBLE_STAGES; ++stage) {
            orb_marble_keepsake_t oldest = state.game.marble.keepsakes[0];
            uint8_t before = state.game.marble.keepsake_count;
            state.game.marble.x = state.game.marble.y = 0;
            step_n(&state, 1);
            if (before == ORB_MARBLE_KEEPSAKES_MAX) {
                CHECK(state.game.marble.keepsake_count == before);
                orb_marble_keepsake_t newest = state.game.marble.keepsakes[ORB_MARBLE_KEEPSAKES_MAX - 1];
                CHECK(newest.x == oldest.x && newest.y == oldest.y && newest.stage == stage + 1);
            } else CHECK(state.game.marble.keepsake_count == before + 1);
            orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
        }
        orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    }
    CHECK(state.game.marble.keepsake_count == ORB_MARBLE_KEEPSAKES_MAX);

    /* Releasing a recorded path through a present-day stone stops locally. */
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
    size_t count_sprites = orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    size_t post = 0;
    for (size_t i = 0; i < count_sprites; ++i)
        if (sprites[i].shape == ORB_GAME_GATE && sprites[i].radius) { post = i; break; }
    CHECK(post > 0);
    state.game.marble.history_head = 2; state.game.marble.history_count = 2;
    float arc_middle = (sprites[post].arc_start + sprites[post].arc_end) * 0.5f;
    state.game.marble.history[0].x = (int16_t)lroundf(cosf(arc_middle) * sprites[post].arc_radius * 16);
    state.game.marble.history[0].y = (int16_t)lroundf(sinf(arc_middle) * sprites[post].arc_radius * 16);
    orb_game_action(&state, ORB_GAME_REWIND_START, 0, 0);
    step_n(&state, 2);
    float before_x = state.game.marble.x, before_y = state.game.marble.y;
    orb_game_action(&state, ORB_GAME_REWIND_STOP, 0, 0);
    CHECK(hypotf(state.game.marble.x - before_x, state.game.marble.y - before_y) <= 32.01f);
    CHECK(state.game.marble.vx == 0 && state.game.marble.vy == 0);
    orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    for (size_t i = 0; i < count_sprites; ++i)
        if (sprites[i].shape == ORB_GAME_GATE && sprites[i].radius)
            CHECK(arc_distance(&sprites[i], sprites[0].x, sprites[0].y) >= 18.0f);
}

static void physical_accelerometer_directions(void)
{
    /* Synthetic static specific force: right side down => ax < 0; bottom down
     * => ay > 0. Missing az is +sqrt(1-ax²-ay²), not a gravity-direction input. */
    const float readings[][2] = {{-0.34202014f, 0}, {0.34202014f, 0},
                                  {0, 0.34202014f}, {0, -0.34202014f}};
    for (unsigned i = 0; i < 4; ++i) {
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_MARBLE, 22);
        state.game.marble.x = -70; state.game.marble.y = 70;
        orb_game_step(&state, readings[i][0], readings[i][1], true, false);
        CHECK(i == 0 ? state.game.marble.vx > 0 : i == 1 ? state.game.marble.vx < 0 :
              state.game.marble.vx == 0);
        CHECK(i == 2 ? state.game.marble.vy > 0 : i == 3 ? state.game.marble.vy < 0 :
              state.game.marble.vy == 0);
    }
    orb_game_state_t snake_state;
    orb_game_init(&snake_state, ORB_GAME_SNAKE, 77);
    orb_game_step(&snake_state, -0.34202014f, 0, true, false);
    orb_game_step(&snake_state, -0.34202014f, 0, true, false);
    CHECK(snake_state.game.snake.cells[0].x > 0); /* Right-side-down steers right. */
    orb_game_init(&snake_state, ORB_GAME_SNAKE, 77);
    orb_game_step(&snake_state, 0.34202014f, 0, true, false);
    orb_game_step(&snake_state, 0.34202014f, 0, true, false);
    CHECK(snake_state.game.snake.cells[0].x < 0); /* Left-side-down steers left. */
}

static void neutral_tilt_calibration(void)
{
    const float radians = 3.1415926535897932385f / 180;
    for (unsigned angle = 20; angle <= 50; angle += 15) {
        float pitch = (float)angle * radians;
        float sy = sinf(pitch), sz = cosf(pitch);
        orb_game_tilt_t tilt;
        orb_game_tilt_reset(&tilt);
        float ax = 1, ay = 1;
        CHECK(!orb_game_tilt_update(&tilt, 0, sy, sz, 0, &ax, &ay));
        CHECK(tilt.average_us == 0 && ax == 0 && ay == 0);
        for (unsigned i = 0; i < 14; ++i) {
            CHECK(!orb_game_tilt_update(&tilt, 0, sy, sz, 20000, &ax, &ay));
            CHECK(ax == 0 && ay == 0);
        }
        CHECK(!tilt.ready && tilt.average_us == 280000);
        CHECK(orb_game_tilt_update(&tilt, 0, sy, sz, 20000, &ax, &ay));
        CHECK(tilt.ready && tilt.average_us == 300000 && ax == 0 && ay == 0);
        CHECK(orb_game_tilt_update(&tilt, 0, sy, sz, 20000, &ax, &ay));
        CHECK(fabsf(ax) < 0.000001f && fabsf(ay) < 0.000001f); /* Captured viewing angle is level. */
        float small = 0.5f * radians;
        CHECK(orb_game_tilt_update(&tilt, -sinf(small), sy * cosf(small), sz * cosf(small),
                                  20000, &ax, &ay));
        CHECK(ax < 0 && fabsf(ay) < 0.000001f); /* Fine input has no hard dead zone. */

        /* Tilt right relative to a pitched neutral: y changes despite zero
         * relative vertical tilt. A plain x/y offset would create false drift. */
        float roll = 20 * radians;
        CHECK(orb_game_tilt_update(&tilt, -sinf(roll), sy * cosf(roll), sz * cosf(roll),
                                  20000, &ax, &ay));
        float first = ax;
        CHECK(first < -0.1f && fabsf(ay) < 0.00001f);
        for (unsigned i = 0; i < 10; ++i)
            CHECK(orb_game_tilt_update(&tilt, -sinf(roll), sy * cosf(roll), sz * cosf(roll),
                                      20000, &ax, &ay));
        CHECK(ax < first && fabsf(first / ax) > 0.90f); /* Flick opens the cutoff. */
        CHECK(fabsf(ay) < 0.00001f && fabsf(ax) < sinf(roll)); /* Gentle curve. */

        /* Explicit restart chooses a new 3D neutral and drops filter state. */
        orb_game_tilt_reset(&tilt);
        float next = pitch + 10 * radians;
        for (unsigned i = 0; i < 15; ++i)
            (void)orb_game_tilt_update(&tilt, 0, sinf(next), cosf(next), 20000, &ax, &ay);
        CHECK(tilt.ready && ax == 0 && ay == 0);
        /* Bottom down relative to this neutral produces positive screen-up
         * specific force, so LVGL marble acceleration is down. */
        CHECK(orb_game_tilt_update(&tilt, 0, sinf(next + roll), cosf(next + roll),
                                  20000, &ax, &ay));
        CHECK(fabsf(ax) < 0.00001f && ay > 0.1f);
    }
    orb_game_tilt_t tilt;
    orb_game_tilt_reset(&tilt);
    float ax, ay;
    /* Alternating sample noise has zero mean. Calibration must average the
     * 300 ms window instead of adopting whichever sample completes it. */
    for (unsigned i = 0; i < 20; ++i) {
        bool ready = orb_game_tilt_update(&tilt, i % 2 ? 0.03f : -0.03f,
            0.5f, 0.8660254f, 15000, &ax, &ay);
        CHECK(ready == (i == 19));
    }
    CHECK(orb_game_tilt_update(&tilt, 0, 0.5f, 0.8660254f, 20000, &ax, &ay));
    CHECK(fabsf(ax) < 0.000001f && fabsf(ay) < 0.000001f);
    orb_game_tilt_reset(&tilt);
    CHECK(!orb_game_tilt_update(&tilt, NAN, 0, 1, 20000, &ax, &ay));
    CHECK(!orb_game_tilt_update(&tilt, 0, 0, 0, 20000, &ax, &ay));
    CHECK(tilt.average_us == 0);
    CHECK(!orb_game_tilt_update(&tilt, 0, 0, 1, 300000, &ax, &ay));
    CHECK(tilt.average_us == 0); /* One delayed sample never calibrates alone. */
}

/* Recorded-style traces: sensor cadence and game cadence are deliberately
 * different, with board-scale noise and the lead's measured flat offset. */
static orb_game_point_t trace_sample(orb_game_tilt_t *tilt, orb_game_state_t *state,
                                     const float force[3], uint32_t sample_us,
                                     uint32_t *step_us)
{
    orb_game_point_t output;
    bool ready = orb_game_tilt_update(tilt, force[0], force[1], force[2], sample_us,
                                      &output.x, &output.y);
    if (state) orb_game_set_motion(state, tilt);
    if (state && ready) {
        *step_us += sample_us;
        while (*step_us >= ORB_GAME_STEP_US) {
            orb_game_step(state, output.x, output.y, true, false);
            *step_us -= ORB_GAME_STEP_US;
        }
    }
    return output;
}

static void trace_relative_roll(const orb_game_tilt_t *tilt, const float neutral[3],
                                 float roll, float force[3])
{
    float magnitude = sqrtf(neutral[0] * neutral[0] + neutral[1] * neutral[1] +
                            neutral[2] * neutral[2]);
    for (unsigned axis = 0; axis < 3; ++axis)
        force[axis] = neutral[axis] * cosf(roll) -
            tilt->right[axis] * magnitude * sinf(roll);
}

static void recorded_motion_traces(void)
{
    const float poses[][3] = {
        {0.025f, 0.036f, 0.924f}, /* Actual dev-board flat desk sample. */
        {0.250f, 0.350f, 0.850f}, /* Tilted desk. */
        {-0.120f, 0.620f, 0.720f}, /* Comfortable in-hand viewing angle. */
    };
    for (unsigned pose = 0; pose < 3; ++pose) {
        for (unsigned cadence = 0; cadence < 2; ++cadence) {
            uint32_t sample_us = cadence ? 5000u : 10000u;
            orb_game_tilt_t tilt;
            orb_game_tilt_reset(&tilt);
            uint32_t step_us = 0;
            for (unsigned i = 0; i < 300000u / sample_us; ++i)
                (void)trace_sample(&tilt, NULL, poses[pose], sample_us, &step_us);
            CHECK(tilt.ready);
            orb_game_state_t snake_state, marble_state;
            orb_game_init(&snake_state, ORB_GAME_SNAKE, 77);
            orb_game_init(&marble_state, ORB_GAME_MARBLE, 22);
            /* Static geometry isolates input drift from moving gate contact. */
            for (unsigned g = 0; g < ORB_MARBLE_GATES_MAX; ++g)
                marble_state.game.marble.gates[g].active = false;
            marble_state.game.marble.x = 100;
            marble_state.game.marble.y = 60;
            uint32_t snake_us = 0, marble_us = 0;
            for (unsigned i = 0; i < 3200000u / sample_us; ++i) {
                float noise = (float)((int)(i % 7) - 3) * 0.0008f;
                float force[3] = {poses[pose][0] + noise, poses[pose][1] - noise,
                                   poses[pose][2] + noise * 0.5f};
                orb_game_point_t output = trace_sample(&tilt, NULL, force, sample_us, &step_us);
                CHECK(hypotf(output.x, output.y) < 0.004f);
                snake_us += sample_us;
                marble_us += sample_us;
                while (snake_us >= ORB_GAME_STEP_US) {
                    orb_game_step(&snake_state, output.x, output.y, true, false);
                    snake_us -= ORB_GAME_STEP_US;
                }
                while (marble_us >= ORB_GAME_STEP_US) {
                    orb_game_step(&marble_state, output.x, output.y, true, false);
                    marble_us -= ORB_GAME_STEP_US;
                }
            }
            CHECK(fabsf(remainderf(snake_state.game.snake.heading + 1.5707963f, 6.2831853f)) < .001f);
            CHECK(hypotf(marble_state.game.marble.x - 100,
                         marble_state.game.marble.y - 60) < 0.5f);

        }
    }

    orb_game_tilt_t tilt;
    orb_game_tilt_reset(&tilt);
    uint32_t step_us = 0;
    const float *neutral = poses[0];
    for (unsigned i = 0; i < 30; ++i)
        (void)trace_sample(&tilt, NULL, neutral, 10000, &step_us);
    /* A slow hand movement stays monotonic, uncoupled and finely controllable. */
    orb_game_point_t previous = {0, 0};
    for (unsigned i = 1; i <= 100; ++i) {
        float force[3];
        trace_relative_roll(&tilt, neutral, (float)i * 0.0018f, force);
        orb_game_point_t output = trace_sample(&tilt, NULL, force, 10000, &step_us);
        CHECK(output.x <= previous.x + 0.00001f && fabsf(output.y) < 0.00001f);
        previous = output;
    }
    CHECK(previous.x < -0.11f);
    /* Beads now has an independent true-gravity/gyro path, traced end to end
     * in sim/tests/test_games_rest.c. It must not inherit this play neutral. */
    /* Quick flicks react within one 100 Hz sample, without an 80 ms low-pass. */
    for (unsigned i = 0; i < 30; ++i)
        (void)trace_sample(&tilt, NULL, neutral, 10000, &step_us);
    float force[3];
    trace_relative_roll(&tilt, neutral, 0.45f, force);
    orb_game_point_t flick = trace_sample(&tilt, NULL, force, 10000, &step_us);
    CHECK(flick.x < -0.30f && fabsf(flick.y) < 0.00001f);
    for (unsigned i = 0; i < 30; ++i)
        (void)trace_sample(&tilt, NULL, neutral, 10000, &step_us);
    CHECK(hypotf(tilt.filtered_x, tilt.filtered_y) < 0.001f);

    /* Impact/free-fall portions of shaking never become a steering command or
     * a captured neutral. Finite but changing gravity directions stay bounded. */
    orb_game_tilt_t shaking;
    orb_game_tilt_reset(&shaking);
    for (unsigned i = 0; i < 100; ++i) {
        const float shock[][3] = {{1.8f, -0.8f, 0.7f}, {0, 0, 0.08f}};
        orb_game_point_t rejected = trace_sample(&shaking, NULL, shock[i % 2], 10000, &step_us);
        CHECK(!shaking.ready && shaking.average_us == 0 && rejected.x == 0 && rejected.y == 0);
        orb_game_point_t suppressed = trace_sample(&tilt, NULL, shock[i % 2], 10000, &step_us);
        CHECK(suppressed.x == 0 && suppressed.y == 0 && tilt.ready);
    }
    for (unsigned i = 0; i < 100; ++i) {
        trace_relative_roll(&tilt, neutral, i % 2 ? -0.35f : 0.35f, force);
        orb_game_point_t output = trace_sample(&tilt, NULL, force, 10000, &step_us);
        CHECK(isfinite(output.x) && isfinite(output.y) && hypotf(output.x, output.y) <= 1);
        (void)trace_sample(&shaking, NULL, force, 10000, &step_us);
        CHECK(!shaking.ready); /* Motion cannot complete the rest-pose window. */
    }
    for (unsigned i = 0; i < 30; ++i)
        (void)trace_sample(&shaking, NULL, neutral, 10000, &step_us);
    CHECK(shaking.ready); /* Recovery needs no restart or power cycle. */

    /* Returning level retains the heading; a later dip steers toward the
     * same absolute direction, without another relative turn. */
    orb_game_tilt_reset(&tilt);
    for (unsigned i = 0; i < 30; ++i)
        (void)trace_sample(&tilt, NULL, neutral, 10000, &step_us);
    orb_game_state_t snake_state;
    orb_game_init(&snake_state, ORB_GAME_SNAKE, 77);
    step_us = 0;
    for (unsigned i = 0; i < 61; ++i) {
        bool dip = (i >= 6 && i < 14) || (i >= 24 && i < 32);
        trace_relative_roll(&tilt, neutral, dip ? 0.30f : 0, force);
        (void)trace_sample(&tilt, &snake_state, force, 10000, &step_us);
        CHECK(!snake_state.game_over);
    }
    CHECK(snake_state.game.snake.cells[0].x > 0);
    CHECK(snake_state.game.snake.heading > -1.5707963f);
}

/* Exercise complete stages through public fake inputs rather than moving the
 * marble onto its goal. Both steering alternatives must remain solvable. */
static void marble_short_stages(void)
{
    for (unsigned touch = 0; touch < 2; ++touch) {
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_MARBLE, 22);
        if (touch) orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
        for (unsigned stage = 0; stage < ORB_MARBLE_STAGES; ++stage) {
            unsigned steps = 0;
            for (; steps < 60 * 60 && !state.game.marble.stage_cleared; ++steps) {
                float x = state.game.marble.x, y = state.game.marble.y;
                if (touch && steps % 8 == 0) {
                    /* Spiral around the physical arrow footprints instead of
                     * repeatedly pushing straight into the same control. Slow
                     * the approach outside the flower, then centre the marble. */
                    float radius = hypotf(x, y);
                    float swirl = radius > 50 ? 0.5f : 0;
                    float brake = radius > 50 ? 2.0f : 0;
                    float dx = -x - y * swirl - state.game.marble.vx * brake;
                    float dy = -y + x * swirl - state.game.marble.vy * brake;
                    if (dx < -4) orb_game_action(&state, ORB_GAME_LEFT, 0, 0);
                    if (dx > 4) orb_game_action(&state, ORB_GAME_RIGHT, 0, 0);
                    if (dy < -4) orb_game_action(&state, ORB_GAME_UP, 0, 0);
                    if (dy > 4) orb_game_action(&state, ORB_GAME_DOWN, 0, 0);
                }
                /* Acceleration points inward, so specific force points outward.
                 * Unit-length static accelerometer vector has a real +z. */
                float ax = x * 0.02f, ay = -y * 0.02f;
                float scale = fmaxf(1, hypotf(ax, ay) / 0.85f);
                orb_game_step(&state, ax / scale, ay / scale, true, false);
            }
            CHECK(state.game.marble.stage_cleared && steps < 60 * 60);
            CHECK(state.game.marble.keepsake_count == stage + 1);
            CHECK(state.score == (stage + 1) * 100);
            orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
        }
        CHECK(state.game_over && state.game.marble.stages_cleared == ORB_MARBLE_STAGES);
    }
}

static void marble_solid_gates_and_history_ink(void)
{
    orb_game_state_t state;
    orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    size_t count = orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    unsigned gates = 0;
    bool inner = false, middle = false, outer = false;
    for (size_t i = 0; i < count; ++i) {
        if (sprites[i].shape != ORB_GAME_GATE || !sprites[i].radius) continue;
        ++gates;
        CHECK(sprites[i].radius * 2 == 12);
        CHECK(sprites[i].arc_end > sprites[i].arc_start);
        inner |= sprites[i].arc_radius == 64;
        middle |= sprites[i].arc_radius == 126;
        outer |= sprites[i].arc_radius == 188;
    }
    CHECK(gates >= 3 && inner && middle && outer);
    CHECK(sprites[0].radius == ORB_MARBLE_BALL_RADIUS &&
          sprites[1].radius == ORB_MARBLE_FLOWER_RADIUS);

    /* The midpoint between the former posts is now just as solid as the rest
     * of the arch. The intentional gap still admits the same inward motion. */
    for (unsigned gap = 0; gap < 2; ++gap) {
        orb_game_init(&state, ORB_GAME_MARBLE, 22);
        state.game.marble.gates[0].speed = 0;
        float angle = state.game.marble.gates[0].angle + (gap ? 0 : 3.14159265f);
        state.game.marble.x = cosf(angle) * 83;
        state.game.marble.y = sinf(angle) * 83;
        state.game.marble.vx = -cosf(angle) * 180;
        state.game.marble.vy = -sinf(angle) * 180;
        step_n(&state, 12);
        float radius = hypotf(state.game.marble.x, state.game.marble.y);
        CHECK(gap ? radius < 64 : radius > 82);
    }

    /* Touch input has no floating buttons and must preserve each full arch.
     * Sample the whole visible stroke, not just its bounding-box centre. */
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    count = orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    unsigned active_arcs = 0;
    for (size_t i = 0; i < count; ++i)
        if (sprites[i].shape == ORB_GAME_GATE && sprites[i].radius) ++active_arcs;
    CHECK(active_arcs == 3);
    for (unsigned frame = 0; frame < 60; ++frame) {
        count = orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
        for (size_t i = 0; i < count; ++i) {
            if (sprites[i].shape != ORB_GAME_GATE || !sprites[i].radius) continue;
            for (unsigned sample = 0; sample <= 24; ++sample) {
                float angle = sprites[i].arc_start +
                    (sprites[i].arc_end - sprites[i].arc_start) * (float)sample / 24;
                CHECK(orb_game_position_clear(cosf(angle) * sprites[i].arc_radius,
                    sinf(angle) * sprites[i].arc_radius, 6, ORB_GAME_MARBLE, true));
            }
        }
        step_n(&state, 1);
    }

    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    orb_game_action(&state, ORB_GAME_RIGHT, 0, 0);
    step_n(&state, 80);
    count = orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    unsigned trail = 2 + ORB_MARBLE_GATES_MAX * ORB_MARBLE_GATE_SEGMENTS;
    CHECK(sprites[trail].radius == 4 && sprites[trail].colour == 0xffffff);
    CHECK(sprites[trail + 3].radius < sprites[trail].radius);
    CHECK(sprites[trail].opacity <= 130 && sprites[trail + 3].opacity < sprites[trail].opacity);
    uint8_t before = sprites[trail].opacity;
    float angle_before = state.game.marble.gates[0].angle;
    orb_game_action(&state, ORB_GAME_REWIND_START, 0, 0);
    step_n(&state, 20);
    orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    CHECK(sprites[trail].opacity < before);
    CHECK(state.game.marble.gates[0].angle != angle_before);
    state.reduce_motion = true;
    orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    CHECK(sprites[trail].radius == 0);

    /* A leftward rewind must leave its ghost to the right, at positions just
     * departed. Sampling older history would wrongly draw the future ahead. */
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    for (unsigned i = 0; i < 30; ++i)
        state.game.marble.history[i] = (orb_marble_sample_t){(int16_t)((100 + i * 2) * 16), 0, 0, 0};
    state.game.marble.history_head = state.game.marble.history_count = 30;
    state.game.marble.x = 158;
    state.game.marble.y = 0;
    orb_game_action(&state, ORB_GAME_REWIND_START, 0, 0);
    CHECK(state.game.marble.trail_count == 0);
    step_n(&state, 18);
    CHECK(state.game.marble.x == 140 && state.game.marble.trail_count == 3);
    for (unsigned i = 0; i < state.game.marble.trail_count; ++i) {
        CHECK(state.game.marble.trail[i].x > state.game.marble.x);
        CHECK(state.game.marble.trail[i].y == 0);
        if (i) CHECK(state.game.marble.trail[i].x > state.game.marble.trail[i - 1].x);
    }
}

static void marble_full_round_geometry(void)
{
    for (unsigned touch = 0; touch < 2; ++touch) {
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_MARBLE, 22);
        if (touch) orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
        for (unsigned stage = 0; stage < ORB_MARBLE_STAGES; ++stage) {
            CHECK(hypotf(state.game.marble.x, state.game.marble.y) > 208);
            unsigned active = 0;
            float outer = 0;
            for (unsigned g = 0; g < ORB_MARBLE_GATES_MAX; ++g) {
                const orb_marble_gate_t *gate = &state.game.marble.gates[g];
                if (!gate->active) continue;
                ++active;
                CHECK(gate->radius > outer);
                if (outer > 0) CHECK(gate->radius - outer >= 60);
                outer = gate->radius;
            }
            CHECK(active == 3);
            CHECK(outer == 188);
            CHECK(233 - outer - ORB_MARBLE_GATE_WIDTH / 2 == 39);
            for (unsigned frame = 0; frame < 120; ++frame) {
                orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
                size_t count = orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
                for (size_t i = 0; i < count; ++i) {
                    const orb_game_sprite_t *arc = &sprites[i];
                    if (arc->shape != ORB_GAME_GATE || !arc->radius) continue;
                    for (unsigned sample = 0; sample <= 24; ++sample) {
                        float angle = arc->arc_start + (arc->arc_end - arc->arc_start) *
                            (float)sample / 24;
                        CHECK(orb_game_position_clear(cosf(angle) * arc->arc_radius,
                            sinf(angle) * arc->arc_radius, arc->radius, ORB_GAME_MARBLE, touch));
                    }
                }
                /* A reserved control pocket must never create a second usable
                 * gate opening. Sample the marble-centre locus of each ring:
                 * every passable point belongs to that ring's rotating gap. */
                for (unsigned g = 0; g < ORB_MARBLE_GATES_MAX; ++g) {
                    const orb_marble_gate_t *gate = &state.game.marble.gates[g];
                    if (!gate->active) continue;
                    for (unsigned sample = 0; sample < 72; ++sample) {
                        float angle = (float)sample * 6.28318531f / 72;
                        float x = cosf(angle) * gate->radius, y = sinf(angle) * gate->radius;
                        if (!orb_game_position_clear(x, y, ORB_MARBLE_BALL_RADIUS,
                            ORB_GAME_MARBLE, touch)) continue;
                        bool blocked = false;
                        for (size_t i = 0; i < count; ++i)
                            if (sprites[i].shape == ORB_GAME_GATE && sprites[i].radius &&
                                sprites[i].arc_radius == gate->radius &&
                                arc_distance(&sprites[i], ORB_GAME_CX + x, ORB_GAME_CY + y) < 18)
                                blocked = true;
                        if (blocked) continue;
                        float relative = angle - gate->angle;
                        while (relative > 3.14159265f) relative -= 6.28318531f;
                        while (relative < -3.14159265f) relative += 6.28318531f;
                        CHECK(fabsf(relative) < gate->gap_angle);
                    }
                }
                step_n(&state, 1);
            }
            state.game.marble.x = state.game.marble.y = 0;
            state.game.marble.vx = state.game.marble.vy = 0;
            step_n(&state, 1);
            orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
        }
    }
}

static void keepsake_records(void)
{
    orb_keepsake_record_t record = {.magic = {'O', 'G', 'K', '1'}, .version = 1, .count = 2, .stages_cleared = 2};
    record.items[0].kind = 0; record.items[0].stage = 1;
    record.items[0].x = -45; record.items[0].y = 50;
    record.items[1].kind = 1; record.items[1].stage = 2;
    record.items[1].x = 80; record.items[1].y = -20;
    uint8_t bytes[ORB_KEEPSAKE_RECORD_SIZE];
    CHECK(orb_keepsake_encode(&record, bytes, sizeof(bytes)) == sizeof(bytes));
    CHECK(bytes[8 + 2] == 211 && bytes[8 + 3] == 255); /* Explicit little endian negative coordinate. */
    orb_keepsake_record_t decoded;
    CHECK(orb_keepsake_decode(bytes, sizeof(bytes), &decoded));
    CHECK(decoded.count == 2 && decoded.stages_cleared == 2);
    CHECK(decoded.items[0].x == -45 && decoded.items[1].y == -20);
    CHECK(!orb_keepsake_decode(bytes, sizeof(bytes) - 1, &decoded));
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        bytes[i] ^= 1;
        CHECK(!orb_keepsake_decode(bytes, sizeof(bytes), &decoded));
        bytes[i] ^= 1;
    }
    record.version = 2;
    CHECK(!orb_keepsake_encode(&record, bytes, sizeof(bytes)));
    record.version = 1; record.count = ORB_MARBLE_KEEPSAKES_MAX + 1;
    CHECK(!orb_keepsake_encode(&record, bytes, sizeof(bytes)));
    record.count = 2; record.items[1].kind = 4;
    CHECK(!orb_keepsake_encode(&record, bytes, sizeof(bytes)));
    record.items[1].kind = 1; record.items[1].x = 300;
    CHECK(!orb_keepsake_encode(&record, bytes, sizeof(bytes)));
    record.items[1].x = 50; record.items[1].stage = 3;
    CHECK(!orb_keepsake_encode(&record, bytes, sizeof(bytes)));
    CHECK(!orb_keepsake_decode(NULL, sizeof(bytes), &decoded));
}

static void orbit(void)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_ORBIT, 9);
    CHECK(state.game.orbit.ring == 0);
    orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
    CHECK(state.game.orbit.ring == 1);
    orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
    CHECK(state.game.orbit.ring == 2);
    orb_game_action(&state, ORB_GAME_TAP, 233, 245);
    CHECK(state.game.orbit.ring == 0); /* Three rings wrap back to the inner. */
    /* A comet on the planet's ring near its angle costs a life; a clean pass
     * costs nothing. */
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
        state.game.orbit.obstacles[i] = (orb_game_obstacle_t){0, 1, false};
    state.game.orbit.ring = 0;
    state.game.orbit.angle = 0;
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){0.0f, 0, false};
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.lives == 2);
    step_n(&state, 10);
    CHECK(state.lives == 2); /* One colliding comet costs at most one life. */
    /* A star on the same ring at the planet's angle is collected for 10. */
    uint32_t before = state.score;
    state.game.orbit.stars[0] = (orb_game_star_t){state.game.orbit.angle, 0, true};
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.score == before + 10);
    CHECK(!state.game.orbit.stars[0].active);
    for (unsigned i = 0; i < 2; ++i) {
        step_n(&state, ORB_GAME_ORBIT_HIT_GRACE);
        state.game.orbit.obstacles[0] = (orb_game_obstacle_t){state.game.orbit.angle, 0, false};
        orb_game_step(&state, 0, 0, false, false);
    }
    CHECK(state.game_over && state.lives == 0);
    uint32_t ticks = state.ticks;
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.ticks == ticks);
    orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
    CHECK(state.game.orbit.ring == 0);
    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    CHECK(!state.game_over && state.lives == 3 && state.score == 0);
}

static void orbit_comet_paths(void)
{
    orb_game_state_t state;
    orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
    orb_game_init(&state, ORB_GAME_ORBIT, 9);
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
        state.game.orbit.obstacles[i].ring = ORB_GAME_ORBIT_RINGS;
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){0, 0, false};
    state.game.orbit.comet_radius[0] = 166.3f;
    state.game.orbit.comet_speed[0] = -1.05f;
    orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    CHECK(fabsf(sprites[7].x - ORB_GAME_CX - 166.3f) < .01f);
    CHECK(sprites[8].y > sprites[7].y && sprites[9].y > sprites[8].y);
    for (unsigned i = 10; i < 19; ++i) CHECK(sprites[i].radius == 0);
    state.game.orbit.comet_speed[0] = 1.05f;
    orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    CHECK(sprites[8].y < sprites[7].y && sprites[9].y < sprites[8].y);

    /* Collision follows the displayed path, even between the usual lanes.
     * An empty slot stays absent after wrapping and cannot cost a life. */
    state.game.orbit.ring = 1;
    state.game.orbit.angle = 0;
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.lives == 3); /* 166.3 is beyond the planet's r136 lane. */
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){0, 0, false};
    state.game.orbit.comet_radius[0] = ORB_GAME_ORBIT_R1;
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.lives == 2); /* Same drawn radius, despite a different lane ID. */
    state.game.orbit.obstacles[0].ring = ORB_GAME_ORBIT_RINGS;
    state.game.orbit.obstacles[0].angle = 6.28f;
    step_n(&state, 30);
    CHECK(state.game.orbit.obstacles[0].ring == ORB_GAME_ORBIT_RINGS);
    CHECK(state.lives == 2);
}

static void orbit_recovery_and_warning(void)
{
    orb_game_state_t state;
    orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
    orb_game_init(&state, ORB_GAME_ORBIT, 9);
    state.game.orbit.angle = 0;
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
        state.game.orbit.obstacles[i] = (orb_game_obstacle_t){0, 0, false};
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.lives == 2 && !state.game_over);
    CHECK(state.game.orbit.hit_grace == ORB_GAME_ORBIT_HIT_GRACE);
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
        CHECK(state.game.orbit.obstacles[i].passed);
    orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    CHECK(sprites[4].shape == ORB_GAME_RING && sprites[4].opacity == 255);
    /* The recovery signal remains a static shape with Reduce Motion. */
    step_n(&state, ORB_GAME_ORBIT_HIT_GRACE - 1);
    orb_game_step(&state, 0, 0, false, true);
    CHECK(!state.game.orbit.hit_grace && state.lives == 2);
    orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
    CHECK(sprites[4].shape == ORB_GAME_DISC);
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){state.game.orbit.angle, 0, false};
    orb_game_step(&state, 0, 0, false, true);
    CHECK(state.lives == 1 && state.game.orbit.hit_grace == ORB_GAME_ORBIT_HIT_GRACE);

    orb_game_init(&state, ORB_GAME_ORBIT, 9);
    state.game.orbit.angle = 0;
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
        state.game.orbit.obstacles[i].ring = ORB_GAME_ORBIT_RINGS;
    /* A global 2pi wrap near the player cannot change the hazard's lane. */
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){6.28f, 2, true};
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.game.orbit.obstacles[0].ring == 2);
    CHECK(state.game.orbit.obstacles[0].passed && state.lives == 3);
    /* Re-arming happens half an orbit away, in either angular direction. */
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){state.game.orbit.angle + 3.137f, 2, true};
    orb_game_step(&state, 0, 0, false, false);
    CHECK(!state.game.orbit.obstacles[0].passed && state.lives == 3);
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){state.game.orbit.angle + 3.147f, 2, true};
    state.game.orbit.comet_speed[0] = -1.05f;
    state.game.orbit.comet_radius[0] = 166.3f;
    orb_game_step(&state, 0, 0, false, false);
    CHECK(!state.game.orbit.obstacles[0].passed);
    CHECK(state.game.orbit.obstacles[0].ring == 2 && state.lives == 3);
}

static void snake(void)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_SNAKE, 77);
    CHECK(state.game.snake.cells[0].x == 0 && state.game.snake.cells[0].y == 0);
    CHECK(state.game.snake.grace == 60 && state.game.snake.length == 6);
    CHECK(fabsf(state.game.snake.heading + 1.5707963f) < .00001f);
    step_n(&state, 60);
    CHECK(!state.game_over && !state.game.snake.grace);
    CHECK(fabsf(state.game.snake.cells[0].x) < .01f && state.game.snake.cells[0].y < -77);
    step_n(&state, 15);
    CHECK(state.score == 10 && state.game.snake.length == 7);
    orb_game_point_t food = state.game.snake.food;
    CHECK(hypotf(food.x, food.y) <= 155);
    for (unsigned i = 0; i < state.game.snake.length; ++i)
        CHECK(hypotf(food.x - state.game.snake.cells[i].x, food.y - state.game.snake.cells[i].y) >= 35);
    for (unsigned i = 0; i < 120 && !state.game_over; ++i) step_n(&state, 1);
    CHECK(state.game_over && !state.game.snake.won); /* Continuous straight run reaches the rim. */
    uint32_t frozen = state.ticks;
    step_n(&state, 8);
    CHECK(state.ticks == frozen);

    orb_game_init(&state, ORB_GAME_SNAKE, 77);
    float heading = state.game.snake.heading;
    orb_game_step(&state, -.4f, 0, true, false);
    CHECK(fabsf(remainderf(state.game.snake.heading - heading, 6.2831853f) -
        ORB_GAME_SNAKE_TURN_RATE / 60) < .00001f);
    for (unsigned i = 0; i < 35; ++i) orb_game_step(&state, -.4f, 0, true, false);
    CHECK(fabsf(remainderf(state.game.snake.heading, 6.2831853f)) < .0001f);
    heading = state.game.snake.heading;
    step_n(&state, 20);
    CHECK(state.game.snake.heading == heading); /* Level keeps going straight. */
    for (unsigned i = 0; i < 40; ++i) orb_game_step(&state, 0, .4f, true, false);
    CHECK(fabsf(state.game.snake.heading - 1.5707963f) < .0001f); /* Absolute down, no re-arm. */

    orb_game_init(&state, ORB_GAME_SNAKE, 77);
    orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    orb_game_action(&state, ORB_GAME_STEER_START, 400, 233);
    for (unsigned i = 0; i < 40; ++i) orb_game_step(&state, .8f, -.8f, true, false);
    CHECK(state.game.snake.heading < 1 && state.game.snake.heading > -1);
    orb_game_action(&state, ORB_GAME_STEER_STOP, 0, 0);
    heading = state.game.snake.heading;
    for (unsigned i = 0; i < 12; ++i) orb_game_step(&state, .8f, -.8f, true, false);
    CHECK(heading == state.game.snake.heading); /* IMU cannot fight Touch Controls. */
    orb_game_action(&state, ORB_GAME_STEER_START, 100, 233);
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    CHECK(state.user_paused && !state.game.snake.touch_held);
    frozen = state.ticks;
    step_n(&state, 120);
    CHECK(state.ticks == frozen);
    orb_game_action(&state, ORB_GAME_STEER_STOP, 0, 0); /* Cancellation accepted while paused. */
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    step_n(&state, 1);
    CHECK(!state.user_paused && state.ticks == frozen + 1);

    orb_game_init(&state, ORB_GAME_SNAKE, 77);
    state.game.snake.grace = 0;
    state.game.snake.length = 7;
    state.game.snake.cells[4] = (orb_game_point_t){0, -10};
    state.game.snake.cells[5] = (orb_game_point_t){0, -22};
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.game_over); /* The visible round body, not an invisible grid cell. */

    orb_game_init(&state, ORB_GAME_SNAKE, 77);
    for (unsigned i = 0; i < ORB_GAME_SNAKE_MAX - 6; ++i) {
        /* Isolated collection fixture exercises every growth and terminal
         * score edge without changing the production movement rules. */
        state.game.snake.food = state.game.snake.cells[0];
        orb_game_step(&state, 0, 0, false, false);
        CHECK(state.game.snake.length == i + 7);
        CHECK(state.score == (i + 1) * 10);
    }
    CHECK(state.game_over && state.game.snake.won);
    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    CHECK(!state.game_over && !state.game.snake.won && state.score == 0 && state.game.snake.grace == 60);
}

static void beads(void)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_BEADS, 31);
    CHECK(!state.game.beads.time_valid);
    /* A zero-projection entry has no downhill force to rearrange its spawn.
     * It must already read as a pool: empty above a level free surface, not
     * a disc-filling texture. Real tilted pours are tested separately below. */
    float surface[7] = {999, 999, 999, 999, 999, 999, 999};
    float centre_x = 0;
    for (unsigned i = 0; i < state.game.beads.count; ++i) {
        const orb_game_bead_t *b = &state.game.beads.beads[i];
        CHECK(b->y > 0);
        centre_x += b->x;
        if (b->x >= -140 && b->x < 140) {
            unsigned bin = (unsigned)((b->x + 140) / 40);
            surface[bin] = fminf(surface[bin], b->y);
        }
    }
    CHECK(fabsf(centre_x / state.game.beads.count) < 1);
    float top = surface[0], bottom = surface[0];
    for (unsigned bin = 0; bin < 7; ++bin) {
        CHECK(surface[bin] < 999);
        top = fminf(top, surface[bin]);
        bottom = fmaxf(bottom, surface[bin]);
    }
    CHECK(bottom - top <= 2.0f * ORB_GAME_BEAD_RADIUS);
    step_n(&state, 1800);
    CHECK(!state.game.beads.settled && state.game.beads.settle_ticks == 0);
    CHECK(state.game.beads.flow_speed < 0.1f); /* Inertia decays without inventing a clock or a hard stop. */
    orb_game_beads_set_time(&state, 0, 0);
    CHECK(state.game.beads.time_valid); /* Midnight is a valid first time too. */
    step_n(&state, ORB_GAME_BEAD_SETTLE_STEPS + 45);
    CHECK(state.game.beads.settled);
    orb_game_init(&state, ORB_GAME_BEADS, 31);
    orb_game_beads_set_time(&state, 10, 9);
    CHECK(state.game.beads.hour == 10 && state.game.beads.minute == 9);
    CHECK(!state.game.beads.settled && state.game.beads.settle_ticks == 0);
    /* Every bead starts finite, inside the field, and stays so while tilted. */
    for (unsigned i = 0; i < ORB_GAME_BEAD_COUNT; ++i) {
        const orb_game_bead_t *b = &state.game.beads.beads[i];
        CHECK(isfinite(b->x) && isfinite(b->y) && isfinite(b->vx) && isfinite(b->vy));
        CHECK(hypotf(b->x, b->y) < ORB_GAME_RADIUS);
        for (unsigned j = 0; j < i; ++j) {
            const orb_game_bead_t *a = &state.game.beads.beads[j];
            /* The resting pool is hexagonally packed at exactly one collision
             * diameter: no overlap to untangle, and the 5 px ink keeps a 4 px
             * visible gap. (The old full-disc lattice used 15 px.) */
            CHECK(hypotf(a->x - b->x, a->y - b->y) >= 2.0f * ORB_GAME_BEAD_RADIUS - 0.01f);
        }
    }
    for (unsigned i = 0; i < 240; ++i)
        orb_game_step(&state, 0.3f, -0.2f, true, false);
    for (unsigned i = 0; i < ORB_GAME_BEAD_COUNT; ++i)
        CHECK(hypotf(state.game.beads.beads[i].x, state.game.beads.beads[i].y) <=
              ORB_GAME_RADIUS - ORB_GAME_BEAD_RADIUS + 0.01f);
    CHECK(!state.game.beads.settled);
    /* Sampled stillness begins travel into the current time. */
    step_n(&state, ORB_GAME_BEAD_SETTLE_STEPS);
    CHECK(state.game.beads.settled);
    CHECK(state.game.beads.digit_count >= 100); /* Larger separated Inter sites. */
    CHECK(state.game.beads.count == ORB_GAME_BEAD_COUNT);
    /* Distinct sites prevent collisions/jitter from clumping the time. */
    for (unsigned i = 0; i < state.game.beads.count; ++i) {
        for (unsigned j = 0; j < i; ++j) {
            int dx = state.game.beads.target_x[i] - state.game.beads.target_x[j];
            int dy = state.game.beads.target_y[i] - state.game.beads.target_y[j];
            CHECK(dx * dx + dy * dy >= 15 * 15);
        }
    }
    /* The clock must lock within the first second after stillness. */
    for (unsigned i = 0; i < 59; ++i)
        orb_game_step(&state, 0, 0, true, false);
    for (unsigned i = 0; i < state.game.beads.count; ++i) {
        const orb_game_bead_t *b = &state.game.beads.beads[i];
        CHECK(b->x == state.game.beads.target_x[i]);
        CHECK(b->y == state.game.beads.target_y[i]);
        CHECK(b->vx == 0 && b->vy == 0);
    }
    orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
    CHECK(orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES) == state.game.beads.count + ORB_GAME_PARTICLES + 1u);
    CHECK(sprites[0].opacity > 0);
    CHECK(sprites[0].radius == ORB_GAME_BEAD_DRAW_RADIUS);
    CHECK(sprites[0].shape == ORB_GAME_BEAD);
    CHECK(ORB_GAME_BEAD_DRAW_RADIUS < ORB_GAME_BEAD_RADIUS);
    CHECK(sprites[0].colour != sprites[1].colour);
    CHECK(sprites[state.game.beads.digit_count + 4u].opacity <= 115);
    bool bright_stroke = false, clipped_edge = false;
    for (unsigned i = 0; i < state.game.beads.digit_count; ++i) {
        bright_stroke |= sprites[i].opacity == 255;
        clipped_edge |= sprites[i].opacity < 255;
    }
    CHECK(bright_stroke && clipped_edge);
    /* A held clock updates on a minute change and locks again. */
    int16_t old_targets[ORB_GAME_BEAD_COUNT];
    memcpy(old_targets, state.game.beads.target_x, sizeof(old_targets));
    orb_game_beads_set_time(&state, 23, 58);
    CHECK(state.game.beads.settled && state.game.beads.travel_ticks == 0);
    CHECK(memcmp(old_targets, state.game.beads.target_x, sizeof(old_targets)));
    for (unsigned i = 0; i < 60; ++i) orb_game_step(&state, 0, 0, true, false);
    for (unsigned i = 0; i < state.game.beads.count; ++i) {
        CHECK(state.game.beads.beads[i].x == state.game.beads.target_x[i]);
        CHECK(state.game.beads.beads[i].y == state.game.beads.target_y[i]);
    }
    /* No valid time exhausts the fixed target/sprite storage. */
    for (unsigned hour = 0; hour < 24; ++hour) {
        for (unsigned minute = 0; minute < 60; ++minute) {
            orb_game_beads_set_time(&state, (uint8_t)hour, (uint8_t)minute);
            CHECK(state.game.beads.count == ORB_GAME_BEAD_COUNT);
            CHECK(state.game.beads.count > state.game.beads.digit_count + 100);
        }
    }
    /* Actual movement, independent of absolute tilt, releases the clock. */
    orb_game_tilt_t moving = {.ready = true, .moving = true};
    orb_game_set_motion(&state, &moving);
    orb_game_step(&state, 0.5f, 0, true, false);
    CHECK(!state.game.beads.settled);
    /* Beads never produce a round end or score. */
    CHECK(!state.game_over && state.score == 0);
    orb_game_state_t a, b;
    orb_game_init(&a, ORB_GAME_BEADS, 31);
    orb_game_init(&b, ORB_GAME_BEADS, 31);
    for (unsigned i = 0; i < 60; ++i) {
        orb_game_step(&a, 0.4f, 0.1f, true, false);
        orb_game_step(&b, 0.4f, 0.1f, true, true);
        orb_game_state_t essential = a;
        essential.reduce_motion = b.reduce_motion;
        essential.light_x = b.light_x;
        essential.light_y = b.light_y;
        CHECK(!memcmp(&essential, &b, sizeof(a))); /* Reduce Motion never alters the fluid. */
    }
}

static void pop(void)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_POP, 12);
    step_n(&state, 1);
    CHECK(state.game.pop.bubbles[0].active);
    orb_game_bubble_t first = state.game.pop.bubbles[0];
    orb_game_action(&state, ORB_GAME_TAP, ORB_GAME_CX + first.x, ORB_GAME_CY + first.y);
    CHECK(!state.game.pop.bubbles[0].active && state.combo == 1 && state.score == 10);
    memset(state.game.pop.bubbles, 0, sizeof(state.game.pop.bubbles));
    state.game.pop.bubbles[0] = (orb_game_bubble_t){30, 0, 20, 0, 24, true};
    state.game.pop.bubbles[1] = (orb_game_bubble_t){80, 0, 20, 0, 24, true};
    orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
    CHECK(!state.game.pop.bubbles[0].active && state.game.pop.bubbles[1].active);
    CHECK(state.combo == 2 && state.score == 30);
    orb_game_action(&state, ORB_GAME_TAP, NAN, NAN);
    CHECK(state.combo == 2);
    orb_game_action(&state, ORB_GAME_TAP, 10, 10);
    CHECK(state.combo == 2);
    state.game.pop.bubbles[0] = (orb_game_bubble_t){ORB_GAME_RADIUS - 24.1f, 0, 20, 0, 24, true};
    step_n(&state, 1);
    CHECK(state.lives == 2 && state.combo == 0);
    for (unsigned i = 0; i < 2; ++i) {
        state.game.pop.bubbles[0] = (orb_game_bubble_t){ORB_GAME_RADIUS - 24.1f, 0, 20, 0, 24, true};
        step_n(&state, 1);
    }
    CHECK(state.game_over && state.lives == 0);
    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    CHECK(!state.game_over && state.score == 0 && state.combo == 0);
}

static void beads_touch_play(void)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_BEADS, 31);
    orb_game_beads_set_time(&state, 10, 9);
    orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    CHECK(state.controls);
    orb_game_action(&state, ORB_GAME_STEER_START, 413, 233);
    for (unsigned i = 0; i < 180; ++i) orb_game_step(&state, 1, -1, false, false);
    float centroid = 0;
    for (unsigned i = 0; i < state.game.beads.count; ++i) centroid += state.game.beads.beads[i].x;
    CHECK(centroid / state.game.beads.count > 115);
    CHECK(!state.game.beads.settled && state.game.beads.settle_ticks == 0);
    orb_game_action(&state, ORB_GAME_STEER_START, 53, 233);
    for (unsigned i = 0; i < 180; ++i) orb_game_step(&state, -1, 1, false, false);
    centroid = 0;
    for (unsigned i = 0; i < state.game.beads.count; ++i) centroid += state.game.beads.beads[i].x;
    CHECK(centroid / state.game.beads.count < -115);
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    CHECK(state.user_paused && !state.game.beads.touch_held);
    orb_game_state_t paused = state;
    for (unsigned i = 0; i < 180; ++i) orb_game_step(&state, 1, 1, true, false);
    CHECK(!memcmp(&paused, &state, sizeof(state)));
    orb_game_action(&state, ORB_GAME_STEER_START, 233, 413);
    CHECK(!state.game.beads.touch_held);
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    for (unsigned i = 0; i < ORB_GAME_BEAD_SETTLE_STEPS + 45; ++i)
        orb_game_step(&state, 1, 1, false, false);
    CHECK(state.game.beads.settled && state.game.beads.travel_ticks == 45);
    orb_game_action(&state, ORB_GAME_STEER_START, 233, 413);
    orb_game_step(&state, 0, 0, false, false);
    CHECK(!state.game.beads.settled && state.game.beads.touch_held);
    orb_game_action(&state, ORB_GAME_STEER_STOP, 0, 0);
    orb_game_action(&state, ORB_GAME_STEER_START, NAN, INFINITY);
    CHECK(!state.game.beads.touch_held);
    orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    CHECK(!state.controls && !state.game.beads.settled && !state.game.beads.settle_ticks);
    CHECK(!state.game_over && !state.score); /* A silent toy has no artificial loss. */
}

static void beads_settle_stays_visible(void)
{
    /* The clock's bottom sites lie near the bezel. Starting from any side of
     * the bowl must never fling beads outside the panel while they form time. */
    for (unsigned direction = 0; direction < 8; ++direction) {
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_BEADS, 31);
        orb_game_beads_set_time(&state, 10, 9);
        float angle = (float)direction * 6.28318530718f / 8;
        for (unsigned i = 0; i < 240; ++i)
            orb_game_step(&state, cosf(angle) * 0.3f, sinf(angle) * 0.3f, true, false);
        for (unsigned i = 0; i < ORB_GAME_BEAD_SETTLE_STEPS + 45; ++i) {
            step_n(&state, 1);
            if (!state.game.beads.settled) continue;
            for (unsigned b = 0; b < state.game.beads.count; ++b) {
                const orb_game_bead_t *bead = &state.game.beads.beads[b];
                CHECK(hypotf(bead->x, bead->y) <= 233 - ORB_GAME_BEAD_RADIUS + 0.01f);
            }
        }
        CHECK(state.game.beads.settled && state.game.beads.travel_ticks == 45);
    }
}

static void pop_refill_and_finish(void)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_POP, 12);
    for (unsigned i = 0; i < 3; ++i) orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
    CHECK(state.score == 60 && state.combo == 3);
    step_n(&state, 17);
    for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) CHECK(!state.game.pop.bubbles[i].active);
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.game.pop.bubbles[0].active);
    CHECK(state.combo == 3 && state.game.pop.time_left == ORB_GAME_POP_ROUND_STEPS - 18);

    state.game.pop.time_left = 1;
    state.game.pop.spawn_in = 1;
    state.game.pop.bubbles[0] = (orb_game_bubble_t){ORB_GAME_RADIUS - 24.1f, 0, 20, 0, 24, true};
    uint32_t score = state.score;
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.game_over && !state.game.pop.time_left);
    CHECK(state.lives == 3 && state.combo == 3 && state.score == score);
    CHECK(state.game.pop.spawn_in == 1 && state.game.pop.bubbles[0].active);
    orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
    CHECK(state.score == score); /* A result-screen tap cannot score a late pop. */
}

static void finish_round(void)
{
    for (unsigned kind = 0; kind < ORB_GAME_COUNT; ++kind) {
        orb_game_state_t state;
        orb_game_init(&state, (orb_game_kind_t)kind, 44);
        state.started = false;
        state.score = 1234;
        orb_game_action(&state, ORB_GAME_FINISH, 0, 0);
        CHECK(!state.started && !state.game_over && state.score == 1234);
        orb_game_action(&state, ORB_GAME_START, 0, 0);
        orb_game_action(&state, ORB_GAME_FINISH, 0, 0);
        CHECK(!state.game_over && state.score == 1234);
        orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
        orb_game_action(&state, ORB_GAME_FINISH, 0, 0);
        CHECK(state.game_over && !state.user_paused && state.score == 1234);
        orb_game_state_t finished = state;
        orb_game_action(&state, ORB_GAME_FINISH, 0, 0);
        CHECK(!memcmp(&state, &finished, sizeof(state)));
        step_n(&state, 5);
        CHECK(state.score == 1234 && state.ticks == 0);
        orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
        CHECK(!state.game_over && state.started && state.score == 0);
    }
}

static void capsule_and_speed_bounds(void)
{
    orb_game_state_t state;
    /* Comet speed scales with score and stays bounded. */
    orb_game_init(&state, ORB_GAME_ORBIT, 4);
    state.score = UINT32_MAX;
    float before = state.game.orbit.obstacles[0].angle;
    orb_game_step(&state, 0, 0, false, false);
    float advance = state.game.orbit.obstacles[0].angle - before;
    CHECK(isfinite(advance) && fabsf(advance - 1.85f / 60) < 0.00001f);
    /* Planet+comet contact radius: within 22 px costs a life, farther is safe. */
    orb_game_init(&state, ORB_GAME_ORBIT, 4);
    state.game.orbit.ring = 0;
    state.game.orbit.angle = 0;
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
        state.game.orbit.obstacles[i] = (orb_game_obstacle_t){3.0f, 1, false};
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){0.10f, 0, false};
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.lives == 2);
    orb_game_init(&state, ORB_GAME_ORBIT, 4);
    state.game.orbit.ring = 0;
    state.game.orbit.angle = 0;
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
        state.game.orbit.obstacles[i] = (orb_game_obstacle_t){3.0f, 1, false};
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){0.60f, 0, false};
    orb_game_step(&state, 0, 0, false, false);
    CHECK(state.lives == 3);
    orb_game_init(&state, ORB_GAME_POP, 4);
    state.score = UINT32_MAX;
    memset(state.game.pop.bubbles, 0, sizeof(state.game.pop.bubbles));
    state.game.pop.spawn_in = 1;
    step_n(&state, 1);
    orb_game_bubble_t bubble = state.game.pop.bubbles[0];
    CHECK(isfinite(bubble.vx) && isfinite(bubble.vy));
    CHECK(fabsf(hypotf(bubble.vx, bubble.vy) - 31) < 0.00001f);
}

static void scenes_and_polish(void)
{
    CHECK(ORB_GAME_CX == 233 && ORB_GAME_CY == 233 && ORB_GAME_RADIUS == 215);
    orb_game_state_t state;
    orb_game_sprite_t a[ORB_GAME_MAX_SPRITES], b[ORB_GAME_MAX_SPRITES];
    orb_game_init(&state, ORB_GAME_MARBLE, 44);
    CHECK(state.started && !state.user_paused && !state.controls);
    state.started = false;
    step_n(&state, 30);
    CHECK(state.ticks == 0);
    orb_game_action(&state, ORB_GAME_LEFT, 0, 0);
    CHECK(state.game.marble.vx == 0);
    orb_game_action(&state, ORB_GAME_START, 0, 0);
    orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    CHECK(state.started && state.controls);
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    step_n(&state, 30);
    CHECK(state.user_paused && state.ticks == 0);
    orb_game_action(&state, ORB_GAME_LEFT, 0, 0);
    CHECK(state.game.marble.vx == 0);
    orb_game_action(&state, ORB_GAME_PAUSE, 0, 0);
    step_n(&state, 1);
    CHECK(state.ticks == 1 && !state.user_paused);
    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    CHECK(state.started && state.controls && state.ticks == 0);
    state.game.marble.x = state.game.marble.y = 0;
    step_n(&state, 1);
    CHECK(state.effect.count == 8 && state.effect.duration == 24);
    size_t marble_count = orb_game_sprites(&state, a, ORB_GAME_MAX_SPRITES);
    CHECK(marble_count == 53);
    unsigned particles = 0;
    for (size_t i = 0; i < marble_count; ++i)
        if (a[i].shape == ORB_GAME_PARTICLE && a[i].radius) ++particles;
    CHECK(particles == 8);
    CHECK(a[0].shape == ORB_GAME_MARBLE_SPRITE);
    orb_game_step(&state, 0.8f, -0.5f, true, false);
    orb_game_sprites(&state, b, ORB_GAME_MAX_SPRITES);
    CHECK(a[0].light_x != b[0].light_x && a[0].light_y != b[0].light_y);
    orb_game_step(&state, 1, 1, true, true);
    orb_game_sprites(&state, b, ORB_GAME_MAX_SPRITES);
    CHECK(b[0].light_x == -3 && b[0].light_y == -4);
    for (size_t i = marble_count - ORB_GAME_PARTICLES - 1; i < marble_count - 1; ++i) CHECK(b[i].radius == 0);
    CHECK(b[marble_count - 1].shape == ORB_GAME_RING && b[marble_count - 1].radius == 18);
    step_n(&state, 30);
    CHECK(state.effect.age == state.effect.duration);

    orb_game_init(&state, ORB_GAME_ORBIT, 1);
    orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
    orb_game_sprites(&state, a, ORB_GAME_MAX_SPRITES);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(a[i].radius && a[i].opacity);
        float relative_x = a[i].x - ORB_GAME_CX;
        float relative_y = a[i].y - ORB_GAME_CY;
        CHECK(fabsf(atan2f(relative_y, relative_x) - state.game.orbit.angle) < 0.00001f);
        float trail_radius = hypotf(relative_x, relative_y);
        CHECK(trail_radius > ORB_GAME_ORBIT_R0 && trail_radius < ORB_GAME_ORBIT_R1);
    }
    CHECK(a[4].colour == 0x6d7cff && a[4].radius == ORB_GAME_ORBIT_PLANET);
    CHECK(fabsf(hypotf(a[4].x - ORB_GAME_CX, a[4].y - ORB_GAME_CY) - ORB_GAME_ORBIT_R1) < 0.01f);
    CHECK(a[7].colour == 0xff9230 && a[7].radius == ORB_GAME_ORBIT_COMET);
    CHECK(a[19].shape == ORB_GAME_STAR && a[19].colour == 0xffd600);
    step_n(&state, 12);
    orb_game_sprites(&state, b, ORB_GAME_MAX_SPRITES);
    for (unsigned i = 0; i < 4; ++i) CHECK(!b[i].radius); /* Switch trail fades. */
    orb_game_state_t faster = state;
    faster.score = 2000;
    float before = state.game.orbit.obstacles[0].angle;
    float fast_before = faster.game.orbit.obstacles[0].angle;
    step_n(&state, 1); step_n(&faster, 1);
    CHECK(faster.game.orbit.obstacles[0].angle - fast_before >
          state.game.orbit.obstacles[0].angle - before);

    orb_game_init(&state, ORB_GAME_SNAKE, 3);
    orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    step_n(&state, 8);
    orb_game_sprites(&state, a, ORB_GAME_MAX_SPRITES);
    step_n(&state, 4);
    orb_game_sprites(&state, b, ORB_GAME_MAX_SPRITES);
    CHECK(a[0].shape == ORB_GAME_HEAD && b[0].y != a[0].y);
    CHECK(hypotf(b[0].x - a[0].x, b[0].y - a[0].y) < 12);
    CHECK(a[1].opacity == 255 && a[3].opacity == 255); /* Frame uses a flat orange body. */
    CHECK(a[0].radius == ORB_GAME_SNAKE_HEAD_RADIUS && a[1].radius == 9);
    state.game.snake.cells[0] = (orb_game_point_t){160, -5};
    orb_game_sprites(&state, b, ORB_GAME_MAX_SPRITES);
    CHECK(b[0].x > ORB_GAME_CX + 150 && b[0].y < ORB_GAME_CY);

    orb_game_init(&state, ORB_GAME_POP, 4);
    memset(state.game.pop.bubbles, 0, sizeof(state.game.pop.bubbles));
    state.game.pop.bubbles[0] = (orb_game_bubble_t){0, 20, 15, 0, 24, true};
    step_n(&state, 1);
    orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
    CHECK(state.game.pop.reward == 10 && state.score == 10);
    orb_game_sprites(&state, a, ORB_GAME_MAX_SPRITES);
    step_n(&state, 8);
    orb_game_sprites(&state, b, ORB_GAME_MAX_SPRITES);
    CHECK(a[18].shape == ORB_GAME_RING && !a[18].radius && !b[18].radius);
    CHECK(a[19].shape == ORB_GAME_SPARK && a[19].colour == 0xffffff && a[19].radius > 0);
    CHECK(a[19].x == b[19].x && a[19].y == b[19].y && a[19].radius == b[19].radius);
    CHECK(b[19].opacity < a[19].opacity);
    for (unsigned i = ORB_GAME_BUBBLES; i < ORB_GAME_BUBBLES + ORB_GAME_PARTICLES; ++i)
        CHECK(!a[i].radius && !b[i].radius); /* The rays replace travelling debris. */
    state.game_over = true;
    uint32_t frozen = state.ticks;
    step_n(&state, 30);
    CHECK(state.ticks == frozen && state.effect.age == state.effect.duration);
}

static void chrome_geometry_and_feedback(void)
{
    CHECK(!orb_game_position_clear(NAN, 0, 1, ORB_GAME_POP, false));
    CHECK(!orb_game_position_clear(0, 0, -1, ORB_GAME_POP, false));
    CHECK(!orb_game_position_clear(0, 0, 1, ORB_GAME_COUNT, false));
    CHECK(orb_game_position_clear(0, -100, 24, ORB_GAME_POP, false));
    CHECK(!orb_game_position_clear(0, -165, 24, ORB_GAME_POP, false));
    CHECK(orb_game_position_clear(0, 166, 24, ORB_GAME_POP, false));
    CHECK(orb_game_position_clear(0, 166, 9, ORB_GAME_MARBLE, false));
    CHECK(orb_game_position_clear(-132, -132, 9, ORB_GAME_MARBLE, false));
    CHECK(orb_game_position_clear(132, -132, 9, ORB_GAME_MARBLE, false));
    CHECK(orb_game_position_clear(-132, -132, 24, ORB_GAME_POP, false));
    CHECK(orb_game_position_clear(0, -74, 12, ORB_GAME_MARBLE, true));
    CHECK(orb_game_position_clear(-46, 60, 9, ORB_GAME_SNAKE, false));
    CHECK(orb_game_position_clear(-46, 60, 9, ORB_GAME_SNAKE, true));
    /* Full head clearance around every part of the circular field. */
    for (unsigned ring = 0; ring < 3; ++ring) {
        float radius = 50 + (float)ring * 75;
        for (unsigned i = 0; i < 360; ++i) {
            float angle = (float)i * 6.28318531f / 360;
            CHECK(orb_game_position_clear(cosf(angle) * radius, sinf(angle) * radius,
                12, ORB_GAME_SNAKE, true));
        }
    }
    CHECK(orb_game_position_clear(0, 0, 12, ORB_GAME_MARBLE, true));
    CHECK(orb_game_position_clear(190, 0, 24, ORB_GAME_POP, false));
    for (uint32_t seed = 1; seed <= 64; ++seed) {
        orb_game_state_t pop_state;
        orb_game_init(&pop_state, ORB_GAME_POP, seed);
        unsigned population = 0;
        for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
            const orb_game_bubble_t *bubble = &pop_state.game.pop.bubbles[i];
            if (!bubble->active) continue;
            ++population;
            CHECK(orb_game_position_clear(bubble->x, bubble->y, bubble->radius, ORB_GAME_POP, false));
            for (unsigned n = i + 1; n < ORB_GAME_BUBBLES; ++n) {
                const orb_game_bubble_t *other = &pop_state.game.pop.bubbles[n];
                if (other->active) CHECK(hypotf(bubble->x - other->x, bubble->y - other->y) >= 60);
            }
        }
        CHECK(population == 3);
        orb_game_state_t snake_state;
        orb_game_init(&snake_state, ORB_GAME_SNAKE, seed);
        orb_game_point_t food = snake_state.game.snake.food;
        CHECK(orb_game_position_clear(food.x, food.y, 9, ORB_GAME_SNAKE, snake_state.controls));
    }
    for (unsigned kind = ORB_GAME_MARBLE; kind < ORB_GAME_COUNT; ++kind) {
        orb_game_state_t state;
        orb_game_init(&state, (orb_game_kind_t)kind, 74);
        for (unsigned frame = 0; frame < 7200; ++frame) {
            if (state.game_over) orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
            if (kind == ORB_GAME_MARBLE && frame % 317 == 0)
                orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
            orb_game_step(&state, sinf((float)frame * 0.017f), cosf((float)frame * 0.023f), true, false);
            orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
            size_t count = orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
            for (size_t i = 0; i < count; ++i) {
                if (!sprites[i].radius) continue;
                CHECK(orb_game_position_clear(sprites[i].x - ORB_GAME_CX,
                    sprites[i].y - ORB_GAME_CY, sprites[i].radius, state.kind, state.controls));
            }
        }
    }
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_ORBIT, 5);
    state.game.orbit.angle = 0;
    state.game.orbit.ring = 0;
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
        state.game.orbit.obstacles[i] = (orb_game_obstacle_t){3.0f, 1, true};
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){0.0f, 0, false};
    orb_game_step(&state, 0, 0, false, true);
    CHECK(state.lives == 2 && state.effect.age == 0);
    CHECK(state.effect.colour == 0xff4245 && state.effect.count == 6);
    step_n(&state, 8);
    CHECK(state.lives == 2 && state.effect.age == 8); /* One event, not repeated every contact frame. */

    orb_game_init(&state, ORB_GAME_POP, 5);
    memset(state.game.pop.bubbles, 0, sizeof(state.game.pop.bubbles));
    state.game.pop.bubbles[0] = (orb_game_bubble_t){80, 20, 0, 0, 24, true};
    state.game.pop.bubbles[1] = (orb_game_bubble_t){145, 20, 0, 0, 24, true};
    orb_game_action(&state, ORB_GAME_TAP, ORB_GAME_CX + 80, ORB_GAME_CY + 20);
    CHECK(state.combo == 1 && state.game.pop.bubbles[1].active);
    for (unsigned frame = 0; frame < 24; ++frame) {
        orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
        size_t count = orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
        for (size_t i = ORB_GAME_BUBBLES; i < count; ++i) {
            if (!sprites[i].radius) continue;
            float distance = hypotf(sprites[i].x - ORB_GAME_CX - 145, sprites[i].y - ORB_GAME_CY - 20);
            CHECK(distance >= sprites[i].radius + 24 + 4);
        }
        step_n(&state, 1);
    }
}

static void reduced_motion_feedback(void)
{
    for (unsigned kind = 0; kind < ORB_GAME_COUNT; ++kind) {
        orb_game_state_t state;
        orb_game_init(&state, (orb_game_kind_t)kind, 6);
        state.game_over = true; /* Freeze essential play while response time advances. */
        state.reduce_motion = true;
        if (kind == ORB_GAME_POP)
            memset(state.game.pop.bubbles, 0, sizeof(state.game.pop.bubbles));
        state.effect = (orb_game_effect_t){0, kind == ORB_GAME_ORBIT ? 141 : 40,
            0xff4245, 0, 24, 6, kind == ORB_GAME_POP ? 24 : kind == ORB_GAME_ORBIT ? 18 : 0};
        orb_game_sprite_t sprites[ORB_GAME_MAX_SPRITES];
        size_t count = orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
        orb_game_sprite_t first = sprites[count - 1];
        CHECK(first.shape == (kind == ORB_GAME_POP ? ORB_GAME_SPARK : ORB_GAME_RING) && first.radius > 0);
        if (kind == ORB_GAME_POP) {
            CHECK(first.colour == 0xffffff);
            CHECK(sprites[count - 2].shape == ORB_GAME_RING && !sprites[count - 2].radius);
        }
        size_t particles_at = count - ORB_GAME_PARTICLES - (kind == ORB_GAME_POP ? 2 : 1);
        for (unsigned frame = 1; frame < 24; ++frame) {
            orb_game_step(&state, 0, 0, false, true);
            CHECK(orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES) == count);
            orb_game_sprite_t mark = sprites[count - 1];
            CHECK(mark.x == first.x && mark.y == first.y && mark.radius == first.radius);
            CHECK(mark.colour == first.colour && mark.opacity < first.opacity && mark.opacity > 0);
            for (size_t i = particles_at; i < particles_at + ORB_GAME_PARTICLES; ++i)
                CHECK(sprites[i].shape == ORB_GAME_PARTICLE && sprites[i].radius == 0);
        }
        orb_game_step(&state, 0, 0, false, true);
        orb_game_sprites(&state, sprites, ORB_GAME_MAX_SPRITES);
        CHECK(sprites[count - 1].radius == 0);
    }
    for (uint32_t seed = 1; seed <= 64; ++seed) {
        orb_game_state_t state;
        orb_game_init(&state, ORB_GAME_MARBLE, seed);
        orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
        orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
        CHECK(state.controls && state.ticks == 0 && state.started);
        CHECK(orb_game_position_clear(state.game.marble.x, state.game.marble.y, 12, state.kind, true));
    }
}

static void held_tilt_ten_seconds(void)
{
    /* Steering games share the same captured frame. Orbit deliberately uses
     * taps for lane changes; its sensor output must nevertheless remain honest
     * for lighting. Snake consumes one tilt gesture then stays latched. */
    const orb_game_kind_t kinds[] = {ORB_GAME_MARBLE, ORB_GAME_ORBIT, ORB_GAME_SNAKE};
    const float poses[][3] = {{0, 0, 1}, {0, 0.57357644f, 0.81915204f},
                             {-0.12f, 0.62f, 0.72f}};
    const uint32_t cadences[] = {5000, 10000, 20000};
    const float quiet_gyro[3] = {0, 0, 0};
    for (unsigned game = 0; game < sizeof(kinds) / sizeof(kinds[0]); ++game) {
        for (unsigned pose = 0; pose < sizeof(poses) / sizeof(poses[0]); ++pose) {
            for (unsigned cadence = 0; cadence < sizeof(cadences) / sizeof(cadences[0]); ++cadence) {
                orb_game_tilt_t tilt;
                orb_game_tilt_reset(&tilt);
                float ax = 0, ay = 0;
                uint32_t sample_us = cadences[cadence];
                for (unsigned i = 0; i < 300000u / sample_us; ++i)
                    (void)orb_game_tilt_update_motion(&tilt, poses[pose][0], poses[pose][1],
                        poses[pose][2], quiet_gyro, sample_us, &ax, &ay);
                CHECK(tilt.ready);
                float captured[3];
                memcpy(captured, tilt.neutral, sizeof(captured));
                float force[3];
                trace_relative_roll(&tilt, poses[pose], 0.34906585f, force);
                orb_game_state_t state;
                orb_game_init(&state, kinds[game], 77);
                if (state.kind == ORB_GAME_ORBIT) {
                    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
                        state.game.orbit.obstacles[i].ring = ORB_GAME_ORBIT_RINGS;
                } else if (state.kind == ORB_GAME_SNAKE) {
                    /* A sustained absolute direction should reach the rim,
                     * without silently relearning neutral in the hand. */
                }
                float minimum = 0, maximum = -1;
                uint32_t step_us = 0;
                for (unsigned i = 0; i < 10000000u / sample_us; ++i) {
                    CHECK(orb_game_tilt_update_motion(&tilt, force[0], force[1], force[2],
                        quiet_gyro, sample_us, &ax, &ay));
                    CHECK(!memcmp(captured, tilt.neutral, sizeof(captured)));
                    if (i * sample_us >= 500000u) {
                        minimum = fminf(minimum, ax);
                        maximum = fmaxf(maximum, ax);
                        CHECK(ax < -0.15f && fabsf(ay) < 0.00001f);
                    }
                    step_us += sample_us;
                    while (step_us >= ORB_GAME_STEP_US) {
                        orb_game_step(&state, ax, ay, true, false);
                        step_us -= ORB_GAME_STEP_US;
                    }
                }
                CHECK(maximum - minimum < 0.0001f);
                if (state.kind == ORB_GAME_MARBLE) {
                    /* A wall can stop motion, so verify the same final force
                     * immediately accelerates a marble in a clear lane. */
                    state.game.marble.x = 100; state.game.marble.y = 60;
                    state.game.marble.vx = state.game.marble.vy = 0;
                    state.game.marble.stage_cleared = false;
                    for (unsigned i = 0; i < ORB_MARBLE_GATES_MAX; ++i)
                        state.game.marble.gates[i].active = false;
                    orb_game_step(&state, ax, ay, true, false);
                    CHECK(state.game.marble.vx > 0);
                } else if (state.kind == ORB_GAME_ORBIT) {
                    CHECK(!state.game_over && state.game.orbit.ring == 0);
                    CHECK(state.game.orbit.angle > 0); /* Continuous essential orbit. */
                } else {
                    CHECK(state.game_over && !state.game.snake.won);
                    CHECK(fabsf(remainderf(state.game.snake.heading, 6.2831853f)) < .001f);
                }
                printf("held tilt trace: %s pose=%u cadence=%u us, 10 s force=[%.6f,%.6f] g\n",
                    orb_game_id(state.kind), pose, sample_us, minimum, maximum);
            }
        }
    }
}

int main(void)
{
    identity();
    physical_accelerometer_directions();
    neutral_tilt_calibration();
    held_tilt_ten_seconds();
    recorded_motion_traces();
    marble();
    keepsake_records();
    marble_short_stages();
    marble_solid_gates_and_history_ink();
    marble_full_round_geometry();
    orbit();
    orbit_comet_paths();
    orbit_recovery_and_warning();
    snake();
    beads();
    beads_touch_play();
    beads_settle_stays_visible();
    pop();
    pop_refill_and_finish();
    finish_round();
    capsule_and_speed_bounds();
    scenes_and_polish();
    chrome_geometry_and_feedback();
    reduced_motion_feedback();
    deterministic_and_bounds();
    printf("game logic: %u checks; state=%zu bytes; zero allocations\n", s_checks, sizeof(orb_game_state_t));
    return 0;
}
