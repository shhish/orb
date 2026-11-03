#include "orb_game_logic.h"

#include <math.h>
#include <string.h>

#define TAU 6.2831853071795864769f
#define PI 3.1415926535897932385f
#define STEP (1.0f / 60.0f)
#define WHITE 0xffffffu
#define GREEN 0x30d158u
#define YELLOW 0xffd600u
#define RED 0xff4245u
#define ORANGE 0xff9230u
#define INDIGO 0x6d7cffu
#define PINK 0xff375fu
#define CYAN 0x3cd3feu
#define PURPLE 0xdb34f2u
#define CHROME_GAP 8.0f
#define MARBLE_GRAVITY 480.0f
#define MARBLE_DAMPING 0.980f
#define MARBLE_GATE_CONTACT (ORB_MARBLE_BALL_RADIUS + ORB_MARBLE_GATE_WIDTH * 0.5f)

typedef struct { float x1, y1, x2, y2; } game_rect_t;
/* Pop reserves only its visible score lane; playing has no outgoing chrome. */
static const game_rect_t s_hud = {-64, -190, 64, -140};

static bool outside_rect(float x, float y, float radius, game_rect_t rect)
{
    float margin = radius + CHROME_GAP;
    return x <= rect.x1 - margin + 0.01f || x >= rect.x2 + margin - 0.01f ||
           y <= rect.y1 - margin + 0.01f || y >= rect.y2 + margin - 0.01f;
}

bool orb_game_position_clear(float x, float y, float radius,
                              orb_game_kind_t kind, bool controls)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(radius) || radius < 0 ||
        (unsigned)kind >= ORB_GAME_COUNT || hypotf(x, y) + radius >
        (kind == ORB_GAME_MARBLE ? ORB_MARBLE_RADIUS : ORB_GAME_RADIUS) + 0.01f)
        return false;
    /* HIG §3: collectibles and directly tapped bubbles use the same exclusion
     * geometry as their movement. Chrome never conceals a required target. */
    if (kind == ORB_GAME_POP && !outside_rect(x, y, radius, s_hud)) return false;
    (void)controls;
    return true;
}

static void reflect(float *vx, float *vy, float nx, float ny)
{
    float speed = *vx * nx + *vy * ny;
    if (speed < 0) {
        *vx -= 1.65f * speed * nx;
        *vy -= 1.65f * speed * ny;
    }
}

static void deflect_rect(float *x, float *y, float *vx, float *vy,
                         float radius, game_rect_t rect)
{
    if (outside_rect(*x, *y, radius, rect)) return;
    float margin = radius + CHROME_GAP;
    float distances[] = {*x - rect.x1 + margin, rect.x2 + margin - *x,
                         *y - rect.y1 + margin, rect.y2 + margin - *y};
    unsigned nearest = 0;
    for (unsigned i = 1; i < 4; ++i)
        if (distances[i] < distances[nearest]) nearest = i;
    switch (nearest) {
    case 0: *x = rect.x1 - margin; reflect(vx, vy, -1, 0); break;
    case 1: *x = rect.x2 + margin; reflect(vx, vy, 1, 0); break;
    case 2: *y = rect.y1 - margin; reflect(vx, vy, 0, -1); break;
    default: *y = rect.y2 + margin; reflect(vx, vy, 0, 1); break;
    }
}

static void deflect_chrome(float *x, float *y, float *vx, float *vy,
                           float radius, orb_game_kind_t kind, bool controls)
{
    (void)controls;
    if (kind == ORB_GAME_POP) deflect_rect(x, y, vx, vy, radius, s_hud);
}

static void pop_spawn(orb_game_state_t *state);

static uint32_t random_next(orb_game_state_t *state)
{
    uint32_t x = state->random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state->random = x;
    return x;
}

static float random_unit(orb_game_state_t *state)
{
    return (float)(random_next(state) & 0xffffu) / 65536.0f;
}

static float finite_unit(float value)
{
    if (!isfinite(value)) return 0;
    return fmaxf(-1, fminf(1, value));
}

static float angle_wrap(float angle)
{
    if (angle < 0) angle += TAU;
    if (angle >= TAU) angle -= TAU;
    return angle;
}

static void burst(orb_game_state_t *state, float x, float y, uint32_t colour,
                  uint8_t count, uint8_t ring_radius)
{
    /* One replaceable event keeps memory and per-frame damage bounded even if
     * several gems are caught in one step. It never consumes gameplay RNG. */
    state->effect = (orb_game_effect_t){x, y, colour, 0, 24, count, ring_radius};
}

static void effect_step(orb_game_state_t *state)
{
    if (state->effect.age < state->effect.duration) ++state->effect.age;
    if (state->kind == ORB_GAME_ORBIT && state->game.orbit.trail_age)
        --state->game.orbit.trail_age;
}

static void snake_food(orb_game_state_t *state)
{
    /* Bounded sunflower search leaves at least 47 px between the pellet's
     * edge and the lethal rim: enough room for the maximum-speed turn radius.
     * Never hide food inside the body or spawn it under the head. */
    unsigned first = random_next(state) % 256u;
    for (unsigned n = 0; n < 256u; ++n) {
        unsigned at = (first + n) % 256u;
        float angle = (float)at * 2.39996323f;
        float radius = 155 * sqrtf(((float)at + .5f) / 256);
        orb_game_point_t food = {cosf(angle) * radius, sinf(angle) * radius};
        /* The body may pass under the score disc's glass; a pellet never. */
        bool clear = hypotf(food.x - ORB_GAME_HUD_X, food.y - ORB_GAME_HUD_Y) >= ORB_GAME_HUD_RADIUS + 8 + CHROME_GAP;
        for (unsigned i = 0; clear && i < state->game.snake.length; ++i) {
            orb_game_point_t body = state->game.snake.cells[i];
            if (hypotf(food.x - body.x, food.y - body.y) < (i ? 35 : 70)) {
                clear = false;
                break;
            }
        }
        if (clear) { state->game.snake.food = food; return; }
    }
    /* Filling all safe spawn space completes the arena rather than creating
     * an unreachable pellet or retrying without a time bound. */
    state->game.snake.won = state->game_over = true;
}

const char *orb_game_id(orb_game_kind_t kind)
{
    static const char *const ids[] = {"marble", "beads", "orbit", "ring-snake", "pop"};
    return (unsigned)kind < ORB_GAME_COUNT ? ids[kind] : NULL;
}

const char *orb_game_name(orb_game_kind_t kind)
{
    static const char *const names[] = {"Marble", "Beads", "Orbit", "Snake", "Pop"};
    return (unsigned)kind < ORB_GAME_COUNT ? names[kind] : NULL;
}

uint32_t orb_game_accent(orb_game_kind_t kind)
{
    static const uint32_t accents[] = {
        0x30d158u, /* Marble: green (goal/success) */
        0xff375fu, /* Beads: pink */
        0x6d7cffu, /* Orbit: indigo (planet) */
        0xff9230u, /* Ring Snake: orange */
        0xffd600u, /* Pop: yellow */
    };
    return (unsigned)kind < ORB_GAME_COUNT ? accents[kind] : 0x30d158u;
}

bool orb_game_kind_from_id(const char *id, orb_game_kind_t *out)
{
    if (!id || !out) return false;
    for (unsigned i = 0; i < ORB_GAME_COUNT; ++i) {
        if (!strcmp(id, orb_game_id((orb_game_kind_t)i))) {
            *out = (orb_game_kind_t)i;
            return true;
        }
    }
    return false;
}

static float normalize_angle(float a)
{
    while (a > PI) a -= TAU;
    while (a < -PI) a += TAU;
    return a;
}

typedef struct { float start, end; } marble_arc_t;

/* The full garden is playable with tilt or field taps: no invisible button
 * cut-outs may split a physical ring (owner requirement, HIG §§1/3). */
static size_t marble_gate_arcs(const orb_game_state_t *state, unsigned gate_index,
                               marble_arc_t *arcs)
{
    const orb_marble_gate_t *gate = &state->game.marble.gates[gate_index];
    if (!gate->active) return 0;
    arcs[0] = (marble_arc_t){gate->angle + gate->gap_angle,
                           gate->angle + TAU - gate->gap_angle};
    return 1;
}

static orb_game_point_t marble_arc_nearest(float radius, marble_arc_t arc, float x, float y)
{
    float angle = atan2f(y, x);
    float middle = (arc.start + arc.end) * 0.5f;
    angle = middle + normalize_angle(angle - middle);
    angle = fmaxf(arc.start, fminf(arc.end, angle));
    return (orb_game_point_t){cosf(angle) * radius, sinf(angle) * radius};
}

static bool marble_gates_clear(const orb_game_state_t *state, float x, float y)
{
    for (unsigned g = 0; g < ORB_MARBLE_GATES_MAX; ++g) {
        marble_arc_t arcs[ORB_MARBLE_GATE_SEGMENTS];
        size_t count = marble_gate_arcs(state, g, arcs);
        for (size_t i = 0; i < count; ++i) {
            orb_game_point_t point = marble_arc_nearest(state->game.marble.gates[g].radius, arcs[i], x, y);
            if (hypotf(x - point.x, y - point.y) < MARBLE_GATE_CONTACT + 0.01f)
                return false;
        }
    }
    return true;
}

static void marble_record(orb_game_state_t *state)
{
    unsigned head = state->game.marble.history_head;
    state->game.marble.history[head] = (orb_marble_sample_t){
        (int16_t)lroundf(state->game.marble.x * 16),
        (int16_t)lroundf(state->game.marble.y * 16),
        (int16_t)lroundf(state->game.marble.vx * 16),
        (int16_t)lroundf(state->game.marble.vy * 16)};
    state->game.marble.history_head = (uint16_t)((head + 1) % ORB_MARBLE_HISTORY_MAX);
    if (state->game.marble.history_count < ORB_MARBLE_HISTORY_MAX)
        ++state->game.marble.history_count;
}

static void marble_trail(orb_game_state_t *state)
{
    state->game.marble.trail_count = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned back = (i + 1) * 3;
        if (state->game.marble.history_count <= back) break;
        unsigned at = (state->game.marble.history_head + ORB_MARBLE_HISTORY_MAX - back - 1) % ORB_MARBLE_HISTORY_MAX;
        state->game.marble.trail[i] = (orb_game_point_t){
            (float)state->game.marble.history[at].x / 16,
            (float)state->game.marble.history[at].y / 16};
        ++state->game.marble.trail_count;
    }
}

static void marble_spawn(orb_game_state_t *state)
{
    /* An outer-lane start makes every ring part of the route to the flower.
     * This lower-left quadrant clears the optional touch arrows. */
    state->game.marble.x = -182;
    state->game.marble.y = 103;
    state->game.marble.vx = state->game.marble.vy = 0;
}

static void marble_setup_stage(orb_game_state_t *state, uint8_t stage)
{
    state->game.marble.stage = stage;
    state->game.marble.stage_count = ORB_MARBLE_STAGES;
    state->game.marble.stage_cleared = false;
    state->game.marble.rewind_active = false;
    state->game.marble.rewind_ticks = 0;
    state->game.marble.history_head = state->game.marble.history_count = 0;
    state->game.marble.sample_tick = state->game.marble.trail_count = 0;
    marble_spawn(state);
    /* Design 1.0: every stage has three concentric arches, with generous
     * introductory gaps. Later stages rotate faster without changing lanes. */
    state->game.marble.gates[0] = (orb_marble_gate_t){64, 0.52f,
        0.40f + stage * 0.06f, 0.61f - stage * 0.03f, true};
    state->game.marble.gates[1] = (orb_marble_gate_t){126, 2.62f,
        -0.32f - stage * 0.04f, 0.44f - stage * 0.02f, true};
    state->game.marble.gates[2] = (orb_marble_gate_t){188, 5.24f,
        0.23f + stage * 0.03f, 0.35f - stage * 0.01f, true};
    marble_record(state);
}

/* Rewinding ignores the present gates, but release never injects an old velocity
 * into a moved wall. Stop, then choose the nearest clear local point in a fixed
 * order (at most 32 px). This is deterministic and has no bounce or random kick. */
static void marble_stop_rewind(orb_game_state_t *state)
{
    if (!state->game.marble.rewind_active) return;
    state->game.marble.rewind_active = false;
    state->game.marble.rewind_ticks = 0;
    state->game.marble.trail_count = state->game.marble.sample_tick = 0;
    state->game.marble.vx = state->game.marble.vy = 0;
    float x = state->game.marble.x, y = state->game.marble.y;
    if (marble_gates_clear(state, x, y)) return;
    for (unsigned radius = 4; radius <= 32; radius += 4) {
        for (unsigned i = 0; i < 16; ++i) {
            float angle = (float)i * TAU / 16;
            float candidate_x = x + cosf(angle) * (float)radius;
            float candidate_y = y + sinf(angle) * (float)radius;
            if (orb_game_position_clear(candidate_x, candidate_y, 12, ORB_GAME_MARBLE, state->controls) &&
                marble_gates_clear(state, candidate_x, candidate_y)) {
                state->game.marble.x = candidate_x;
                state->game.marble.y = candidate_y;
                return;
            }
        }
    }
}

/* ---- Beads: fixed-step tilt fluid, settles into the time when still ---- */

#define BEAD_ACCEL_CAP 1850.0f
#define BEAD_RESPONSE_GAIN 3.6f
#define BEAD_DAMPING 0.9975f /* Mild drag: 0.995 per 60 Hz frame. */
#define BEAD_SUBSTEPS 2u
#define BEAD_ITERATIONS 3u
#define BEAD_MAX_SPEED 460.0f
#define BEAD_GRID 30u
#define BEAD_CELL 16u /* At least the full 14 px collision diameter. */
/* Settle spring: the semi-implicit update applies damping after acceleration,
 * so 0.60 gives a nearly critical response here (HIG §6). A looser spring can
 * overshoot a bottom clock site beyond the round panel and visibly lose beads. */
#define BEAD_SETTLE_K 240.0f
#define BEAD_SETTLE_DAMPING 0.60f

orb_game_point_t orb_game_beads_force(float ax, float ay)
{
    /* Physical gravity reaches here unattenuated. A smooth radial response
     * makes a 5–10 degree pour decisive without an abrupt dead zone or a
     * diagonal speed advantage; large tilts asymptote to a safe force cap. */
    float amount = hypotf(ax, ay);
    if (!isfinite(amount) || amount < 0.0000001f) return (orb_game_point_t){0};
    float scale = BEAD_ACCEL_CAP * tanhf(amount * BEAD_RESPONSE_GAIN) / amount;
    return (orb_game_point_t){-ax * scale, ay * scale};
}

/* Inter 4.1 (OFL), weight 800, optical size 32, sampled at the frame's
 * 150 px size on a 3 px mask. Sampling that silhouette on the SVG's global
 * 9 px lattice preserves its proportional curves and counters (HIG §§4/5).
 * Edge coverage keeps a clipped bead from merging a colon into its neighbour. */
static const uint64_t s_bead_digits[11][39] = {
    /* 0 */ {
        UINT64_C(0x00007c000), UINT64_C(0x0003ff800), UINT64_C(0x000ffff00), UINT64_C(0x003ffff00), UINT64_C(0x007ffffc0), UINT64_C(0x00fffffc0),
        UINT64_C(0x00ffffff0), UINT64_C(0x01ffc7ff0), UINT64_C(0x03ff01ff8), UINT64_C(0x03fe00ff8), UINT64_C(0x03fc007fc), UINT64_C(0x07fc007fc),
        UINT64_C(0x07fc003fc), UINT64_C(0x07f8003fc), UINT64_C(0x0ff8003fc), UINT64_C(0x0ff8003fc), UINT64_C(0x0ff0003fe), UINT64_C(0x0ff0003fe),
        UINT64_C(0x0ff0003fe), UINT64_C(0x0ff0003fe), UINT64_C(0x0ff0003fe), UINT64_C(0x0ff0003fe), UINT64_C(0x0ff8003fc), UINT64_C(0x0ff8003fc),
        UINT64_C(0x0ff8003fc), UINT64_C(0x07f8003fc), UINT64_C(0x07fc007fc), UINT64_C(0x07fc007fc), UINT64_C(0x03fe00ff8), UINT64_C(0x03ff00ff8),
        UINT64_C(0x01ff83ff0), UINT64_C(0x01ffffff0), UINT64_C(0x00fffffe0), UINT64_C(0x00fffffc0), UINT64_C(0x003ffff80), UINT64_C(0x001ffff00),
        UINT64_C(0x0007ffc00), UINT64_C(0x0001ff000), UINT64_C(0x000000000)
    },
    /* 1 */ {
        UINT64_C(0x000000000), UINT64_C(0x00003fe00), UINT64_C(0x00003ff80), UINT64_C(0x00003ffc0), UINT64_C(0x00003fff0), UINT64_C(0x00003fffc),
        UINT64_C(0x00003fffe), UINT64_C(0x00003fffe), UINT64_C(0x00003fffe), UINT64_C(0x00003fefe), UINT64_C(0x00003fe3e), UINT64_C(0x00003fe1e),
        UINT64_C(0x00003fe06), UINT64_C(0x00003fe02), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00),
        UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00),
        UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00),
        UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00), UINT64_C(0x00003fe00),
        UINT64_C(0x00003fe00), UINT64_C(0x000000000), UINT64_C(0x000000000)
    },
    /* 2 */ {
        UINT64_C(0x00003f000), UINT64_C(0x0001ffc00), UINT64_C(0x000ffff80), UINT64_C(0x001ffffc0), UINT64_C(0x003fffff0), UINT64_C(0x007fffff0),
        UINT64_C(0x00ffffff8), UINT64_C(0x00ffe3ff8), UINT64_C(0x00ff807fc), UINT64_C(0x00ff007fc), UINT64_C(0x01ff003fc), UINT64_C(0x01ff003fc),
        UINT64_C(0x01ff003fc), UINT64_C(0x01ff003fc), UINT64_C(0x00ff00000), UINT64_C(0x00ff00000), UINT64_C(0x00ffc0000), UINT64_C(0x00ffc0000),
        UINT64_C(0x003ff0000), UINT64_C(0x003ff0000), UINT64_C(0x001ffc000), UINT64_C(0x000ffc000), UINT64_C(0x0003ff000), UINT64_C(0x0003ff800),
        UINT64_C(0x0000ffe00), UINT64_C(0x0000fff00), UINT64_C(0x00003ff80), UINT64_C(0x00001ffc0), UINT64_C(0x00000fff0), UINT64_C(0x000003ff0),
        UINT64_C(0x01ffffffc), UINT64_C(0x01ffffffc), UINT64_C(0x01ffffffc), UINT64_C(0x01ffffffc), UINT64_C(0x01ffffffc), UINT64_C(0x01ffffffc),
        UINT64_C(0x01ffffffc), UINT64_C(0x000000000), UINT64_C(0x000000000)
    },
    /* 3 */ {
        UINT64_C(0x0000ff800), UINT64_C(0x0003ffe00), UINT64_C(0x000ffffc0), UINT64_C(0x001ffffc0), UINT64_C(0x003fffff0), UINT64_C(0x007fffff0),
        UINT64_C(0x00ffffff8), UINT64_C(0x00ffc1ff8), UINT64_C(0x00ff007fc), UINT64_C(0x00ff007fc), UINT64_C(0x00ff003fc), UINT64_C(0x00ff003fc),
        UINT64_C(0x00ff00000), UINT64_C(0x00ff00000), UINT64_C(0x00ff80000), UINT64_C(0x007fc0000), UINT64_C(0x003fff000), UINT64_C(0x000fff000),
        UINT64_C(0x0007ff000), UINT64_C(0x001fff000), UINT64_C(0x007fff000), UINT64_C(0x00ffff000), UINT64_C(0x00ff80000), UINT64_C(0x01ff00000),
        UINT64_C(0x03fe00000), UINT64_C(0x03fe003fe), UINT64_C(0x03fe003fe), UINT64_C(0x03fe003fc), UINT64_C(0x03ff003fc), UINT64_C(0x03ff007fc),
        UINT64_C(0x01ffc1ffc), UINT64_C(0x00ffffff8), UINT64_C(0x00ffffff0), UINT64_C(0x007fffff0), UINT64_C(0x003ffffc0), UINT64_C(0x001ffffc0),
        UINT64_C(0x0003fff00), UINT64_C(0x0000ff800), UINT64_C(0x000000000)
    },
    /* 4 */ {
        UINT64_C(0x000000000), UINT64_C(0x00fff0000), UINT64_C(0x00fff0000), UINT64_C(0x00fff8000), UINT64_C(0x00fffc000), UINT64_C(0x00fffc000),
        UINT64_C(0x00ffff000), UINT64_C(0x00ffff000), UINT64_C(0x00ffff800), UINT64_C(0x00ffff800), UINT64_C(0x00ffbfc00), UINT64_C(0x00ffbfc00),
        UINT64_C(0x00ff8ff00), UINT64_C(0x00ff8ff00), UINT64_C(0x00ff87f80), UINT64_C(0x00ff87fc0), UINT64_C(0x00ff83fc0), UINT64_C(0x00ff81fe0),
        UINT64_C(0x00ff80ff0), UINT64_C(0x00ff80ff0), UINT64_C(0x00ff807f8), UINT64_C(0x00ff803fc), UINT64_C(0x00ff803fc), UINT64_C(0x0fffffffe),
        UINT64_C(0x0fffffffe), UINT64_C(0x0fffffffe), UINT64_C(0x0fffffffe), UINT64_C(0x0fffffffe), UINT64_C(0x0fffffffe), UINT64_C(0x0fffffffe),
        UINT64_C(0x00ff80000), UINT64_C(0x00ff80000), UINT64_C(0x00ff80000), UINT64_C(0x00ff80000), UINT64_C(0x00ff80000), UINT64_C(0x00ff80000),
        UINT64_C(0x00ff80000), UINT64_C(0x00ff80000), UINT64_C(0x000000000)
    },
    /* 5 */ {
        UINT64_C(0x000000000), UINT64_C(0x007fffffc), UINT64_C(0x007fffffc), UINT64_C(0x007fffffc), UINT64_C(0x007fffffc), UINT64_C(0x007fffffc),
        UINT64_C(0x007fffffc), UINT64_C(0x007fffffc), UINT64_C(0x0000003fc), UINT64_C(0x0000003fc), UINT64_C(0x0000003fc), UINT64_C(0x0000003fc),
        UINT64_C(0x00007c3fc), UINT64_C(0x0001ff3fc), UINT64_C(0x000fffffc), UINT64_C(0x000fffffc), UINT64_C(0x003fffffc), UINT64_C(0x003fffffc),
        UINT64_C(0x00ffffffc), UINT64_C(0x00ffc0ffc), UINT64_C(0x00ff803fc), UINT64_C(0x00ff003fc), UINT64_C(0x01ff00000), UINT64_C(0x01ff00000),
        UINT64_C(0x01fe00000), UINT64_C(0x01fe00000), UINT64_C(0x01ff001fe), UINT64_C(0x01ff003fe), UINT64_C(0x00ff003fc), UINT64_C(0x00ff003fc),
        UINT64_C(0x00ffc0ffc), UINT64_C(0x00ffffffc), UINT64_C(0x007fffff0), UINT64_C(0x003fffff0), UINT64_C(0x001ffffe0), UINT64_C(0x000ffffc0),
        UINT64_C(0x0003fff00), UINT64_C(0x0000ffc00), UINT64_C(0x000000000)
    },
    /* 6 */ {
        UINT64_C(0x0001ff000), UINT64_C(0x0003ffc00), UINT64_C(0x001ffff00), UINT64_C(0x003ffff80), UINT64_C(0x007ffffc0), UINT64_C(0x00fffffe0),
        UINT64_C(0x00ffffff0), UINT64_C(0x01ff83ff0), UINT64_C(0x03ff00ff8), UINT64_C(0x03fe007f8), UINT64_C(0x03fc003fc), UINT64_C(0x0000003fc),
        UINT64_C(0x0000003fc), UINT64_C(0x0000f03fc), UINT64_C(0x000fff1fc), UINT64_C(0x001fff9fc), UINT64_C(0x007fffdfe), UINT64_C(0x00ffffffe),
        UINT64_C(0x00ffffffe), UINT64_C(0x01ffffffe), UINT64_C(0x03ff00ffe), UINT64_C(0x03ff00ffe), UINT64_C(0x03fe007fe), UINT64_C(0x03fc007fc),
        UINT64_C(0x03fc003fc), UINT64_C(0x03fc003fc), UINT64_C(0x03fc003fc), UINT64_C(0x03fc007fc), UINT64_C(0x03fe00ffc), UINT64_C(0x03ff00ff8),
        UINT64_C(0x01ffc3ff0), UINT64_C(0x01ffffff0), UINT64_C(0x00fffffe0), UINT64_C(0x00fffffc0), UINT64_C(0x003ffff80), UINT64_C(0x001ffff00),
        UINT64_C(0x0003ffc00), UINT64_C(0x0000ff000), UINT64_C(0x000000000)
    },
    /* 7 */ {
        UINT64_C(0x000000000), UINT64_C(0x007fffffe), UINT64_C(0x007fffffe), UINT64_C(0x007fffffe), UINT64_C(0x007fffffe), UINT64_C(0x007fffffe),
        UINT64_C(0x007fffffe), UINT64_C(0x007fffffe), UINT64_C(0x003fc0000), UINT64_C(0x003fc0000), UINT64_C(0x003fe0000), UINT64_C(0x001ff0000),
        UINT64_C(0x000ff0000), UINT64_C(0x000ff0000), UINT64_C(0x000ff8000), UINT64_C(0x0007fc000), UINT64_C(0x0003fc000), UINT64_C(0x0003fc000),
        UINT64_C(0x0003fe000), UINT64_C(0x0001ff000), UINT64_C(0x0000ff000), UINT64_C(0x0000ff000), UINT64_C(0x0000ff800), UINT64_C(0x00007fc00),
        UINT64_C(0x00003fc00), UINT64_C(0x00003fc00), UINT64_C(0x00003fe00), UINT64_C(0x00001ff00), UINT64_C(0x00000ff00), UINT64_C(0x00000ff00),
        UINT64_C(0x00000ff80), UINT64_C(0x000007fc0), UINT64_C(0x000003fc0), UINT64_C(0x000003fc0), UINT64_C(0x000003fe0), UINT64_C(0x000001ff0),
        UINT64_C(0x000000ff0), UINT64_C(0x000000ff0), UINT64_C(0x000000000)
    },
    /* 8 */ {
        UINT64_C(0x0000ff800), UINT64_C(0x0003ffe00), UINT64_C(0x001ffffc0), UINT64_C(0x003ffffc0), UINT64_C(0x007fffff0), UINT64_C(0x00ffffff0),
        UINT64_C(0x00ffe3ff8), UINT64_C(0x00ffc0ff8), UINT64_C(0x01ff00ffc), UINT64_C(0x01ff007fc), UINT64_C(0x01ff007fc), UINT64_C(0x01ff007fc),
        UINT64_C(0x00ff007f8), UINT64_C(0x00ff00ff8), UINT64_C(0x00ffc0ff0), UINT64_C(0x007fe3ff0), UINT64_C(0x003ffffc0), UINT64_C(0x000ffff80),
        UINT64_C(0x0003ffe00), UINT64_C(0x000ffff80), UINT64_C(0x007fffff0), UINT64_C(0x00ffffff0), UINT64_C(0x01ff80ffc), UINT64_C(0x01ff00ffc),
        UINT64_C(0x03ff003fc), UINT64_C(0x03fe003fc), UINT64_C(0x03fe003fe), UINT64_C(0x03fe003fe), UINT64_C(0x03fe003fe), UINT64_C(0x03ff003fc),
        UINT64_C(0x03ff80ffc), UINT64_C(0x01ffc1ffc), UINT64_C(0x00ffffff8), UINT64_C(0x00ffffff8), UINT64_C(0x003ffffe0), UINT64_C(0x003ffffc0),
        UINT64_C(0x0007fff00), UINT64_C(0x0000ff800), UINT64_C(0x000000000)
    },
    /* 9 */ {
        UINT64_C(0x00003f000), UINT64_C(0x0000ffc00), UINT64_C(0x0007fff80), UINT64_C(0x000ffffc0), UINT64_C(0x003fffff0), UINT64_C(0x003fffff0),
        UINT64_C(0x007fffff8), UINT64_C(0x00ffe3ffc), UINT64_C(0x00ff80ffc), UINT64_C(0x00ff007fc), UINT64_C(0x01ff003fc), UINT64_C(0x01fe003fe),
        UINT64_C(0x03fe003fe), UINT64_C(0x03fe003fe), UINT64_C(0x03fe003fe), UINT64_C(0x03ff003fc), UINT64_C(0x03ff007fc), UINT64_C(0x03ff80ffc),
        UINT64_C(0x03ffe3ffc), UINT64_C(0x03ffffff8), UINT64_C(0x03ffffff0), UINT64_C(0x03ffffff0), UINT64_C(0x03fdfffc0), UINT64_C(0x03fcfff80),
        UINT64_C(0x03fc1fc00), UINT64_C(0x03fc00000), UINT64_C(0x01fe00000), UINT64_C(0x01fe003fc), UINT64_C(0x00ff003fc), UINT64_C(0x00ff007fc),
        UINT64_C(0x00ffc1ffc), UINT64_C(0x007fffff8), UINT64_C(0x003fffff0), UINT64_C(0x003fffff0), UINT64_C(0x000ffffc0), UINT64_C(0x000ffffc0),
        UINT64_C(0x0001ffe00), UINT64_C(0x00007f800), UINT64_C(0x000000000)
    },
    /* : */ {
        UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x000000000),
        UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x000000000),
        UINT64_C(0x0000000f0), UINT64_C(0x0000001f8), UINT64_C(0x0000003fc), UINT64_C(0x0000003fc), UINT64_C(0x0000007fc), UINT64_C(0x0000007fc),
        UINT64_C(0x0000003fc), UINT64_C(0x0000003fc), UINT64_C(0x0000001f0), UINT64_C(0x0000000e0), UINT64_C(0x000000000), UINT64_C(0x000000000),
        UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x000000000), UINT64_C(0x0000000f0), UINT64_C(0x0000001f8),
        UINT64_C(0x0000003fc), UINT64_C(0x0000003fc), UINT64_C(0x0000007fc), UINT64_C(0x0000007fc), UINT64_C(0x0000003fc), UINT64_C(0x0000003fc),
        UINT64_C(0x0000001f0), UINT64_C(0x0000000e0), UINT64_C(0x000000000)
    },
};
/* Optical spacing keeps the narrow one clear of a neighbouring round digit. */
static const uint8_t s_bead_advances[11] = {100, 67, 92, 94, 101, 91, 95, 85, 94, 95, 38};

static void bead_target(orb_game_state_t *state, unsigned *count, int x, int y, uint8_t opacity)
{
    if (*count >= ORB_GAME_BEAD_COUNT) return;
    /* Every clock/pool site preserves the same visible separation as play,
     * including a loose bead which falls close to the pool's lattice. */
    for (unsigned i = 0; i < *count; ++i) {
        int dx = x - 233 - state->game.beads.target_x[i];
        int dy = y - 233 - state->game.beads.target_y[i];
        if (dx * dx + dy * dy < 15 * 15) return;
    }
    state->game.beads.target_x[*count] = (int16_t)(x - 233);
    state->game.beads.target_y[*count] = (int16_t)(y - 233);
    state->game.beads.target_opacity[*count] = opacity;
    ++*count;
}

static unsigned bead_glyph_coverage(unsigned digit, float x, float y)
{
    /* Keep a full lattice column empty on either side of the colon. Whole
     * particle discs cannot reproduce SVG's clipped rightmost edge; two clear
     * dot columns preserve the punctuation without joining a zero's stroke. */
    if (digit == 10)
        return x >= 9 && x <= 24 && ((y >= 40 && y <= 64) || (y >= 85 && y <= 109)) ? 9 : 0;
    unsigned covered = 0;
    for (int dy = -2; dy <= 2; dy += 2) {
        for (int dx = -2; dx <= 2; dx += 2) {
            int col = (int)floorf((x + (float)dx) / 3.0f);
            int row = (int)floorf((y + (float)dy) / 3.0f);
            if (col < 0 || col >= 36 || row < 0 || row >= 39) continue;
            if (s_bead_digits[digit][row] & (UINT64_C(1) << col)) ++covered;
        }
    }
    return covered;
}

static void beads_assign_targets(orb_game_state_t *state)
{
    uint8_t hour = state->game.beads.hour, minute = state->game.beads.minute;
    uint8_t glyph[5];
    unsigned glyph_count = 0;
    if (hour >= 10) glyph[glyph_count++] = hour / 10;
    glyph[glyph_count++] = hour % 10;
    glyph[glyph_count++] = 10; /* Colon. */
    glyph[glyph_count++] = minute / 10;
    glyph[glyph_count++] = minute % 10;
    int width = -4 * ((int)glyph_count - 1);
    for (unsigned g = 0; g < glyph_count; ++g) width += s_bead_advances[glyph[g]];
    float left = 233.0f - (float)width * 0.5f;
    unsigned count = 0;
    for (unsigned g = 0; g < glyph_count; ++g) {
        unsigned digit = glyph[g];
        for (int y = 149; y <= 266; y += 15) {
            for (int x = 5; x < 466; x += 15) {
                unsigned coverage = bead_glyph_coverage(digit, (float)x - left - 0.5f, (float)y - 150.5f);
                if (coverage >= 4) bead_target(state, &count, x, y, (uint8_t)(255u * coverage / 9));
            }
        }
        left += s_bead_advances[digit] - 4;
    }
    state->game.beads.digit_count = (uint16_t)count;
    /* Fill upward from the round lower edge. The pool owns every remaining
     * particle, so a minute change never creates/disappears individual beads.
     * Its depth varies naturally with the number needed for the time. */
    static const int16_t loose[4][2] = {{140, 330}, {322, 318}, {250, 336}, {199, 322}};
    for (unsigned i = 0; i < 4; ++i)
        bead_target(state, &count, loose[i][0], loose[i][1], i == 3 ? 153 : 255);
    /* Hex rows like the resting pool (14 px rows, 8 px offsets: >= 15 px). */
    const int edge = 233 - ORB_GAME_BEAD_RADIUS;
    for (int row = 0, y = 440; y >= 282 && count < ORB_GAME_BEAD_COUNT; ++row, y -= 14) {
        for (int x = 53 + (row & 1) * 8; x <= 413 && count < ORB_GAME_BEAD_COUNT; x += 15) {
            int dx = x - 233, dy = y - 233;
            if (dx * dx + dy * dy <= edge * edge)
                bead_target(state, &count, x, y, 115);
        }
    }
    state->game.beads.count = (uint16_t)count;
    state->game.beads.travel_ticks = 0;
}

static void beads_spawn(orb_game_state_t *state)
{
    /* A level pool under the stand's screen-down gravity. Hex rows one
     * collision diameter apart; the final row spreads across the surface. */
    const float diameter = ORB_GAME_BEAD_RADIUS * 2.0f, pitch = diameter * 0.8660254f;
    const float edge = ORB_GAME_RADIUS - ORB_GAME_BEAD_RADIUS;
    unsigned count = 0;
    for (unsigned row = 0; count < ORB_GAME_BEAD_COUNT; ++row) {
        float y = edge - (float)row * pitch;
        float half = sqrtf(fmaxf(0, edge * edge - y * y)), offset = (row & 1u) ? diameter * 0.5f : 0;
        int first = (int)ceilf((-half - offset) / diameter), last = (int)floorf((half - offset) / diameter);
        if (last < first) continue;
        unsigned slots = (unsigned)(last - first + 1);
        unsigned take = ORB_GAME_BEAD_COUNT - count < slots ? ORB_GAME_BEAD_COUNT - count : slots;
        for (unsigned i = 0; i < take; ++i) {
            unsigned slot = take == slots ? i : (2u * i + 1u) * slots / (2u * take);
            state->game.beads.beads[count++] = (orb_game_bead_t){
                offset + (float)(first + (int)slot) * diameter, y, 0, 0};
        }
    }
}

void orb_game_beads_set_time(orb_game_state_t *state, uint8_t hour, uint8_t minute)
{
    if (!state || state->kind != ORB_GAME_BEADS) return;
    hour %= 24;
    minute %= 60;
    if (state->game.beads.time_valid && state->game.beads.hour == hour &&
        state->game.beads.minute == minute) return;
    state->game.beads.time_valid = true;
    state->game.beads.hour = hour;
    state->game.beads.minute = minute;
    /* A minute change must update a held clock without waiting for a tilt. */
    beads_assign_targets(state);
}

/* Spatial grid head/next scratch: file-scope so physics never allocates. */
static uint16_t s_bead_cells[BEAD_GRID * BEAD_GRID];
static uint16_t s_bead_next[ORB_GAME_BEAD_COUNT];

static unsigned bead_cell_index(float x, float y)
{
    int cx = (int)((x + 233.0f) / BEAD_CELL);
    int cy = (int)((y + 233.0f) / BEAD_CELL);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= (int)BEAD_GRID) cx = (int)BEAD_GRID - 1;
    if (cy >= (int)BEAD_GRID) cy = (int)BEAD_GRID - 1;
    return (unsigned)(cy * BEAD_GRID + cx);
}

static void beads_collide(orb_game_state_t *state)
{
    const float min_dist = (float)(ORB_GAME_BEAD_RADIUS * 2);
    for (unsigned i = 0; i < BEAD_GRID * BEAD_GRID; ++i) s_bead_cells[i] = 0xFFFF;
    for (unsigned i = 0; i < state->game.beads.count; ++i) {
        unsigned cell = bead_cell_index(state->game.beads.beads[i].x, state->game.beads.beads[i].y);
        s_bead_next[i] = s_bead_cells[cell];
        s_bead_cells[cell] = (uint16_t)i;
    }
    for (unsigned i = 0; i < state->game.beads.count; ++i) {
        orb_game_bead_t *a = &state->game.beads.beads[i];
        int cx = (int)((a->x + 233.0f) / BEAD_CELL);
        int cy = (int)((a->y + 233.0f) / BEAD_CELL);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int gx = cx + dx, gy = cy + dy;
                if (gx < 0 || gy < 0 || gx >= (int)BEAD_GRID || gy >= (int)BEAD_GRID) continue;
                unsigned cell = (unsigned)(gy * BEAD_GRID + gx);
                for (unsigned j = s_bead_cells[cell]; j != 0xFFFF; j = s_bead_next[j]) {
                    if (j <= i) break; /* Descending chains contain only visited pairs from here. */
                    orb_game_bead_t *b = &state->game.beads.beads[j];
                    float ddx = b->x - a->x, ddy = b->y - a->y;
                    float distance2 = ddx * ddx + ddy * ddy;
                    if (distance2 >= min_dist * min_dist) continue;
                    float dist = sqrtf(distance2);
                    if (dist < 0.001f) {
                        /* Coincident particles need a deterministic separating
                         * normal too; never leave an unresolvable clump. */
                        ddx = (i & 1u) ? 0.001f : -0.001f;
                        ddy = 0;
                        dist = 0.001f;
                    }
                    float nx = ddx / dist, ny = ddy / dist;
                    float overlap = min_dist - dist;
                    a->x -= nx * overlap * 0.5f; a->y -= ny * overlap * 0.5f;
                    b->x += nx * overlap * 0.5f; b->y += ny * overlap * 0.5f;
                    /* Equal-mass, inelastic contact shares approaching speed.
                     * The opposite sign injects energy into every overlap and
                     * makes a neutral puddle jitter forever. */
                    float relative = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;
                    if (relative < 0) {
                        a->vx += relative * 0.5f * nx; a->vy += relative * 0.5f * ny;
                        b->vx -= relative * 0.5f * nx; b->vy -= relative * 0.5f * ny;
                    }
                    /* Mild symmetric viscosity only removes relative kinetic
                     * energy. Projection is a split positional correction and
                     * is never converted into a spurious velocity impulse. */
                    float tx = (b->vx - a->vx) * 0.004f;
                    float ty = (b->vy - a->vy) * 0.004f;
                    a->vx += tx; a->vy += ty;
                    b->vx -= tx; b->vy -= ty;
                }
            }
        }
    }
}

static void beads_wall(orb_game_state_t *state)
{
    const float edge = ORB_GAME_RADIUS - ORB_GAME_BEAD_RADIUS;
    for (unsigned i = 0; i < state->game.beads.count; ++i) {
        orb_game_bead_t *b = &state->game.beads.beads[i];
        /* A full-pixel guard rejects only points whose rounded hypotf is inside. */
        float length = b->x * b->x + b->y * b->y <= (edge - 1) * (edge - 1) ? 0 : hypotf(b->x, b->y);
        if (length > edge) {
            float nx = b->x / length, ny = b->y / length;
            b->x = nx * edge; b->y = ny * edge;
            float into = b->vx * nx + b->vy * ny;
            if (into > 0) {
                /* Sand settles against the rim instead of bouncing like a ball.
                 * Retain tangential momentum so it can pour around the bowl. */
                b->vx -= into * nx;
                b->vy -= into * ny;
                b->vx *= 0.998f;
                b->vy *= 0.998f;
            }
        }
    }
}

static void beads_step(orb_game_state_t *state, float ax, float ay)
{
    if (state->controls) {
        bool held = state->game.beads.touch_held;
        float dx = state->game.beads.touch_x / 200, dy = state->game.beads.touch_y / 200;
        float amount = fmaxf(1, hypotf(dx, dy));
        ax = held ? -dx / amount : 0;
        ay = held ? dy / amount : 0;
        state->motion_moving = held;
        if (held) state->game.beads.touch_quiet_ticks = 0;
        else if (state->game.beads.touch_quiet_ticks < ORB_GAME_BEAD_SETTLE_STEPS)
            ++state->game.beads.touch_quiet_ticks;
        /* Releasing the touch alternative is its explicit rest gesture;
         * actual tilt mode still requires sampled gyro-confirmed desk quiet. */
        state->game.beads.settle_ticks = state->game.beads.touch_quiet_ticks;
    }
    bool settled = state->game.beads.settled;
    if (settled && state->motion_moving) {
        state->game.beads.settled = false;
        state->game.beads.settle_ticks = 0;
        settled = false;
    } else if (!settled && state->game.beads.time_valid &&
               state->game.beads.settle_ticks >= ORB_GAME_BEAD_SETTLE_STEPS) {
        settled = state->game.beads.settled = true;
        beads_assign_targets(state);
    }
    if (settled) {
        state->game.beads.flow_speed = 0;
        /* Fixed-step spring travel has a finite deadline: a held clock must
         * be exact and motionless, rather than forever hovering near a cell. */
        if (state->game.beads.travel_ticks < 45) ++state->game.beads.travel_ticks;
        for (unsigned i = 0; i < state->game.beads.count; ++i) {
            orb_game_bead_t *b = &state->game.beads.beads[i];
            float dx = (float)state->game.beads.target_x[i] - b->x;
            float dy = (float)state->game.beads.target_y[i] - b->y;
            b->vx = (b->vx + dx * BEAD_SETTLE_K * STEP) * BEAD_SETTLE_DAMPING;
            b->vy = (b->vy + dy * BEAD_SETTLE_K * STEP) * BEAD_SETTLE_DAMPING;
            b->x += b->vx * STEP;
            b->y += b->vy * STEP;
            if (state->game.beads.travel_ticks >= 45 ||
                (fabsf(dx) < 0.6f && fabsf(dy) < 0.6f &&
                 fabsf(b->vx) < 4 && fabsf(b->vy) < 4)) {
                b->x = (float)state->game.beads.target_x[i];
                b->y = (float)state->game.beads.target_y[i];
                b->vx = b->vy = 0;
            }
        }
    } else {
        /* Specific force points uphill; downhill is -ax/+ay in LVGL pixels. */
        orb_game_point_t gravity = orb_game_beads_force(ax, ay);
        float gx = gravity.x, gy = gravity.y;
        const float dt = STEP / BEAD_SUBSTEPS;
        for (unsigned substep = 0; substep < BEAD_SUBSTEPS; ++substep) {
            for (unsigned i = 0; i < state->game.beads.count; ++i) {
                orb_game_bead_t *b = &state->game.beads.beads[i];
                state->game.beads.previous[i] = (orb_game_point_t){b->x, b->y};
                b->vx = (b->vx + gx * dt) * BEAD_DAMPING;
                b->vy = (b->vy + gy * dt) * BEAD_DAMPING;
                float speed2 = b->vx * b->vx + b->vy * b->vy;
                if (speed2 > BEAD_MAX_SPEED * BEAD_MAX_SPEED) {
                    float scale = BEAD_MAX_SPEED / sqrtf(speed2);
                    b->vx *= scale; b->vy *= scale;
                }
                b->x += b->vx * dt;
                b->y += b->vy * dt;
            }
            /* Rebuild the existing spatial hash on every constraint pass.
             * Smaller integration steps prevent tunnelling, repeated contacts
             * distribute pile pressure instead of a single visible shove. */
            for (unsigned iteration = 0; iteration < BEAD_ITERATIONS; ++iteration) {
                beads_collide(state);
                beads_wall(state);
            }
            for (unsigned i = 0; i < state->game.beads.count; ++i) {
                orb_game_bead_t *b = &state->game.beads.beads[i];
                float vx = (b->x - state->game.beads.previous[i].x) / dt;
                float vy = (b->y - state->game.beads.previous[i].y) / dt;
                /* PBD velocity follows actual constrained travel. Bound it by
                 * the inelastic contact result: overlap correction must never
                 * create kinetic energy (including the initial random pile). */
                float energy = b->vx * b->vx + b->vy * b->vy;
                float speed2 = vx * vx + vy * vy;
                float scale = speed2 > energy ? sqrtf(energy / speed2) : 1;
                b->vx = vx * scale; b->vy = vy * scale;
            }
        }
    }
    /* The designed pool sits inside the round panel but can extend a
     * fraction past the fluid's inset wall; never dislodge a locked site. */
    if (!settled) beads_wall(state);
    if (!settled) {
        float speed = 0;
        for (unsigned i = 0; i < state->game.beads.count; ++i) {
            const orb_game_bead_t *b = &state->game.beads.beads[i];
            speed += b->vx * b->vx + b->vy * b->vy;
        }
        state->game.beads.flow_speed = state->game.beads.count ?
            sqrtf(speed / (float)state->game.beads.count) : 0;
    }
}

void orb_game_init(orb_game_state_t *state, orb_game_kind_t kind, uint32_t seed)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->kind = kind;
    state->random = seed ? seed : 0x6f726231u;
    state->lives = 3;
    state->started = true;
    state->light_x = -3;
    state->light_y = -4;
    switch (kind) {
    case ORB_GAME_MARBLE:
        marble_setup_stage(state, 0);
        break;
    case ORB_GAME_BEADS:
        beads_spawn(state);
        beads_assign_targets(state);
        break;
    case ORB_GAME_ORBIT:
        state->game.orbit.previous_radius = ORB_GAME_ORBIT_R1;
        state->game.orbit.angle = -PI * 0.25f;
        for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i) {
            state->game.orbit.obstacles[i].angle = (float)i * 1.15f;
            state->game.orbit.obstacles[i].ring = (uint8_t)(i % ORB_GAME_ORBIT_RINGS);
        }
        for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i)
            state->game.orbit.stars[i] = (orb_game_star_t){
                (float)i * 2.0f, (uint8_t)((i + 1) % ORB_GAME_ORBIT_RINGS), true};
        break;
    case ORB_GAME_SNAKE:
        state->game.snake.length = 6;
        state->game.snake.heading = -PI / 2;
        state->game.snake.grace = ORB_GAME_SNAKE_GRACE;
        /* A centre/up start gives a full two seconds before the rim. The
         * first pellet teaches straight motion before demanding a turn. */
        for (unsigned i = 0; i < state->game.snake.length; ++i)
            state->game.snake.cells[i] = (orb_game_point_t){0, (float)i * ORB_GAME_SNAKE_SPACING};
        state->game.snake.food = (orb_game_point_t){0, -96};
        break;
    case ORB_GAME_POP:
        /* A round starts with visible choices, rather than one lonely outline.
         * Their existing velocities make them drift as soon as Play begins. */
        for (unsigned i = 0; i < 3; ++i) pop_spawn(state);
        state->game.pop.spawn_in = 68;
        state->game.pop.time_left = ORB_GAME_POP_ROUND_STEPS;
        break;
    default:
        state->game_over = true;
        break;
    }
}

static void marble_step(orb_game_state_t *state, float ax, float ay)
{
    if (state->game.marble.stage_cleared) return;
    for (unsigned g = 0; g < ORB_MARBLE_GATES_MAX; ++g) {
        orb_marble_gate_t *gate = &state->game.marble.gates[g];
        if (gate->active) gate->angle = angle_wrap(gate->angle + gate->speed * STEP);
    }
    if (state->game.marble.rewind_active) {
        if (state->game.marble.rewind_ticks < UINT16_MAX) ++state->game.marble.rewind_ticks;
        /* Consume at the same 30 Hz as recording; gate time always advances. */
        if (++state->game.marble.sample_tick >= 2) {
            state->game.marble.sample_tick = 0;
            if (state->game.marble.history_count > 1) {
                /* The ghost belongs behind the marble in either direction.
                 * During rewind these are departed positions, not the older
                 * samples that the marble has yet to revisit. */
                if (state->game.marble.rewind_ticks % 6 == 2) {
                    unsigned trail = state->game.marble.trail_count;
                    if (trail < 4) ++state->game.marble.trail_count;
                    if (trail > 3) trail = 3;
                    memmove(state->game.marble.trail + 1, state->game.marble.trail,
                        trail * sizeof(state->game.marble.trail[0]));
                    state->game.marble.trail[0] = (orb_game_point_t){
                        state->game.marble.x, state->game.marble.y};
                }
                unsigned previous = (state->game.marble.history_head + ORB_MARBLE_HISTORY_MAX - 2) % ORB_MARBLE_HISTORY_MAX;
                state->game.marble.history_head = (uint16_t)((previous + 1) % ORB_MARBLE_HISTORY_MAX);
                --state->game.marble.history_count;
                state->game.marble.x = (float)state->game.marble.history[previous].x / 16;
                state->game.marble.y = (float)state->game.marble.history[previous].y / 16;
            }
        }
        /* An empty history remains held, stationary; releasing the control
         * is the only way to resume. There is no surprise forward movement. */
        return;
    }

    /* Touch Controls is a non-motion alternative: IMU cannot fight its arrows. */
    if (state->controls) ax = ay = 0;
    /* A wider field needs more travel per tilt, with a shorter coast to avoid
     * racing past the next inward lane. The 60 Hz integrator is unchanged. */
    /* Specific force points uphill: right-side-down reads ax < 0; bottom-down
     * reads ay > 0. LVGL reverses only the measured screen-up y coordinate. */
    float vx = (state->game.marble.vx - ax * MARBLE_GRAVITY * STEP) * MARBLE_DAMPING;
    float vy = (state->game.marble.vy + ay * MARBLE_GRAVITY * STEP) * MARBLE_DAMPING;
    float speed = hypotf(vx, vy);
    if (speed > ORB_MARBLE_MAX_SPEED) {
        vx *= ORB_MARBLE_MAX_SPEED / speed; vy *= ORB_MARBLE_MAX_SPEED / speed;
    }
    state->game.marble.vx = vx;
    state->game.marble.vy = vy;
    state->game.marble.x += vx * STEP;
    state->game.marble.y += vy * STEP;
    const float edge = ORB_MARBLE_RADIUS - 12;
    marble_arc_t gate_arcs[ORB_MARBLE_GATES_MAX][ORB_MARBLE_GATE_SEGMENTS];
    size_t gate_counts[ORB_MARBLE_GATES_MAX];
    for (unsigned g = 0; g < ORB_MARBLE_GATES_MAX; ++g)
        gate_counts[g] = marble_gate_arcs(state, g, gate_arcs[g]);
    for (unsigned pass = 0; pass < 4; ++pass) {
        float length = hypotf(state->game.marble.x, state->game.marble.y);
        if (length > edge) {
            float nx = state->game.marble.x / length, ny = state->game.marble.y / length;
            state->game.marble.x = nx * edge; state->game.marble.y = ny * edge;
            reflect(&state->game.marble.vx, &state->game.marble.vy, -nx, -ny);
        }
        deflect_chrome(&state->game.marble.x, &state->game.marble.y,
            &state->game.marble.vx, &state->game.marble.vy, 12, ORB_GAME_MARBLE, state->controls);
        for (unsigned g = 0; g < ORB_MARBLE_GATES_MAX; ++g) {
            for (size_t i = 0; i < gate_counts[g]; ++i) {
                orb_game_point_t point = marble_arc_nearest(state->game.marble.gates[g].radius,
                    gate_arcs[g][i], state->game.marble.x, state->game.marble.y);
                float dx = state->game.marble.x - point.x, dy = state->game.marble.y - point.y;
                float distance = hypotf(dx, dy);
                if (distance >= MARBLE_GATE_CONTACT + 0.02f) continue;
                float nx = distance > 0.001f ? dx / distance : point.x / state->game.marble.gates[g].radius;
                float ny = distance > 0.001f ? dy / distance : point.y / state->game.marble.gates[g].radius;
                state->game.marble.x = point.x + nx * (MARBLE_GATE_CONTACT + 0.02f);
                state->game.marble.y = point.y + ny * (MARBLE_GATE_CONTACT + 0.02f);
                reflect(&state->game.marble.vx, &state->game.marble.vy, nx, ny);
            }
        }
    }
    if (!orb_game_position_clear(state->game.marble.x, state->game.marble.y, 12, state->kind, state->controls)) {
        /* Only an extreme external/test position can exhaust the bounded solve. */
        marble_spawn(state);
    }
    if (++state->game.marble.sample_tick >= 2) {
        state->game.marble.sample_tick = 0;
        marble_record(state);
        marble_trail(state);
    }
    if (hypotf(state->game.marble.x, state->game.marble.y) < 20) {
        state->game.marble.stage_cleared = true;
        state->game.marble.vx = state->game.marble.vy = 0;
        state->score += 100;
        burst(state, 0, 0, GREEN, 8, 18);
        if (state->game.marble.stages_cleared < state->game.marble.stage + 1)
            state->game.marble.stages_cleared = (uint8_t)(state->game.marble.stage + 1);
        unsigned k = state->game.marble.keepsake_count;
        int16_t plant_x, plant_y;
        if (k == ORB_MARBLE_KEEPSAKES_MAX) {
            /* A full garden still receives the new keepsake. Replace its oldest
             * planting in the same spot, preserving all other positions. */
            plant_x = state->game.marble.keepsakes[0].x;
            plant_y = state->game.marble.keepsakes[0].y;
            memmove(state->game.marble.keepsakes, state->game.marble.keepsakes + 1,
                (ORB_MARBLE_KEEPSAKES_MAX - 1) * sizeof(state->game.marble.keepsakes[0]));
            k = ORB_MARBLE_KEEPSAKES_MAX - 1;
        } else {
            float angle = (float)k * 2.39996323f;
            float radius = 26 + 22 * sqrtf((float)k);
            plant_x = (int16_t)lroundf(cosf(angle) * radius);
            plant_y = (int16_t)lroundf(sinf(angle) * radius);
            ++state->game.marble.keepsake_count;
        }
        state->game.marble.keepsakes[k] = (orb_marble_keepsake_t){
            (uint8_t)(state->game.marble.stage % 3), (uint8_t)(state->game.marble.stage + 1), plant_x, plant_y};
    }
}

static float orbit_radius(uint8_t ring)
{
    static const float radii[ORB_GAME_ORBIT_RINGS] = {
        ORB_GAME_ORBIT_R0, ORB_GAME_ORBIT_R1, ORB_GAME_ORBIT_R2};
    return radii[ring % ORB_GAME_ORBIT_RINGS];
}

static float orbit_comet_radius(const orb_game_state_t *state, unsigned index)
{
    float radius = state->game.orbit.comet_radius[index];
    return radius > 0 ? radius : orbit_radius(state->game.orbit.obstacles[index].ring);
}

static float orbit_comet_speed(const orb_game_state_t *state, unsigned index)
{
    float speed = state->game.orbit.comet_speed[index];
    return speed != 0 ? speed : 1.05f + fminf((float)state->score / 1600, 0.8f);
}

static void orbit_step(orb_game_state_t *state)
{
    /* The planet drifts slowly around its ring; comets overtake, stars drift
     * the other way and are collected when they meet the planet. */
    float previous_planet = state->game.orbit.angle;
    if (state->game.orbit.hit_grace) --state->game.orbit.hit_grace;
    state->game.orbit.angle = angle_wrap(state->game.orbit.angle + 0.35f * STEP);
    float planet_angle = state->game.orbit.angle;
    float planet_radius = orbit_radius(state->game.orbit.ring);
    float px = cosf(planet_angle) * planet_radius;
    float py = sinf(planet_angle) * planet_radius;
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i) {
        orb_game_obstacle_t *o = &state->game.orbit.obstacles[i];
        if (o->ring >= ORB_GAME_ORBIT_RINGS) continue;
        float speed = orbit_comet_speed(state, i);
        float previous = o->angle;
        o->angle = angle_wrap(previous + speed * STEP);
        /* Change lanes only across the far side of the planet, leaving at
         * least half a relative orbit of warning. A global angle wrap could
         * teleport a comet onto the player near the right-hand side. */
        float relative_before = angle_wrap(previous - previous_planet);
        float relative_after = angle_wrap(o->angle - planet_angle);
        if ((speed > 0.35f && relative_before < PI && relative_after >= PI) ||
            (speed < 0.35f && relative_before > PI && relative_after <= PI)) {
            o->passed = false;
            if (state->game.orbit.comet_radius[i] <= 0)
                o->ring = (uint8_t)(random_next(state) % ORB_GAME_ORBIT_RINGS);
        }
        if (state->game.orbit.comet_radius[i] <= 0 && o->ring != state->game.orbit.ring) continue;
        float r = orbit_comet_radius(state, i);
        float cx = cosf(o->angle) * r, cy = sinf(o->angle) * r;
        float dx = cx - px, dy = cy - py;
        float hit = (float)(ORB_GAME_ORBIT_PLANET + ORB_GAME_ORBIT_COMET);
        if (dx * dx + dy * dy <= hit * hit) {
            if (!o->passed) {
                o->passed = true;
                /* HIG §§6/8: one contact gets a readable, steady recovery
                 * state, never a strobe or several lost lives in one frame. */
                if (state->game.orbit.hit_grace) continue;
                state->game.orbit.hit_grace = ORB_GAME_ORBIT_HIT_GRACE;
                burst(state, px, py, RED, 6, 18);
                if (state->lives) --state->lives;
                if (!state->lives) {
                    state->game_over = true;
                    return; /* A terminal collision cannot also award a star. */
                }
            }
        } else if (!o->passed && ((speed > 0 && previous < planet_angle && o->angle >= planet_angle) ||
                   (speed < 0 && previous > planet_angle && o->angle <= planet_angle))) {
            o->passed = true; /* Passed behind the planet without a hit. */
        }
    }
    for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i) {
        orb_game_star_t *s = &state->game.orbit.stars[i];
        if (!s->active) {
            s->angle = (float)(random_next(state) % 628) / 100.0f;
            s->ring = (uint8_t)(random_next(state) % ORB_GAME_ORBIT_RINGS);
            s->active = true;
            continue;
        }
        s->angle = angle_wrap(s->angle - 0.5f * STEP);
        if (s->ring != state->game.orbit.ring) continue;
        float r = orbit_radius(s->ring) - 6;
        float sx = cosf(s->angle) * r, sy = sinf(s->angle) * r;
        /* The star's visible 14 px tips belong to its pickup area. A smaller
         * hidden radius made a visible brush past a star fail to collect. */
        float collect = (float)(ORB_GAME_ORBIT_PLANET + ORB_GAME_ORBIT_STAR);
        if ((sx - px) * (sx - px) + (sy - py) * (sy - py) <= collect * collect) {
            s->active = false;
            state->score += 10;
            burst(state, sx, sy, YELLOW, 6, 0);
        }
    }
}

static float snake_speed(const orb_game_state_t *state)
{
    /* Identical tilt/touch physics; food earns a gentle 78 -> 128 px/s ramp. */
    return fminf(128, 78 + (state->game.snake.length - 6) * 1.2f);
}

static float point_segment_distance(orb_game_point_t p, orb_game_point_t a, orb_game_point_t b)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float length2 = dx * dx + dy * dy;
    float t = length2 > .00001f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2 : 0;
    t = fminf(1, fmaxf(0, t));
    return hypotf(p.x - a.x - t * dx, p.y - a.y - t * dy);
}

static void snake_step(orb_game_state_t *state, float ax, float ay)
{
    orb_game_point_t head = state->game.snake.cells[0], old_head = head;
    float dx = -ax, dy = ay;
    bool steering = !state->controls && hypotf(dx, dy) > .045f;
    if (state->game.snake.touch_held) {
        dx = state->game.snake.finger.x - head.x;
        dy = state->game.snake.finger.y - head.y;
        steering = hypotf(dx, dy) > 8;
    }
    if (steering) {
        /* Absolute screen-frame direction, not a relative left/right action.
         * Returning level preserves the heading already reached. A bounded
         * rate prevents a single sample/finger jump reversing into the neck. */
        float delta = remainderf(atan2f(dy, dx) - state->game.snake.heading, TAU);
        float turn = ORB_GAME_SNAKE_TURN_RATE * STEP;
        state->game.snake.heading = angle_wrap(state->game.snake.heading +
            fminf(turn, fmaxf(-turn, delta)));
    }
    float speed = snake_speed(state);
    head.x += cosf(state->game.snake.heading) * speed * STEP;
    head.y += sinf(state->game.snake.heading) * speed * STEP;
    bool eat = point_segment_distance(state->game.snake.food,
        state->game.snake.cells[0], head) <= ORB_GAME_SNAKE_HEAD_RADIUS + 8;
    bool grace = state->game.snake.grace > 0;
    if (grace) --state->game.snake.grace;
    bool collision = hypotf(head.x, head.y) + ORB_GAME_SNAKE_HEAD_RADIUS > ORB_GAME_RADIUS;
    if (collision && !grace) { state->game_over = true; return; }
    state->game.snake.cells[0] = head;
    for (unsigned i = 1; i < state->game.snake.length; ++i) {
        orb_game_point_t ahead = state->game.snake.cells[i - 1];
        orb_game_point_t *body = &state->game.snake.cells[i];
        orb_game_point_t old_body = *body;
        float distance = hypotf(ahead.x - body->x, ahead.y - body->y);
        if (distance > ORB_GAME_SNAKE_SPACING) {
            body->x = ahead.x + (body->x - ahead.x) * ORB_GAME_SNAKE_SPACING / distance;
            body->y = ahead.y + (body->y - ahead.y) * ORB_GAME_SNAKE_SPACING / distance;
        }
        /* The hitbox is each visible round segment, not an invisible link
         * between scalloped discs. Sweep relative head/body motion so the
         * vacating tail remains legal and even a crossed contact cannot
         * tunnel. Attached neck segments intentionally overlap the head. */
        if (i >= 4 && !grace) {
            orb_game_point_t from = {old_head.x - old_body.x, old_head.y - old_body.y};
            orb_game_point_t to = {head.x - body->x, head.y - body->y};
            if (point_segment_distance((orb_game_point_t){0, 0}, from, to) <
                ORB_GAME_SNAKE_HEAD_RADIUS + ORB_GAME_SNAKE_BODY_RADIUS)
                collision = true;
        }
    }
    if (collision && !grace) { state->game_over = true; return; }
    if (state->game.snake.food_age < 24) ++state->game.snake.food_age;
    if (eat) {
        state->score += 10;
        burst(state, head.x, head.y, ORANGE, 6, 0);
        unsigned length = state->game.snake.length;
        if (length < ORB_GAME_SNAKE_MAX) {
            state->game.snake.cells[length] = state->game.snake.cells[length - 1];
            state->game.snake.length++;
        }
        state->game.snake.food_age = 0;
        if (state->game.snake.length == ORB_GAME_SNAKE_MAX) {
            state->game.snake.won = state->game_over = true;
        } else snake_food(state);
    }
}

static void pop_spawn(orb_game_state_t *state)
{
    for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
        orb_game_bubble_t *bubble = &state->game.pop.bubbles[i];
        if (bubble->active) continue;
        for (unsigned attempt = 0; attempt < 48; ++attempt) {
            float angle = random_unit(state) * TAU;
            float radius = 30 + random_unit(state) * 90;
            float x = cosf(angle) * radius, y = sinf(angle) * radius;
            if (!orb_game_position_clear(x, y, 24, ORB_GAME_POP, false)) continue;
            bool separated = true;
            for (unsigned n = 0; n < ORB_GAME_BUBBLES; ++n) {
                const orb_game_bubble_t *other = &state->game.pop.bubbles[n];
                if (other->active && hypotf(x - other->x, y - other->y) < 60) {
                    separated = false;
                    break;
                }
            }
            if (!separated) continue;
            float speed = 15 + fminf((float)state->score / 140, 16);
            *bubble = (orb_game_bubble_t){x, y, cosf(angle) * speed, sinf(angle) * speed, 24, true};
            return;
        }
        return; /* Crowded: retry next spawn interval without an overlap. */
    }
}

static void pop_step(orb_game_state_t *state)
{
    if (state->game.pop.time_left) --state->game.pop.time_left;
    if (!state->game.pop.time_left) {
        state->game_over = true;
        return; /* The timed result cannot also receive a last-frame miss. */
    }
    bool any_active = false;
    for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i)
        any_active |= state->game.pop.bubbles[i].active;
    /* Reward clearing the field with a new choice within 300 ms. Keeping the
     * normal cadence otherwise lets an unhurried player follow each bubble. */
    if (!any_active && state->game.pop.spawn_in > 18) state->game.pop.spawn_in = 18;
    if (state->game.pop.spawn_in) --state->game.pop.spawn_in;
    if (!state->game.pop.spawn_in) {
        pop_spawn(state);
        state->game.pop.spawn_in = 68;
    }
    for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
        orb_game_bubble_t *bubble = &state->game.pop.bubbles[i];
        if (!bubble->active) continue;
        /* The small perpendicular sway is gameplay drift, not a glow/trail.
         * It changes direction smoothly without a frame allocation or RNG. */
        float sway = sinf((float)state->ticks * 0.035f + (float)i) * 2.5f;
        float speed = hypotf(bubble->vx, bubble->vy);
        float nx = speed > 0 ? bubble->vx / speed : 0;
        float ny = speed > 0 ? bubble->vy / speed : 0;
        bubble->x += (bubble->vx - ny * sway) * STEP;
        bubble->y += (bubble->vy + nx * sway) * STEP;
        deflect_chrome(&bubble->x, &bubble->y, &bubble->vx, &bubble->vy,
                       bubble->radius, ORB_GAME_POP, false);
        if (hypotf(bubble->x, bubble->y) + bubble->radius >= ORB_GAME_RADIUS) {
            bubble->active = false;
            state->combo = 0;
            if (state->lives) --state->lives;
            if (!state->lives) state->game_over = true;
        }
    }
}

void orb_game_step(orb_game_state_t *state, float ax, float ay,
                   bool motion_valid, bool reduce_motion)
{
    if (!state) return;
    state->reduce_motion = reduce_motion;
    if (!state->started || state->user_paused) return;
    effect_step(state);
    if (state->game_over) return;
    /* HIG §§6/8: essential movement remains; sprite extraction suppresses
     * pulses, transient trails, particles and IMU lighting under Reduce Motion. */
    ax = motion_valid ? finite_unit(ax) : 0;
    ay = motion_valid ? finite_unit(ay) : 0;
    if (!reduce_motion) {
        state->light_x = (int8_t)lroundf(-3 + ax * 4);
        state->light_y = (int8_t)lroundf(-4 - ay * 4);
    } else {
        state->light_x = -3;
        state->light_y = -4;
    }
    ++state->ticks;
    switch (state->kind) {
    case ORB_GAME_MARBLE: marble_step(state, ax, ay); break;
    case ORB_GAME_BEADS: beads_step(state, ax, ay); break;
    case ORB_GAME_ORBIT: orbit_step(state); break;
    case ORB_GAME_SNAKE: snake_step(state, ax, ay); break;
    case ORB_GAME_POP: pop_step(state); break;
    default: break;
    }
}

static void pop_one(orb_game_state_t *state, unsigned index)
{
    orb_game_bubble_t *bubble = &state->game.pop.bubbles[index];
    burst(state, bubble->x, bubble->y, YELLOW, 6, bubble->radius);
    bubble->active = false;
    if (state->combo < 99) ++state->combo;
    state->game.pop.reward = (uint16_t)(10 * state->combo);
    state->score += state->game.pop.reward;
}

static void gameplay_action(orb_game_state_t *state, orb_game_action_t action, float x, float y)
{
    switch (state->kind) {
    case ORB_GAME_BEADS:
        if (action == ORB_GAME_STEER_START && state->controls && isfinite(x) && isfinite(y)) {
            state->game.beads.touch_x = x - ORB_GAME_CX;
            state->game.beads.touch_y = y - ORB_GAME_CY;
            state->game.beads.touch_held = true;
            state->game.beads.touch_quiet_ticks = 0;
        }
        break;
    case ORB_GAME_MARBLE:
        if (state->game.marble.stage_cleared) {
            if (action == ORB_GAME_PRIMARY) {
                if (state->game.marble.stage + 1 < ORB_MARBLE_STAGES)
                    marble_setup_stage(state, (uint8_t)(state->game.marble.stage + 1));
                else state->game_over = true;
            }
            break;
        }
        if (action == ORB_GAME_REWIND_START || action == ORB_GAME_REWIND) {
            if (!state->game.marble.rewind_active) {
                state->game.marble.sample_tick = 0;
                state->game.marble.rewind_ticks = 0;
                state->game.marble.trail_count = 0;
            }
            state->game.marble.rewind_active = true;
            state->game.marble.vx = state->game.marble.vy = 0;
            break;
        }
        if (state->game.marble.rewind_active) break;
        if (action == ORB_GAME_LEFT) state->game.marble.vx -= 45;
        if (action == ORB_GAME_RIGHT) state->game.marble.vx += 45;
        if (action == ORB_GAME_UP) state->game.marble.vy -= 45;
        if (action == ORB_GAME_DOWN) state->game.marble.vy += 45;
        if (action == ORB_GAME_TAP && isfinite(x) && isfinite(y)) {
            float dx = x - ORB_GAME_CX - state->game.marble.x;
            float dy = y - ORB_GAME_CY - state->game.marble.y;
            float distance = hypotf(dx, dy);
            if (distance > 1) {
                state->game.marble.vx += dx / distance * 45;
                state->game.marble.vy += dy / distance * 45;
            }
        }
        break;
    case ORB_GAME_ORBIT:
        if (action == ORB_GAME_PRIMARY || action == ORB_GAME_TAP ||
            action == ORB_GAME_LEFT || action == ORB_GAME_RIGHT)
        {
            state->game.orbit.previous_radius = orbit_radius(state->game.orbit.ring);
            state->game.orbit.ring = (uint8_t)((state->game.orbit.ring + 1) % ORB_GAME_ORBIT_RINGS);
            state->game.orbit.trail_age = 12;
        }
        break;
    case ORB_GAME_SNAKE:
        if (action == ORB_GAME_STEER_START && isfinite(x) && isfinite(y)) {
            state->game.snake.finger = (orb_game_point_t){x - ORB_GAME_CX, y - ORB_GAME_CY};
            state->game.snake.touch_held = true;
        }
        break;
    case ORB_GAME_POP: {
        int selected = -1;
        float best = 1e9f;
        for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
            orb_game_bubble_t *bubble = &state->game.pop.bubbles[i];
            if (!bubble->active) continue;
            if (action == ORB_GAME_PRIMARY) {
                float distance = hypotf(bubble->x, bubble->y);
                if (distance < best) { best = distance; selected = (int)i; }
            } else if (action == ORB_GAME_TAP && isfinite(x) && isfinite(y)) {
                float distance = hypotf(x - ORB_GAME_CX - bubble->x, y - ORB_GAME_CY - bubble->y);
                if (distance <= bubble->radius + 12 && distance < best) {
                    best = distance;
                    selected = (int)i;
                }
            }
        }
        if (selected >= 0) pop_one(state, (unsigned)selected);
        break;
    }
    default: break;
    }
}

void orb_game_action(orb_game_state_t *state, orb_game_action_t action, float x, float y)
{
    if (!state) return;
    if (state->kind == ORB_GAME_BEADS && action == ORB_GAME_STEER_STOP) {
        if (state->game.beads.touch_held) state->game.beads.touch_quiet_ticks = 0;
        state->game.beads.touch_held = false;
        return;
    }
    if (state->kind == ORB_GAME_SNAKE && action == ORB_GAME_STEER_STOP) {
        state->game.snake.touch_held = false;
        return;
    }
    if (state->kind == ORB_GAME_MARBLE && action == ORB_GAME_REWIND_STOP) {
        marble_stop_rewind(state);
        return;
    }
    if (state->kind == ORB_GAME_MARBLE && action == ORB_GAME_GARDEN) {
        if (!state->started || state->game_over || state->user_paused)
            state->game.marble.garden_open = !state->game.marble.garden_open;
        return;
    }
    if (action == ORB_GAME_RESTART) {
        bool controls = state->controls;
        uint8_t keep_count = state->kind == ORB_GAME_MARBLE ? state->game.marble.keepsake_count : 0;
        uint8_t stages_cleared = state->kind == ORB_GAME_MARBLE ? state->game.marble.stages_cleared : 0;
        orb_marble_keepsake_t keeps[ORB_MARBLE_KEEPSAKES_MAX];
        if (state->kind == ORB_GAME_MARBLE)
            memcpy(keeps, state->game.marble.keepsakes, sizeof(keeps));
        orb_game_init(state, state->kind, state->random);
        state->controls = controls;
        if (state->kind == ORB_GAME_MARBLE) {
            state->game.marble.keepsake_count = keep_count;
            state->game.marble.stages_cleared = stages_cleared;
            memcpy(state->game.marble.keepsakes, keeps, sizeof(keeps));
        }
        return;
    }
    if (action == ORB_GAME_START) {
        state->started = true;
        if (state->kind == ORB_GAME_MARBLE) state->game.marble.garden_open = false;
        return;
    }
    if (action == ORB_GAME_CONTROLS) {
        if (state->kind == ORB_GAME_BEADS) {
            state->game.beads.touch_held = false;
            state->game.beads.touch_quiet_ticks = state->game.beads.settle_ticks = 0;
            state->game.beads.settled = false;
        }
        if (state->kind == ORB_GAME_MARBLE) marble_stop_rewind(state);
        state->controls = !state->controls;
        if (state->kind == ORB_GAME_SNAKE) {
            state->game.snake.touch_held = false;
        }
        if (state->kind == ORB_GAME_MARBLE) {
            if (!orb_game_position_clear(state->game.marble.x, state->game.marble.y, 12, state->kind, state->controls)) {
                marble_spawn(state);
            }
            /* A control-mode geometry change starts a new truthful path. */
            state->game.marble.history_head = state->game.marble.history_count = 0;
            state->game.marble.sample_tick = 0;
            marble_record(state);
        }
        return;
    }
    if (action == ORB_GAME_PAUSE) {
        if (state->kind == ORB_GAME_BEADS) {
            state->game.beads.touch_held = false;
            state->game.beads.touch_quiet_ticks = 0;
        }
        if (state->kind == ORB_GAME_MARBLE) marble_stop_rewind(state);
        if (state->kind == ORB_GAME_SNAKE) state->game.snake.touch_held = false;
        if (state->started && !state->game_over) state->user_paused = !state->user_paused;
        return;
    }
    if (action == ORB_GAME_FINISH) {
        /* Ending a round is an explicit paused-screen decision that retains
         * the score and already-planted keepsakes. */
        if (state->started && !state->game_over && state->user_paused) {
            if (state->kind == ORB_GAME_MARBLE) marble_stop_rewind(state);
            state->game_over = true;
            state->user_paused = false;
        }
        return;
    }
    if (state->game_over || !state->started || state->user_paused) return;
    gameplay_action(state, action, x, y);
}

static orb_game_sprite_t sprite(float x, float y, uint16_t radius, uint32_t colour, uint8_t shape)
{
    return (orb_game_sprite_t){.x = ORB_GAME_CX + x, .y = ORB_GAME_CY + y,
        .radius = radius, .colour = colour, .shape = shape, .opacity = 255};
}

#define EMIT(value) do { if (count < capacity) out[count] = (value); ++count; } while (0)

static size_t marble_sprites(const orb_game_state_t *state, orb_game_sprite_t *out,
                              size_t capacity, size_t count)
{
    bool garden = state->game.marble.garden_open;
    orb_game_sprite_t marble = sprite(state->game.marble.x, state->game.marble.y,
        garden ? 0 : ORB_MARBLE_BALL_RADIUS, WHITE, ORB_GAME_MARBLE_SPRITE);
    marble.light_x = state->light_x; marble.light_y = state->light_y;
    EMIT(marble);
    EMIT(sprite(0, 0, garden ? 0 : ORB_MARBLE_FLOWER_RADIUS, GREEN, ORB_GAME_FLOWER));
    for (unsigned g = 0; g < ORB_MARBLE_GATES_MAX; ++g) {
        marble_arc_t arcs[ORB_MARBLE_GATE_SEGMENTS];
        size_t arc_count = garden ? 0 : marble_gate_arcs(state, g, arcs);
        for (unsigned i = 0; i < ORB_MARBLE_GATE_SEGMENTS; ++i) {
            orb_game_sprite_t arc = sprite(0, 0,
                i < arc_count ? ORB_MARBLE_GATE_WIDTH / 2 : 0, 0x3a3a3c, ORB_GAME_GATE);
            if (i < arc_count) {
                arc.arc_radius = state->game.marble.gates[g].radius;
                arc.arc_start = arcs[i].start;
                arc.arc_end = arcs[i].end;
            }
            EMIT(arc);
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        orb_game_point_t point = state->game.marble.trail[i];
        bool active = !garden && !state->reduce_motion &&
            i < state->game.marble.trail_count &&
            orb_game_position_clear(point.x, point.y, 4, state->kind, state->controls);
        orb_game_sprite_t trail = sprite(point.x, point.y, active ? (uint16_t)(4 - (i + 1) / 2) : 0, WHITE, ORB_GAME_DISC);
        unsigned fade = state->game.marble.rewind_active ? state->game.marble.rewind_ticks / 3 : 0;
        if (fade > 40) fade = 40;
        trail.opacity = (uint8_t)(87 - i * 20 - fade);
        EMIT(trail);
    }
    for (unsigned k = 0; k < ORB_MARBLE_KEEPSAKES_MAX; ++k) {
        orb_marble_keepsake_t item = state->game.marble.keepsakes[k];
        bool active = garden && k < state->game.marble.keepsake_count;
        EMIT(sprite(item.x, item.y, active ? 13 : 0,
            item.kind == 0 ? 0x8e8e93 : GREEN,
            item.kind == 0 ? ORB_GAME_STONE : ORB_GAME_FLOWER));
    }
    return count;
}

static size_t beads_sprites(const orb_game_state_t *state, orb_game_sprite_t *out,
                              size_t capacity, size_t count)
{
    /* HIG §§1/5/9: fixed identity tones plus a small flat reflection keep
     * the particles individual without gradients, glass or glow. */
    for (unsigned i = 0; i < state->game.beads.count; ++i) {
        const orb_game_bead_t *b = &state->game.beads.beads[i];
        unsigned tone = ((i * 1664525u + 1013904223u) >> 16) % 30u;
        uint32_t colour = ((226u + tone) << 16) | ((49u + tone / 5u) << 8) | (84u + tone / 3u);
        orb_game_sprite_t bead = sprite(b->x, b->y, ORB_GAME_BEAD_DRAW_RADIUS,
                                        colour, ORB_GAME_BEAD);
        if (state->game.beads.settled) bead.opacity = state->game.beads.target_opacity[i];
        EMIT(bead);
    }
    return count;
}

static size_t orbit_sprites(const orb_game_state_t *state, orb_game_sprite_t *out,
                              size_t capacity, size_t count)
{
    float radius = orbit_radius(state->game.orbit.ring);
    float pa = state->game.orbit.angle;
    /* A ring switch leaves four short-lived separate indigo discs. */
    for (unsigned i = 0; i < 4; ++i) {
        float t = (float)(i + 1) / 5;
        float trail_radius = state->game.orbit.previous_radius +
            (radius - state->game.orbit.previous_radius) * t;
        orb_game_sprite_t trail = sprite(cosf(pa) * trail_radius, sinf(pa) * trail_radius,
            state->game.orbit.trail_age && !state->reduce_motion ? (uint16_t)(3 + i) : 0,
            INDIGO, ORB_GAME_DISC);
        trail.opacity = (uint8_t)(state->game.orbit.trail_age * (10 + i * 2));
        EMIT(trail);
    }
    /* The indigo planet, with two quiet dim discs trailing its motion. */
    float px = cosf(pa) * radius, py = sinf(pa) * radius;
    EMIT(sprite(px, py, ORB_GAME_ORBIT_PLANET, INDIGO,
        state->game.orbit.hit_grace ? ORB_GAME_RING : ORB_GAME_DISC));
    /* HIG §§1/6: trails describe the previous curved path, rather than a
     * straight tangent that points away from the orbital motion. */
    orb_game_sprite_t ptrail1 = sprite(cosf(pa - .12217f) * (radius - 3),
        sinf(pa - .12217f) * (radius - 3), 6, INDIGO, ORB_GAME_DISC);
    ptrail1.opacity = 115;
    EMIT(ptrail1);
    orb_game_sprite_t ptrail2 = sprite(cosf(pa - .22689f) * (radius - 4),
        sinf(pa - .22689f) * (radius - 4), 4, INDIGO, ORB_GAME_DISC);
    ptrail2.opacity = 64;
    EMIT(ptrail2);
    for (unsigned i = 0; i < ORB_GAME_OBSTACLES; ++i) {
        orb_game_obstacle_t obstacle = state->game.orbit.obstacles[i];
        float cr = orbit_comet_radius(state, i);
        float ca = obstacle.angle;
        float direction = orbit_comet_speed(state, i) < 0 ? -1 : 1;
        bool active = obstacle.ring < ORB_GAME_ORBIT_RINGS;
        EMIT(sprite(cosf(ca) * cr, sinf(ca) * cr,
            active ? ORB_GAME_ORBIT_COMET : 0, ORANGE, ORB_GAME_DISC));
        float tail = ca - direction * 17.4f / cr;
        orb_game_sprite_t c1 = sprite(cosf(tail) * (cr - 1), sinf(tail) * (cr - 1),
            active ? 5 : 0, ORANGE, ORB_GAME_DISC);
        c1.opacity = 115;
        EMIT(c1);
        tail = ca - direction * 32.4f / cr;
        orb_game_sprite_t c2 = sprite(cosf(tail) * (cr - 1.5f), sinf(tail) * (cr - 1.5f),
            active ? 4 : 0, ORANGE, ORB_GAME_DISC);
        c2.opacity = 64;
        EMIT(c2);
    }
    for (unsigned i = 0; i < ORB_GAME_ORBIT_STARS; ++i) {
        orb_game_star_t star = state->game.orbit.stars[i];
        if (!star.active) continue;
        float sr = orbit_radius(star.ring) - 6;
        EMIT(sprite(cosf(star.angle) * sr, sinf(star.angle) * sr,
                    ORB_GAME_ORBIT_STAR, YELLOW, ORB_GAME_STAR));
    }
    return count;
}

static size_t snake_sprites(const orb_game_state_t *state, orb_game_sprite_t *out,
                              size_t capacity, size_t count)
{
    for (unsigned i = 0; i < ORB_GAME_SNAKE_MAX; ++i) {
        orb_game_sprite_t segment = sprite(0, 0, 0, ORANGE, ORB_GAME_DISC);
        if (i < state->game.snake.length) {
            orb_game_point_t p = state->game.snake.cells[i];
            segment = sprite(p.x, p.y, ORB_GAME_SNAKE_BODY_RADIUS,
                i ? GREEN : WHITE, i ? ORB_GAME_DISC : ORB_GAME_HEAD);
            if (!i) {
                segment.radius = ORB_GAME_SNAKE_HEAD_RADIUS;
                segment.light_x = (int8_t)lroundf(cosf(state->game.snake.heading) * 5);
                segment.light_y = (int8_t)lroundf(sinf(state->game.snake.heading) * 5);
            }
        }
        EMIT(segment);
    }
    orb_game_sprite_t food = sprite(state->game.snake.food.x,
        state->game.snake.food.y, 8, ORANGE, ORB_GAME_DISC);
    EMIT(food);
    /* HIG §§5/6: Gray2 keeps the lethal edge above 3:1 on black;
     * orange warns as the head nears the same visible boundary. */
    float edge = hypotf(state->game.snake.cells[0].x, state->game.snake.cells[0].y);
    for (unsigned i = 0; i < 8; ++i) {
        orb_game_sprite_t wall = sprite(0, 0, 1, edge > 160 ? ORANGE : 0x636366u, ORB_GAME_GATE);
        wall.arc_radius = ORB_GAME_RADIUS - 1; wall.opacity = edge > 160 ? 150 : 255;
        wall.arc_start = (float)i * TAU / 8; wall.arc_end = (float)(i + 1) * TAU / 8;
        EMIT(wall);
    }
    return count;
}

static size_t pop_sprites(const orb_game_state_t *state, orb_game_sprite_t *out,
                              size_t capacity, size_t count)
{
    for (unsigned i = 0; i < ORB_GAME_BUBBLES; ++i) {
        orb_game_bubble_t bubble = state->game.pop.bubbles[i];
        orb_game_sprite_t target = sprite(bubble.x, bubble.y,
            bubble.active ? bubble.radius : 0, YELLOW, ORB_GAME_BUBBLE);
        target.light_x = state->light_x;
        target.light_y = state->light_y;
        EMIT(target);
    }
    return count;
}

size_t orb_game_sprites(const orb_game_state_t *state, orb_game_sprite_t *out, size_t capacity)
{
    if (!state || !out || (unsigned)state->kind >= ORB_GAME_COUNT) return 0;
    size_t count = 0;
    switch (state->kind) {
    case ORB_GAME_MARBLE: count = marble_sprites(state, out, capacity, count); break;
    case ORB_GAME_BEADS: count = beads_sprites(state, out, capacity, count); break;
    case ORB_GAME_ORBIT: count = orbit_sprites(state, out, capacity, count); break;
    case ORB_GAME_SNAKE: count = snake_sprites(state, out, capacity, count); break;
    case ORB_GAME_POP: count = pop_sprites(state, out, capacity, count); break;
    default: break;
    }
    const orb_game_effect_t *effect = &state->effect;
    bool active = effect->duration && effect->age < effect->duration &&
        !(state->kind == ORB_GAME_MARBLE && state->game.marble.garden_open);
    for (unsigned i = 0; i < ORB_GAME_PARTICLES; ++i) {
        float t = effect->duration ? (float)effect->age / effect->duration : 1;
        float angle = (float)i * TAU / (effect->count ? effect->count : 1);
        float distance = 6 + t * 32;
        float x = effect->x + cosf(angle) * distance;
        float y = effect->y + sinf(angle) * distance + t * t * 9;
        uint16_t radius = active && !state->reduce_motion && state->kind != ORB_GAME_POP && i < effect->count ?
            (uint16_t)(3 - (unsigned)(t * 2)) : 0;
        if (hypotf(x, y) + radius > ORB_GAME_RADIUS - 1) radius = 0;
        if (!orb_game_position_clear(x, y, radius, state->kind, state->controls)) radius = 0;
        if (state->kind == ORB_GAME_POP) {
            for (unsigned n = 0; n < ORB_GAME_BUBBLES; ++n) {
                const orb_game_bubble_t *bubble = &state->game.pop.bubbles[n];
                if (bubble->active && hypotf(x - bubble->x, y - bubble->y) < radius + bubble->radius + 4)
                    radius = 0;
            }
        }
        orb_game_sprite_t particle = sprite(x, y, radius, effect->colour, ORB_GAME_PARTICLE);
        particle.opacity = (uint8_t)(255 * (1 - t));
        EMIT(particle);
    }
    /* HIG §§6/8: Reduce Motion retains the event's meaning as a stationary
     * fading mark. It removes particle travel and spatial ring expansion. */
    uint16_t ring_radius = 0;
    if (active && state->kind != ORB_GAME_POP) {
        if (state->reduce_motion) ring_radius = effect->ring_radius ? effect->ring_radius : 14;
        else if (effect->ring_radius) ring_radius = (uint16_t)(effect->ring_radius + effect->age);
    }
    float available = ORB_GAME_RADIUS - hypotf(effect->x, effect->y) - 1;
    if (state->kind == ORB_GAME_POP) {
        for (unsigned n = 0; n < ORB_GAME_BUBBLES; ++n) {
            const orb_game_bubble_t *bubble = &state->game.pop.bubbles[n];
            if (bubble->active)
                available = fminf(available, hypotf(effect->x - bubble->x, effect->y - bubble->y) - bubble->radius - 8);
        }
    }
    if ((float)ring_radius > available) ring_radius = (uint16_t)fmaxf(0, floorf(available));
    /* Cap the bounded response instead of clipping it at the panel or drawing
     * it across the next target. Opacity still completes its short decay. */
    while (ring_radius && !orb_game_position_clear(effect->x, effect->y, ring_radius, state->kind, state->controls))
        --ring_radius;
    orb_game_sprite_t ring = sprite(effect->x, effect->y, ring_radius, effect->colour, ORB_GAME_RING);
    ring.opacity = active ? (uint8_t)(255 * (effect->duration - effect->age) / effect->duration) : 0;
    EMIT(ring);
    /* Design 1.0: a popped bubble leaves five short flat rays. Essential
     * stationary feedback remains with Reduce Motion; there is no glow. */
    if (state->kind == ORB_GAME_POP) {
        uint16_t spark_radius = active ? (uint16_t)fmaxf(0, fminf(36, floorf(available))) : 0;
        while (spark_radius && !orb_game_position_clear(effect->x, effect->y,
                spark_radius, state->kind, state->controls)) --spark_radius;
        orb_game_sprite_t spark = sprite(effect->x, effect->y, spark_radius,
                                         WHITE, ORB_GAME_SPARK);
        spark.opacity = active ? (uint8_t)(255 * (effect->duration - effect->age) / effect->duration) : 0;
        EMIT(spark);
    }
#undef EMIT
    return count < capacity ? count : capacity;
}
