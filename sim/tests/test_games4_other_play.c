#include "orb_game_logic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.1415926535897932385f
#define TAU (2 * PI)
static unsigned s_checks;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

static void step(orb_game_state_t *state, float ax, float ay)
{
    orb_game_step(state, ax, ay, true, false);
}

/* Pause in the middle of real play: neither elapsed steps nor taps, steering,
 * timers, spawn cadence or collision states may sneak through the pause. */
static void pause_resume(orb_game_state_t *state)
{
    orb_game_action(state, ORB_GAME_PAUSE, 0, 0);
    CHECK(state->user_paused);
    orb_game_state_t before = *state;
    for (unsigned i = 0; i < 120; ++i) {
        step(state, -0.6f, 0.4f);
        orb_game_action(state, ORB_GAME_PRIMARY, ORB_GAME_CX, ORB_GAME_CY);
        orb_game_action(state, ORB_GAME_TAP, ORB_GAME_CX, ORB_GAME_CY);
    }
    CHECK(memcmp(state, &before, sizeof(before)) == 0);
    orb_game_action(state, ORB_GAME_PAUSE, 0, 0);
    CHECK(!state->user_paused && !state->game_over);
}

static void finish_and_replay(orb_game_state_t *state)
{
    uint32_t score = state->score;
    orb_game_action(state, ORB_GAME_PAUSE, 0, 0);
    orb_game_action(state, ORB_GAME_FINISH, 0, 0);
    CHECK(state->game_over && !state->user_paused && state->score == score);
    for (unsigned i = 0; i < 120; ++i) {
        step(state, 0, 0);
        orb_game_action(state, ORB_GAME_PRIMARY, 0, 0);
    }
    CHECK(state->game_over && state->score == score);
    orb_game_action(state, ORB_GAME_RESTART, 0, 0);
    CHECK(state->started && !state->game_over && !state->user_paused && !state->score);
}

static void marble_play(unsigned touch)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_MARBLE, 22);
    if (touch) orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    unsigned total_steps = 0;
    for (unsigned stage = 0; stage < ORB_MARBLE_STAGES; ++stage) {
        unsigned steps = 0;
        for (; steps < 60 * 60 && !state.game.marble.stage_cleared; ++steps) {
            if (steps == 120) pause_resume(&state);
            if (steps == 240) {
                uint16_t history = state.game.marble.history_count;
                orb_game_action(&state, ORB_GAME_REWIND_START, 0, 0);
                for (unsigned i = 0; i < 12; ++i) step(&state, 0, 0);
                CHECK(state.game.marble.history_count < history);
                orb_game_action(&state, ORB_GAME_REWIND_STOP, 0, 0);
                CHECK(!state.game.marble.rewind_active);
                CHECK(state.game.marble.vx == 0 && state.game.marble.vy == 0);
            }
            float x = state.game.marble.x, y = state.game.marble.y;
            if (touch && steps % 8 == 0) {
                float radius = hypotf(x, y);
                float swirl = radius > 50 ? 0.5f : 0;
                float brake = radius > 50 ? 2 : 0;
                float dx = -x - y * swirl - state.game.marble.vx * brake;
                float dy = -y + x * swirl - state.game.marble.vy * brake;
                if (dx < -4) orb_game_action(&state, ORB_GAME_LEFT, 0, 0);
                if (dx > 4) orb_game_action(&state, ORB_GAME_RIGHT, 0, 0);
                if (dy < -4) orb_game_action(&state, ORB_GAME_UP, 0, 0);
                if (dy > 4) orb_game_action(&state, ORB_GAME_DOWN, 0, 0);
            }
            float ax = x * 0.02f, ay = -y * 0.02f;
            float scale = fmaxf(1, hypotf(ax, ay) / 0.85f);
            step(&state, ax / scale, ay / scale);
            CHECK(hypotf(state.game.marble.x, state.game.marble.y) <=
                  ORB_MARBLE_RADIUS - ORB_MARBLE_BALL_RADIUS + 0.01f);
            CHECK(hypotf(state.game.marble.vx, state.game.marble.vy) <= ORB_MARBLE_MAX_SPEED + 0.01f);
        }
        total_steps += steps;
        CHECK(state.game.marble.stage_cleared && state.score == (stage + 1) * 100);
        CHECK(state.game.marble.keepsake_count == stage + 1);
        CHECK(state.game.marble.stages_cleared == stage + 1);
        for (unsigned i = 0; i < 120; ++i) step(&state, 0, 0);
        CHECK(state.game.marble.stage_cleared && state.score == (stage + 1) * 100);
        orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
    }
    CHECK(state.game_over && state.score == 300);
    orb_game_action(&state, ORB_GAME_GARDEN, 0, 0);
    CHECK(state.game.marble.garden_open && state.game.marble.keepsake_count == 3);
    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    CHECK(!state.game_over && !state.game.marble.garden_open && !state.score);
    CHECK(state.game.marble.keepsake_count == 3);
    /* Conservatory has no lethal loss. Explicitly abandoning an unfinished
     * puzzle is its losing/exit run, and retains earned keepsakes. */
    for (unsigned i = 0; i < 180; ++i) step(&state, -0.4f, 0.3f);
    pause_resume(&state);
    finish_and_replay(&state);
    CHECK(state.game.marble.keepsake_count == 3);
    printf("Marble %s: full three-stage win in %u steps, rewind, pause, abandon/replay\n",
           touch ? "touch" : "tilt", total_steps);
}

static float angle_delta(float angle)
{
    while (angle > PI) angle -= TAU;
    while (angle < -PI) angle += TAU;
    return angle;
}

static float lane_safety(const orb_game_state_t *state, unsigned lane)
{
    float safety = 10;
    float comet_speed = 1.05f + fminf((float)state->score / 1600, 0.8f);
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i) {
        const orb_game_obstacle_t *o = &state->game.orbit.obstacles[i];
        if (o->ring != lane || o->passed) continue;
        float relative = angle_delta(o->angle - state->game.orbit.angle);
        /* Behind is approaching. Ahead recedes until the safe far-side lane
         * recycle; sample the next 0.55 s at the production ramp speed. */
        for (unsigned sample = 0; sample < 7; ++sample) {
            float separation = fabsf(angle_delta(relative + (comet_speed - .35f) * (float)sample / 12));
            if (separation < safety) safety = separation;
        }
    }
    return safety;
}

static void orbit_play(unsigned touch)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_ORBIT, 112);
    if (touch) orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    for (unsigned tick = 0; tick < 60 * 90; ++tick) {
        if (tick == 600) pause_resume(&state);
        /* One player tap at most each 100 ms. Score only by reaching stars;
         * obstacles, scores and positions are never patched by this script. */
        if (tick % 6 == 0) {
            unsigned lane = state.game.orbit.ring;
            unsigned next = (lane + 1) % ORB_GAME_ORBIT_RINGS;
            float here = lane_safety(&state, lane), there = lane_safety(&state, next);
            float nearest_star = 10;
            unsigned star_lane = lane;
            for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i) {
                const orb_game_star_t *star = &state.game.orbit.stars[i];
                float ahead = angle_delta(star->angle - state.game.orbit.angle);
                if (star->active && ahead > 0 && ahead < nearest_star && lane_safety(&state, star->ring) > .5f) {
                    nearest_star = ahead;
                    star_lane = star->ring;
                }
            }
            if ((here < .5f && there > here) || (here >= .5f && star_lane != lane && there > .5f))
                orb_game_action(&state, touch ? ORB_GAME_TAP : ORB_GAME_PRIMARY, ORB_GAME_CX, ORB_GAME_CY);
        }
        step(&state, .5f * sinf((float)tick / 50), .3f);
        CHECK(!state.game_over && state.lives == 3);
    }
    CHECK(state.score >= 100 && state.score % 10 == 0);
    uint32_t earned = state.score;
    finish_and_replay(&state);
    /* No input: natural collisions, with no fixture teleports or forced lives. */
    unsigned losing_steps = 0;
    for (; losing_steps < 60 * 180 && !state.game_over; ++losing_steps) step(&state, 0, 0);
    CHECK(state.game_over && !state.lives && losing_steps > ORB_GAME_ORBIT_HIT_GRACE * 2);
    uint32_t lost_score = state.score;
    for (unsigned i = 0; i < 120; ++i) {
        step(&state, 0, 0);
        orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
    }
    CHECK(state.score == lost_score);
    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    CHECK(state.lives == 3 && !state.game_over && !state.score);
    printf("Orbit %s: 90 s survival earned %u, natural loss in %u steps, pause/replay\n",
           touch ? "touch" : "primary", earned, losing_steps);
}

static void pop_play(unsigned nearest)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_POP, 39);
    if (nearest) orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    unsigned pops = 0;
    uint32_t expected_score = 0;
    for (unsigned tick = 0; tick < ORB_GAME_POP_ROUND_STEPS; ++tick) {
        if (tick == 1200) pause_resume(&state);
        if (tick % 12 == 0) {
            int selected = -1;
            for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i)
                if (state.game.pop.bubbles[i].active) { selected = (int)i; break; }
            if (selected >= 0) {
                const orb_game_bubble_t *bubble = &state.game.pop.bubbles[selected];
                orb_game_action(&state, nearest ? ORB_GAME_PRIMARY : ORB_GAME_TAP,
                    ORB_GAME_CX + bubble->x, ORB_GAME_CY + bubble->y);
                ++pops;
                expected_score += 10 * (pops < 99 ? pops : 99);
                CHECK(state.score == expected_score && state.combo == (pops < 99 ? pops : 99));
            }
        }
        step(&state, .4f, -.4f);
        CHECK(state.game.pop.time_left == ORB_GAME_POP_ROUND_STEPS - tick - 1);
        CHECK(state.lives == 3);
        CHECK(state.game_over == (tick + 1 == ORB_GAME_POP_ROUND_STEPS));
    }
    CHECK(state.score == expected_score && state.score > 0 && state.lives == 3);
    uint32_t earned = state.score;
    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    CHECK(!state.game_over && !state.score && !state.combo && state.lives == 3);
    unsigned losing_steps = 0;
    for (; losing_steps < ORB_GAME_POP_ROUND_STEPS && !state.game_over; ++losing_steps) step(&state, 0, 0);
    CHECK(state.game_over && !state.lives && state.game.pop.time_left && !state.score && !state.combo);
    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    for (unsigned i = 0; i < 100; ++i) step(&state, 0, 0);
    finish_and_replay(&state);
    printf("Pop %s: full 45 s win, %u pops/%u points, natural loss in %u steps, pause/replay\n",
           nearest ? "nearest" : "direct touch", pops, earned, losing_steps);
}

static void orbit_collision_boundaries(void)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_ORBIT, 4);
    state.game.orbit.angle = 0;
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i)
        state.game.orbit.obstacles[i].ring = ORB_GAME_ORBIT_RINGS;
    for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i) state.game.orbit.stars[i].active = false;
    /* Visible outer star tip touches the planet: 25 px apart, beyond the old
     * invisible 22 px pickup radius, inside the artwork's combined 28 px. */
    state.game.orbit.stars[0] = (orb_game_star_t){.angle = .34f, .ring = 0, .active = true};
    step(&state, 0, 0);
    CHECK(state.score == 10 && !state.game.orbit.stars[0].active);

    orb_game_init(&state, ORB_GAME_ORBIT, 4);
    state.lives = 1;
    state.game.orbit.angle = 0;
    state.game.orbit.obstacles[0] = (orb_game_obstacle_t){0, 0, false};
    state.game.orbit.stars[0] = (orb_game_star_t){0, 0, true};
    step(&state, 0, 0);
    CHECK(state.game_over && !state.lives && !state.score);
    CHECK(state.effect.colour == 0xff4245u);
    CHECK(state.game.orbit.stars[0].active);
}

int main(void)
{
    for (unsigned alternative = 0; alternative < 2; ++alternative) {
        marble_play(alternative);
        orbit_play(alternative);
        pop_play(alternative);
    }
    orbit_collision_boundaries();
    printf("Games4 Marble/Orbit/Pop: %u checks passed\n", s_checks);
    return 0;
}
