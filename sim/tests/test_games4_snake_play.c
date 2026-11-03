/* Player scripts use only public input actions and fixed steps. The winning
 * pilot reads the visible arena, explores headings on temporary copies, then
 * supplies real tilt/finger inputs. It never teleports food, edits scores or
 * shortens the body in the played round. */
#include "orb_game_logic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.1415926535897932385f
static unsigned s_checks;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

static void steer_step(orb_game_state_t *state, float angle, bool touch)
{
    if (touch) {
        /* A real finger stays within the round glass. Intersect the desired
         * ray with r220, rather than using an impossible off-panel target. */
        orb_game_point_t p = state->game.snake.cells[0];
        float dx = cosf(angle), dy = sinf(angle);
        float dot = p.x * dx + p.y * dy;
        float distance = -dot + sqrtf(dot * dot + 220 * 220 - p.x * p.x - p.y * p.y);
        orb_game_action(state, ORB_GAME_STEER_START,
            ORB_GAME_CX + p.x + dx * distance, ORB_GAME_CY + p.y + dy * distance);
    }
    orb_game_step(state, -cosf(angle) * .5f, sinf(angle) * .5f, true, false);
}

static float choose_heading(const orb_game_state_t *state, bool touch)
{
    orb_game_point_t head = state->game.snake.cells[0], food = state->game.snake.food;
    float goal = atan2f(food.y - head.y, food.x - head.x);
    float best = -1e30f, angle = goal;
    /* A short, bounded look-ahead keeps the script honest about turn radius,
     * body collisions and the rim, just as a player anticipates a curve. */
    for (unsigned k = 0; k < 33; ++k) {
        float candidate = goal + (float)((k + 1) / 2) * (k % 2 ? 1.0f : -1.0f) * PI / 16;
        orb_game_state_t preview = *state;
        for (unsigned j = 0; j < 45; ++j) steer_step(&preview, candidate, touch);
        if (preview.game_over && !preview.game.snake.won) continue;
        orb_game_point_t p = preview.game.snake.cells[0];
        float distance = hypotf(p.x - food.x, p.y - food.y);
        float rim = hypotf(p.x, p.y);
        /* Like a player, keep room from the body (beyond the attached neck)
         * at the end of the look-ahead. Without it the pilot only survived one
         * particular pellet order; any change to spawns (the Snake score disc
         * now excludes pellets from under it) cornered a 40-segment body. */
        float room = 60;
        for (unsigned b = 4; b < preview.game.snake.length; ++b)
            room = fminf(room, hypotf(preview.game.snake.cells[b].x - p.x, preview.game.snake.cells[b].y - p.y));
        float value = (float)(preview.score - state->score) * 100 - distance - fmaxf(0, rim - 160) * 2 + room * .5f;
        if (value > best) { best = value; angle = candidate; }
    }
    return angle;
}

static void pause_resume(orb_game_state_t *state)
{
    orb_game_action(state, ORB_GAME_PAUSE, 0, 0);
    CHECK(state->user_paused && !state->game.snake.touch_held);
    orb_game_state_t paused = *state;
    for (unsigned i = 0; i < 180; ++i) steer_step(state, 0, true);
    CHECK(!memcmp(state, &paused, sizeof(paused)));
    orb_game_action(state, ORB_GAME_STEER_STOP, 0, 0);
    orb_game_action(state, ORB_GAME_PAUSE, 0, 0);
    CHECK(!state->user_paused && !state->game.snake.touch_held);
}

static void run_to_score(orb_game_state_t *state, bool touch, unsigned target)
{
    bool paused = false;
    for (unsigned ticks = 0; ticks < 12000 && !state->game_over && state->score < target;) {
        float angle = choose_heading(state, touch);
        for (unsigned i = 0; i < 6 && !state->game_over; ++i, ++ticks) {
            unsigned before = state->score;
            steer_step(state, angle, touch);
            CHECK(state->score == before || state->score == before + 10);
            CHECK(state->game.snake.length == 6 + state->score / 10);
            CHECK(state->game.snake.length <= ORB_GAME_SNAKE_MAX);
            /* The new score HUD must never hide the next objective. Check
             * every naturally spawned pellet in both complete input runs. */
            orb_game_point_t food = state->game.snake.food;
            CHECK(hypotf(food.x - ORB_GAME_HUD_X, food.y - ORB_GAME_HUD_Y) >=
                ORB_GAME_HUD_RADIUS + 8);
            for (unsigned j = 0; j < state->game.snake.length; ++j) {
                orb_game_point_t p = state->game.snake.cells[j];
                CHECK(isfinite(p.x) && isfinite(p.y));
            }
        }
        if (!paused && state->score >= 50 && !state->game_over) {
            pause_resume(state);
            paused = true;
        }
    }
    CHECK(paused && state->score >= target);
}

static void winning_run(bool touch)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_SNAKE, 3);
    if (touch) orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    run_to_score(&state, touch, 420);
    CHECK(state.game_over && state.game.snake.won && state.score == 420);
    CHECK(state.game.snake.length == ORB_GAME_SNAKE_MAX);
    printf("Snake %s: natural food run complete, score=%u, steps=%u, pause/resume passed\n",
        touch ? "touch" : "tilt", state.score, state.ticks);
    uint32_t ticks = state.ticks;
    for (unsigned i = 0; i < 120; ++i) orb_game_step(&state, .5f, .5f, true, false);
    CHECK(state.ticks == ticks && state.game_over);
    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    CHECK(!state.game_over && state.score == 0 && state.game.snake.grace == 60);
    CHECK(state.controls == touch && !state.game.snake.touch_held);
}

static void losing_runs(bool touch)
{
    orb_game_state_t state;
    orb_game_init(&state, ORB_GAME_SNAKE, 3);
    if (touch) orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    for (unsigned i = 0; i < 300 && !state.game_over; ++i) steer_step(&state, -PI / 2, touch);
    CHECK(state.game_over && !state.game.snake.won);
    CHECK(hypotf(state.game.snake.cells[0].x, state.game.snake.cells[0].y) > 200);
    CHECK(state.score == 10); /* The fair first pellet lies on the initial path. */

    orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
    /* A fresh deterministic round with natural food builds enough tail for
     * the tightest legal turn to demonstrate an unambiguous self-collision. */
    orb_game_init(&state, ORB_GAME_SNAKE, 3);
    if (touch) orb_game_action(&state, ORB_GAME_CONTROLS, 0, 0);
    run_to_score(&state, touch, 80);
    for (unsigned i = 0; i < 600 && !state.game_over; ++i)
        steer_step(&state, state.game.snake.heading + .5f, touch);
    CHECK(state.game_over && !state.game.snake.won);
    CHECK(hypotf(state.game.snake.cells[0].x, state.game.snake.cells[0].y) < 180);
    CHECK(state.score == 80);
    printf("Snake %s: natural rim loss and self-collision passed\n", touch ? "touch" : "tilt");
}

int main(void)
{
    winning_run(false);
    winning_run(true);
    losing_runs(false);
    losing_runs(true);
    printf("games4 Snake player scripts: %u checks\n", s_checks);
    return 0;
}
