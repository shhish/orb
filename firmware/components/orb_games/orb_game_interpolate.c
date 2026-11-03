#include "orb_game_interpolate.h"
#include <math.h>

static float blend(float a, float b, float t) { return a + (b - a) * t; }
static float angle(float a, float b, float t)
{
    float delta = remainderf(b - a, 6.28318530718f);
    return a + delta * t;
}

void orb_game_interpolate(const orb_game_state_t *previous, orb_game_state_t *current,
                          float alpha)
{
    if (!previous || !current || !isfinite(alpha) || previous->kind != current->kind ||
        previous->ticks + 1 != current->ticks || current->user_paused || current->game_over ||
        !current->started || previous->controls != current->controls) return;
    alpha = fmaxf(0, fminf(alpha, 1));
    switch (current->kind) {
    case ORB_GAME_MARBLE:
        if (previous->game.marble.stage != current->game.marble.stage ||
            current->game.marble.stage_cleared || current->game.marble.garden_open ||
            previous->game.marble.rewind_active != current->game.marble.rewind_active) break;
        current->game.marble.x = blend(previous->game.marble.x, current->game.marble.x, alpha);
        current->game.marble.y = blend(previous->game.marble.y, current->game.marble.y, alpha);
        for (unsigned i = 0; i < ORB_MARBLE_GATES_MAX; ++i)
            current->game.marble.gates[i].angle = angle(previous->game.marble.gates[i].angle,
                                                       current->game.marble.gates[i].angle, alpha);
        break;
    case ORB_GAME_BEADS:
        if (previous->game.beads.count != current->game.beads.count ||
            previous->game.beads.settled != current->game.beads.settled ||
            (current->game.beads.settled && current->game.beads.travel_ticks == 45)) break;
        for (unsigned i = 0; i < current->game.beads.count; ++i) {
            current->game.beads.beads[i].x = blend(previous->game.beads.beads[i].x,
                                                  current->game.beads.beads[i].x, alpha);
            current->game.beads.beads[i].y = blend(previous->game.beads.beads[i].y,
                                                  current->game.beads.beads[i].y, alpha);
        }
        break;
    case ORB_GAME_SNAKE:
        if (previous->game.snake.length != current->game.snake.length) break;
        for (unsigned i = 0; i < current->game.snake.length; ++i) {
            current->game.snake.cells[i].x = blend(previous->game.snake.cells[i].x,
                                                  current->game.snake.cells[i].x, alpha);
            current->game.snake.cells[i].y = blend(previous->game.snake.cells[i].y,
                                                  current->game.snake.cells[i].y, alpha);
        }
        break;
    case ORB_GAME_ORBIT:
        if (previous->game.orbit.ring != current->game.orbit.ring ||
            previous->lives != current->lives) break;
        current->game.orbit.angle = angle(previous->game.orbit.angle, current->game.orbit.angle, alpha);
        for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i) {
            if (previous->game.orbit.obstacles[i].ring != current->game.orbit.obstacles[i].ring ||
                previous->game.orbit.obstacles[i].passed != current->game.orbit.obstacles[i].passed) continue;
            current->game.orbit.obstacles[i].angle = angle(previous->game.orbit.obstacles[i].angle,
                                                          current->game.orbit.obstacles[i].angle, alpha);
        }
        for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i)
            if (previous->game.orbit.stars[i].ring == current->game.orbit.stars[i].ring &&
                previous->game.orbit.stars[i].active == current->game.orbit.stars[i].active)
                current->game.orbit.stars[i].angle = angle(previous->game.orbit.stars[i].angle,
                                                          current->game.orbit.stars[i].angle, alpha);
        break;
    case ORB_GAME_POP:
        for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
            if (!previous->game.pop.bubbles[i].active || !current->game.pop.bubbles[i].active ||
                previous->game.pop.bubbles[i].radius != current->game.pop.bubbles[i].radius) continue;
            current->game.pop.bubbles[i].x = blend(previous->game.pop.bubbles[i].x,
                                                   current->game.pop.bubbles[i].x, alpha);
            current->game.pop.bubbles[i].y = blend(previous->game.pop.bubbles[i].y,
                                                   current->game.pop.bubbles[i].y, alpha);
        }
        break;
    default: break;
    }
}

size_t orb_game_snapshot_bytes(orb_game_kind_t kind)
{
    size_t prefix = offsetof(orb_game_state_t, game);
    switch (kind) {
    case ORB_GAME_MARBLE: return offsetof(orb_game_state_t, game.marble.history);
    case ORB_GAME_BEADS: return prefix + sizeof(((orb_game_state_t *)0)->game.beads);
    case ORB_GAME_SNAKE: return prefix + sizeof(((orb_game_state_t *)0)->game.snake);
    case ORB_GAME_ORBIT: return prefix + sizeof(((orb_game_state_t *)0)->game.orbit);
    case ORB_GAME_POP: return prefix + sizeof(((orb_game_state_t *)0)->game.pop);
    default: return sizeof(orb_game_state_t);
    }
}

void orb_game_pose_capture(const orb_game_state_t *state, orb_game_pose_t *pose)
{
    pose->kind = state->kind; pose->ticks = state->ticks;
    pose->playing = state->started && !state->user_paused && !state->game_over;
    pose->controls = state->controls; pose->lives = state->lives;
    switch (state->kind) {
    case ORB_GAME_MARBLE:
        pose->game.marble.x = state->game.marble.x; pose->game.marble.y = state->game.marble.y;
        pose->game.marble.stage = state->game.marble.stage;
        pose->game.marble.rewinding = state->game.marble.rewind_active;
        for (unsigned i = 0; i < ORB_MARBLE_GATES_MAX; ++i)
            pose->game.marble.angle[i] = state->game.marble.gates[i].angle;
        break;
    case ORB_GAME_BEADS:
        pose->game.beads.count = state->game.beads.count;
        pose->game.beads.settled = state->game.beads.settled;
        for (unsigned i = 0; i < state->game.beads.count; ++i)
            pose->game.beads.points[i] = (orb_game_point_t){state->game.beads.beads[i].x, state->game.beads.beads[i].y};
        break;
    case ORB_GAME_SNAKE:
        pose->game.snake.length = state->game.snake.length;
        for (unsigned i = 0; i < state->game.snake.length; ++i)
            pose->game.snake.points[i] = (orb_game_point_t){state->game.snake.cells[i].x, state->game.snake.cells[i].y};
        break;
    case ORB_GAME_ORBIT:
        pose->game.orbit.angle = state->game.orbit.angle; pose->game.orbit.ring = state->game.orbit.ring;
        for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i) {
            pose->game.orbit.comets[i] = state->game.orbit.obstacles[i].angle;
            pose->game.orbit.comet_ring[i] = state->game.orbit.obstacles[i].ring;
            pose->game.orbit.comet_passed[i] = state->game.orbit.obstacles[i].passed;
        }
        for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i) {
            pose->game.orbit.stars[i] = state->game.orbit.stars[i].angle;
            pose->game.orbit.star_ring[i] = state->game.orbit.stars[i].ring;
            pose->game.orbit.star_active[i] = state->game.orbit.stars[i].active;
        }
        break;
    case ORB_GAME_POP:
        for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
            pose->game.pop.points[i] = (orb_game_point_t){state->game.pop.bubbles[i].x, state->game.pop.bubbles[i].y};
            pose->game.pop.radius[i] = state->game.pop.bubbles[i].radius;
            pose->game.pop.active[i] = state->game.pop.bubbles[i].active;
        }
        break;
    default: break;
    }
}

void orb_game_pose_interpolate(orb_game_state_t *state, const orb_game_pose_t *pose, float alpha)
{
    if (!state || !pose || !isfinite(alpha) || pose->kind != state->kind ||
        pose->ticks + 1 != state->ticks || state->user_paused || state->game_over ||
        !state->started || pose->controls != state->controls) return;
    alpha = fmaxf(0, fminf(alpha, 1));
    switch (state->kind) {
    case ORB_GAME_MARBLE:
        if (pose->game.marble.stage != state->game.marble.stage ||
            state->game.marble.stage_cleared || state->game.marble.garden_open ||
            pose->game.marble.rewinding != state->game.marble.rewind_active) break;
        state->game.marble.x = blend(pose->game.marble.x, state->game.marble.x, alpha);
        state->game.marble.y = blend(pose->game.marble.y, state->game.marble.y, alpha);
        for (unsigned i = 0; i < ORB_MARBLE_GATES_MAX; ++i)
            state->game.marble.gates[i].angle = angle(pose->game.marble.angle[i], state->game.marble.gates[i].angle, alpha);
        break;
    case ORB_GAME_BEADS:
        if (pose->game.beads.count != state->game.beads.count ||
            pose->game.beads.settled != state->game.beads.settled ||
            (state->game.beads.settled && state->game.beads.travel_ticks == 45)) break;
        for (unsigned i = 0; i < state->game.beads.count; ++i) {
            state->game.beads.beads[i].x = blend(pose->game.beads.points[i].x, state->game.beads.beads[i].x, alpha);
            state->game.beads.beads[i].y = blend(pose->game.beads.points[i].y, state->game.beads.beads[i].y, alpha);
        }
        break;
    case ORB_GAME_SNAKE:
        if (pose->game.snake.length != state->game.snake.length) break;
        for (unsigned i = 0; i < state->game.snake.length; ++i) {
            state->game.snake.cells[i].x = blend(pose->game.snake.points[i].x, state->game.snake.cells[i].x, alpha);
            state->game.snake.cells[i].y = blend(pose->game.snake.points[i].y, state->game.snake.cells[i].y, alpha);
        }
        break;
    case ORB_GAME_ORBIT:
        if (pose->game.orbit.ring != state->game.orbit.ring || pose->lives != state->lives) break;
        state->game.orbit.angle = angle(pose->game.orbit.angle, state->game.orbit.angle, alpha);
        for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i) {
            if (pose->game.orbit.comet_ring[i] != state->game.orbit.obstacles[i].ring ||
                pose->game.orbit.comet_passed[i] != state->game.orbit.obstacles[i].passed) continue;
            state->game.orbit.obstacles[i].angle = angle(pose->game.orbit.comets[i], state->game.orbit.obstacles[i].angle, alpha);
        }
        for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i)
            if (pose->game.orbit.star_ring[i] == state->game.orbit.stars[i].ring &&
                pose->game.orbit.star_active[i] == state->game.orbit.stars[i].active)
                state->game.orbit.stars[i].angle = angle(pose->game.orbit.stars[i], state->game.orbit.stars[i].angle, alpha);
        break;
    case ORB_GAME_POP:
        for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
            if (!pose->game.pop.active[i] || !state->game.pop.bubbles[i].active ||
                pose->game.pop.radius[i] != state->game.pop.bubbles[i].radius) continue;
            state->game.pop.bubbles[i].x = blend(pose->game.pop.points[i].x, state->game.pop.bubbles[i].x, alpha);
            state->game.pop.bubbles[i].y = blend(pose->game.pop.points[i].y, state->game.pop.bubbles[i].y, alpha);
        }
        break;
    default: break;
    }
}
