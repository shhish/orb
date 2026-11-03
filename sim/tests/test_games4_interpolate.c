#include "orb_game_interpolate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.2831853071795864769f
static unsigned s_checks;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

static void near(float actual, float expected)
{
    CHECK(isfinite(actual) && fabsf(actual - expected) < .0001f);
}

static void angle_near(float actual, float expected)
{
    CHECK(isfinite(actual) && fabsf(remainderf(actual - expected, TAU)) < .0001f);
}

static float blend(float a, float b, float t) { return a + (b - a) * t; }

static void basic_games(void)
{
    const float alphas[] = {-1, 0, .25f, .5f, 1, 2};
    for (unsigned kind = 0; kind < ORB_GAME_COUNT; ++kind) {
        orb_game_state_t previous, current;
        orb_game_init(&previous, (orb_game_kind_t)kind, 41);
        current = previous;
        orb_game_step(&current, -.3f, .2f, true, false);
        CHECK(current.ticks == previous.ticks + 1);
        orb_game_state_t source = previous;
        for (unsigned sample = 0; sample < sizeof(alphas) / sizeof(alphas[0]); ++sample) {
            float t = fminf(1, fmaxf(0, alphas[sample]));
            orb_game_state_t result = current;
            orb_game_pose_t pose;
            orb_game_pose_capture(&previous, &pose);
            orb_game_state_t compact = current;
            orb_game_pose_interpolate(&compact, &pose, alphas[sample]);
            orb_game_interpolate(&previous, &result, alphas[sample]);
            CHECK(memcmp(&result, &compact, sizeof(result)) == 0);
            CHECK(memcmp(&previous, &source, sizeof(source)) == 0);
            CHECK(result.kind == current.kind && result.score == current.score &&
                  result.combo == current.combo && result.ticks == current.ticks &&
                  result.random == current.random && result.lives == current.lives);
            CHECK(result.started == current.started && result.game_over == current.game_over &&
                  result.user_paused == current.user_paused && result.controls == current.controls);
            CHECK(memcmp(&result.effect, &current.effect, sizeof(current.effect)) == 0);
            switch ((orb_game_kind_t)kind) {
            case ORB_GAME_MARBLE:
                near(result.game.marble.x, blend(previous.game.marble.x, current.game.marble.x, t));
                near(result.game.marble.y, blend(previous.game.marble.y, current.game.marble.y, t));
                near(result.game.marble.vx, current.game.marble.vx);
                near(result.game.marble.vy, current.game.marble.vy);
                CHECK(result.game.marble.history_head == current.game.marble.history_head);
                for (unsigned i = 0; i < ORB_MARBLE_GATES_MAX; ++i)
                    angle_near(result.game.marble.gates[i].angle,
                        previous.game.marble.gates[i].angle + remainderf(current.game.marble.gates[i].angle -
                            previous.game.marble.gates[i].angle, TAU) * t);
                break;
            case ORB_GAME_BEADS:
                for (unsigned i = 0; i < current.game.beads.count; ++i) {
                    near(result.game.beads.beads[i].x,
                         blend(previous.game.beads.beads[i].x, current.game.beads.beads[i].x, t));
                    near(result.game.beads.beads[i].y,
                         blend(previous.game.beads.beads[i].y, current.game.beads.beads[i].y, t));
                    near(result.game.beads.beads[i].vx, current.game.beads.beads[i].vx);
                    near(result.game.beads.beads[i].vy, current.game.beads.beads[i].vy);
                }
                break;
            case ORB_GAME_SNAKE:
                for (unsigned i = 0; i < current.game.snake.length; ++i) {
                    near(result.game.snake.cells[i].x,
                         blend(previous.game.snake.cells[i].x, current.game.snake.cells[i].x, t));
                    near(result.game.snake.cells[i].y,
                         blend(previous.game.snake.cells[i].y, current.game.snake.cells[i].y, t));
                }
                CHECK(result.game.snake.grace == current.game.snake.grace);
                near(result.game.snake.heading, current.game.snake.heading);
                break;
            case ORB_GAME_ORBIT:
                angle_near(result.game.orbit.angle, previous.game.orbit.angle +
                    remainderf(current.game.orbit.angle - previous.game.orbit.angle, TAU) * t);
                CHECK(result.game.orbit.hit_grace == current.game.orbit.hit_grace);
                break;
            case ORB_GAME_POP:
                for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
                    if (!current.game.pop.bubbles[i].active) continue;
                    near(result.game.pop.bubbles[i].x,
                         blend(previous.game.pop.bubbles[i].x, current.game.pop.bubbles[i].x, t));
                    near(result.game.pop.bubbles[i].y,
                         blend(previous.game.pop.bubbles[i].y, current.game.pop.bubbles[i].y, t));
                }
                CHECK(result.game.pop.time_left == current.game.pop.time_left);
                break;
            default: CHECK(false); break;
            }
        }
    }
}

static void untouched(const orb_game_state_t *previous, const orb_game_state_t *current, float alpha)
{
    orb_game_state_t result = *current;
    orb_game_pose_t pose;
    orb_game_pose_capture(previous, &pose);
    orb_game_state_t compact = *current;
    orb_game_pose_interpolate(&compact, &pose, alpha);
    orb_game_interpolate(previous, &result, alpha);
    CHECK(memcmp(&result, &compact, sizeof(result)) == 0);
    CHECK(memcmp(&result, current, sizeof(result)) == 0);
}

static void discontinuities(void)
{
    for (unsigned kind = 0; kind < ORB_GAME_COUNT; ++kind) {
        orb_game_state_t previous, current;
        orb_game_init(&previous, (orb_game_kind_t)kind, 13);
        current = previous;
        orb_game_step(&current, -.3f, .2f, true, false);
        untouched(&previous, &current, NAN);
        untouched(&previous, &current, INFINITY);
        orb_game_state_t changed = current;
        changed.user_paused = true;
        untouched(&previous, &changed, .5f);
        changed = current; changed.game_over = true;
        untouched(&previous, &changed, .5f);
        changed = current; changed.started = false;
        untouched(&previous, &changed, .5f);
        changed = current; changed.controls = !previous.controls;
        untouched(&previous, &changed, .5f);
        changed = current; changed.ticks += 3;
        untouched(&previous, &changed, .5f);
        changed = previous;
        untouched(&previous, &changed, .5f);
        changed = current; changed.kind = (orb_game_kind_t)((kind + 1) % ORB_GAME_COUNT);
        untouched(&previous, &changed, .5f);
    }
    orb_game_state_t previous, current;
    orb_game_init(&previous, ORB_GAME_MARBLE, 13);
    current = previous; ++current.ticks;
    current.game.marble.stage++;
    untouched(&previous, &current, .5f);
    current = previous; ++current.ticks;
    current.game.marble.stage_cleared = true;
    untouched(&previous, &current, .5f);
    current = previous; ++current.ticks;
    current.game.marble.garden_open = true;
    untouched(&previous, &current, .5f);
    current = previous; ++current.ticks;
    current.game.marble.rewind_active = true;
    untouched(&previous, &current, .5f);

    orb_game_init(&previous, ORB_GAME_BEADS, 13);
    current = previous; ++current.ticks;
    current.game.beads.count--;
    untouched(&previous, &current, .5f);
    current = previous; ++current.ticks;
    current.game.beads.settled = true;
    untouched(&previous, &current, .5f);
    previous.game.beads.settled = true;
    current = previous; ++current.ticks;
    current.game.beads.travel_ticks = 45;
    current.game.beads.beads[0].x += 5;
    untouched(&previous, &current, .5f); /* Clock's exact held sites never shimmer. */

    orb_game_init(&previous, ORB_GAME_SNAKE, 13);
    current = previous; ++current.ticks; ++current.game.snake.length;
    untouched(&previous, &current, .5f);
    orb_game_init(&previous, ORB_GAME_ORBIT, 13);
    current = previous; ++current.ticks; ++current.game.orbit.ring;
    untouched(&previous, &current, .5f);
    current = previous; ++current.ticks; --current.lives;
    untouched(&previous, &current, .5f);
}

static void wraps_and_slots(void)
{
    orb_game_state_t previous, current;
    orb_game_init(&previous, ORB_GAME_ORBIT, 7);
    previous.game.orbit.angle = TAU - .01f;
    previous.game.orbit.obstacles[0].angle = TAU - .02f;
    previous.game.orbit.stars[0] = (orb_game_star_t){.01f, 2, true};
    current = previous; ++current.ticks;
    current.game.orbit.angle = .03f;
    current.game.orbit.obstacles[0].angle = .02f;
    current.game.orbit.stars[0].angle = TAU - .01f;
    orb_game_interpolate(&previous, &current, .5f);
    angle_near(current.game.orbit.angle, .01f);
    angle_near(current.game.orbit.obstacles[0].angle, 0);
    angle_near(current.game.orbit.stars[0].angle, 0);

    current = previous; ++current.ticks;
    current.game.orbit.obstacles[0].ring = 2;
    current.game.orbit.obstacles[0].angle = 2;
    current.game.orbit.stars[0].active = false;
    current.game.orbit.stars[0].angle = 2;
    orb_game_interpolate(&previous, &current, .5f);
    near(current.game.orbit.obstacles[0].angle, 2);
    near(current.game.orbit.stars[0].angle, 2);

    orb_game_init(&previous, ORB_GAME_POP, 7);
    current = previous; ++current.ticks;
    current.game.pop.bubbles[0].active = false;
    current.game.pop.bubbles[0].x += 50;
    current.game.pop.bubbles[3] = (orb_game_bubble_t){50, 50, 15, 0, 24, true};
    orb_game_interpolate(&previous, &current, .5f);
    near(current.game.pop.bubbles[0].x, previous.game.pop.bubbles[0].x + 50);
    near(current.game.pop.bubbles[3].x, 50); /* A new bubble never flies in from a stale slot. */
    orb_game_interpolate(NULL, &current, .5f);
    orb_game_interpolate(&previous, NULL, .5f);
}

int main(void)
{
    basic_games();
    discontinuities();
    wraps_and_slots();
    printf("Games4 interpolation: %u checks passed\n", s_checks);
    return 0;
}
