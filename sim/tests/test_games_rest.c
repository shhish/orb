/* Synthetic sensor traces use production gravity, quiet detection and physics.
 * Sensor time stays independent of the 60 Hz integrator; no trace advances
 * desk stillness by repeating a sample during fixed-step catch-up. */
#include "orb_game_logic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define PI 3.1415926535897932385f
#define RAD (PI / 180.0f)

static unsigned s_checks;
static FILE *s_csv;
#define CHECK(condition) do { ++s_checks; if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

typedef struct {
    orb_game_tilt_t tilt;
    orb_game_state_t game;
    orb_game_point_t gravity;
    uint32_t cadence_us, step_us, elapsed_us, random;
} rest_trace_t;

typedef struct {
    float mean_x, mean_y, mean_speed, rms_speed, low_fraction, p95_speed, worst_speed;
} pile_stats_t;

static uint32_t trace_random(rest_trace_t *trace)
{
    trace->random ^= trace->random << 13;
    trace->random ^= trace->random >> 17;
    trace->random ^= trace->random << 5;
    return trace->random;
}

static float signed_noise(rest_trace_t *trace)
{
    return (float)(trace_random(trace) & 0xffffu) / 32768.0f - 1;
}

static void init_trace(rest_trace_t *trace, uint32_t cadence_us, bool clock)
{
    *trace = (rest_trace_t){.cadence_us = cadence_us, .random = 0x68616e64u};
    orb_game_init(&trace->game, ORB_GAME_BEADS, 0x72657374u);
    if (clock) orb_game_beads_set_time(&trace->game, 10, 9);
}

static orb_game_point_t sample(rest_trace_t *trace, const float force[3], const float *gyro)
{
    orb_game_point_t output;
    bool valid = orb_game_gravity_update_motion(&trace->tilt, force[0], force[1], force[2],
        gyro, trace->cadence_us, &output.x, &output.y);
    orb_game_set_motion(&trace->game, &trace->tilt);
    trace->gravity = output;
    if (valid) {
        trace->step_us += trace->cadence_us;
        while (trace->step_us >= ORB_GAME_STEP_US) {
            orb_game_step(&trace->game, output.x, output.y, true, false);
            trace->step_us -= ORB_GAME_STEP_US;
        }
    } else trace->step_us = 0;
    trace->elapsed_us += trace->cadence_us;
    CHECK(isfinite(output.x) && isfinite(output.y));
    CHECK(hypotf(output.x, output.y) <= 1.001f);
    return output;
}

static bool exact_clock(const orb_game_state_t *state)
{
    if (!state->game.beads.settled || state->game.beads.travel_ticks < 45) return false;
    for (unsigned i = 0; i < state->game.beads.count; ++i) {
        const orb_game_bead_t *bead = &state->game.beads.beads[i];
        if (bead->x != state->game.beads.target_x[i] ||
            bead->y != state->game.beads.target_y[i] || bead->vx || bead->vy) return false;
    }
    return true;
}

static int float_order(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static pile_stats_t pile_stats(const orb_game_state_t *state, float side)
{
    pile_stats_t stats = {0};
    unsigned count = state->game.beads.count;
    CHECK(count > 0);
    float speeds[ORB_GAME_BEAD_COUNT];
    for (unsigned i = 0; i < count; ++i) {
        const orb_game_bead_t *bead = &state->game.beads.beads[i];
        CHECK(isfinite(bead->x) && isfinite(bead->y) && isfinite(bead->vx) && isfinite(bead->vy));
        CHECK(hypotf(bead->x, bead->y) <= (state->game.beads.settled ?
            233.0f : ORB_GAME_RADIUS) - ORB_GAME_BEAD_RADIUS + 0.01f);
        stats.mean_x += bead->x;
        stats.mean_y += bead->y;
        float speed2 = bead->vx * bead->vx + bead->vy * bead->vy;
        speeds[i] = sqrtf(speed2);
        stats.mean_speed += speeds[i];
        stats.worst_speed = fmaxf(stats.worst_speed, speeds[i]);
        stats.rms_speed += speed2;
        if (bead->x * side > 120 && hypotf(bead->x, bead->y) > 160) stats.low_fraction += 1;
    }
    qsort(speeds, count, sizeof(speeds[0]), float_order);
    stats.p95_speed = speeds[(count - 1u) * 95u / 100u];
    stats.mean_x /= (float)count;
    stats.mean_y /= (float)count;
    stats.mean_speed /= (float)count;
    stats.rms_speed = sqrtf(stats.rms_speed / (float)count);
    stats.low_fraction /= (float)count;
    return stats;
}

static void record(const char *phase, const rest_trace_t *trace, float ax)
{
    pile_stats_t stats = pile_stats(&trace->game, ax > 0 ? -1 : 1);
    orb_game_point_t force = orb_game_beads_force(ax, trace->gravity.y);
    if (s_csv) fprintf(s_csv, "%s,%.3f,%.6f,%.3f,%.3f,%.5f,%.5f,%.4f,%u,%u,%.5f,%.3f,%.6f,%.3f\n",
        phase, (double)trace->elapsed_us / 1000000, (double)ax,
        (double)stats.mean_x, (double)stats.mean_y, (double)stats.mean_speed,
        (double)stats.rms_speed, (double)stats.low_fraction,
        trace->game.game.beads.settled ? 1u : 0u, trace->tilt.still_us,
        (double)(asinf(fminf(1, fmaxf(-1, ax))) / RAD), (double)force.x,
        (double)trace->gravity.y, (double)force.y);
}

static orb_game_point_t hand_sample(rest_trace_t *trace, float degrees);

static void response_sweeps(void)
{
    const float angles[] = {0.5f, 1, 2, 5, 10, 20, 35, 60, 90};
    float previous = 0;
    for (unsigned a = 0; a < sizeof(angles) / sizeof(angles[0]); ++a) {
        float angle = angles[a], gravity = sinf(angle * RAD);
        orb_game_point_t force = orb_game_beads_force(gravity, 0);
        float acceleration = -force.x;
        CHECK(acceleration > previous && acceleration < 1850);
        CHECK(fabsf(force.y) < 0.00001f);
        orb_game_point_t diagonal = orb_game_beads_force(gravity * 0.70710678f, gravity * 0.70710678f);
        CHECK(fabsf(hypotf(diagonal.x, diagonal.y) - acceleration) < 0.001f);
        /* Log the actual first step too, after both damping applications.
         * No collision or velocity cap can hide upstream attenuation here. */
        rest_trace_t isolated;
        init_trace(&isolated, 10000, false);
        isolated.game.game.beads.count = 1;
        isolated.game.game.beads.beads[0] = (orb_game_bead_t){0};
        orb_game_point_t output = hand_sample(&isolated, angle);
        orb_game_step(&isolated.game, output.x, output.y, true, false);
        float actual = -isolated.game.game.beads.beads[0].vx * 60;
        CHECK(fabsf(actual - acceleration * 0.9962531f) < 0.02f);
        printf("beads force: %.1f deg, %.6f g, %.3f px/s² (%.3f px/s²/deg), integrated %.3f px/s²\n",
            (double)angle, (double)output.x, (double)acceleration,
            (double)(acceleration / angle), (double)actual);
        previous = acceleration;
    }
    for (unsigned a = 1; a <= 4; ++a) {
        const float sweep_angles[] = {0, 2, 5, 10, 20};
        rest_trace_t trace;
        init_trace(&trace, 10000, false);
        float angle = sweep_angles[a];
        for (unsigned i = 0; i < 300; ++i) {
            orb_game_point_t output = hand_sample(&trace, angle);
            if (i % 10 == 9) record("gain-left", &trace, output.x);
        }
        pile_stats_t left = pile_stats(&trace.game, -1);
        for (unsigned i = 0; i < 300; ++i) {
            orb_game_point_t output = hand_sample(&trace, -angle);
            if (i % 10 == 9) record("gain-reversal", &trace, output.x);
        }
        pile_stats_t right = pile_stats(&trace.game, 1);
        printf("beads gain sweep: +/-%.0f deg 3 s each, centroids %.3f -> %.3f px\n",
            (double)angle, (double)left.mean_x, (double)right.mean_x);
        CHECK(left.mean_x < -115 && right.mean_x > 115);
    }
}

/* A hand holding one angle is not a stand: deterministic small gyro jitter
 * keeps it in fluid play while gravity itself remains a constant held tilt. */
static orb_game_point_t hand_sample(rest_trace_t *trace, float degrees)
{
    const float force[3] = {sinf(degrees * RAD), 0, cosf(degrees * RAD)};
    float gyro[3] = {0.8f * signed_noise(trace), 0.8f * signed_noise(trace),
        0.8f * signed_noise(trace)};
    return sample(trace, force, gyro);
}

static void pour_left_right(void)
{
    rest_trace_t trace;
    init_trace(&trace, 10000, true);
    float x_before[ORB_GAME_BEAD_COUNT];
    for (unsigned i = 0; i < 300; ++i) {
        orb_game_point_t output = hand_sample(&trace, 35);
        CHECK(fabsf(output.x - sinf(35 * RAD)) < 0.00001f);
        CHECK(!trace.game.game.beads.settled);
        if (i % 10 == 9) record("pour-left", &trace, output.x);
    }
    pile_stats_t left = pile_stats(&trace.game, -1);
    CHECK(left.mean_x < -115 && left.low_fraction > 0.35f);
    for (unsigned i = 0; i < trace.game.game.beads.count; ++i)
        x_before[i] = trace.game.game.beads.beads[i].x;
    unsigned crossed = 0;
    for (unsigned i = 0; i < 300; ++i) {
        orb_game_point_t output = hand_sample(&trace, -35);
        CHECK(fabsf(output.x + sinf(35 * RAD)) < 0.00001f);
        CHECK(!trace.game.game.beads.settled);
        if (i % 10 == 9) record("pour-right", &trace, output.x);
    }
    for (unsigned i = 0; i < trace.game.game.beads.count; ++i) {
        const orb_game_bead_t *bead = &trace.game.game.beads.beads[i];
        if (bead->x > 30 && x_before[i] < -30) ++crossed;
    }
    pile_stats_t right = pile_stats(&trace.game, 1);
    CHECK(right.mean_x > 115 && right.low_fraction > 0.35f);
    CHECK(crossed > trace.game.game.beads.count * 95u / 100u);
    printf("beads pour: left 3 s centroid %.2f px, %.1f%% at rim; right 3 s centroid %.2f px, %.1f%% at rim, %u/%u cross\n",
        (double)left.mean_x, (double)(left.low_fraction * 100), (double)right.mean_x,
        (double)(right.low_fraction * 100), crossed, trace.game.game.beads.count);
}

static void level_sweep(void)
{
    rest_trace_t trace;
    init_trace(&trace, 10000, false);
    /* Sparse, noncontact particles isolate fine gravity from legitimate
     * contact rest. Every force lies inside the former 0.02 g freeze zone. */
    trace.game.game.beads.count = 7;
    for (unsigned i = 0; i < trace.game.game.beads.count; ++i)
        trace.game.game.beads.beads[i] = (orb_game_bead_t){-60, -60 + (float)i * 20, 30, 0};
    float previous_vx = 30, minimum_speed = 1000;
    for (unsigned i = 0; i <= 200; ++i) {
        float degrees = -0.5f + (float)i / 200;
        orb_game_point_t output = hand_sample(&trace, degrees);
        CHECK(fabsf(output.x - sinf(degrees * RAD)) < 0.00001f);
        const orb_game_bead_t *bead = &trace.game.game.beads.beads[0];
        if (fabsf(degrees) < 0.1f) {
            CHECK(fabsf(bead->vx) > 0.5f);
            minimum_speed = fminf(minimum_speed, fabsf(bead->vx));
        }
        CHECK(fabsf(bead->vx - previous_vx) < 2);
        previous_vx = bead->vx;
        if (i % 10 == 0) record("level-sweep", &trace, output.x);
    }
    CHECK(trace.game.game.beads.beads[0].x > -50);
    printf("beads sweep: -0.5 to +0.5 deg through level, minimum central speed %.4f px/s; no zero-force plateau\n",
        (double)minimum_speed);
}

static unsigned wait_for_clock(rest_trace_t *trace, const float pose[3], unsigned max_us)
{
    unsigned lock_us = 0;
    for (unsigned i = 1; i <= max_us / trace->cadence_us; ++i) {
        float noise = (float)((int)(i % 7) - 3) * 0.00004f;
        const float force[3] = {pose[0] + noise, pose[1] - noise, pose[2] + noise * 0.5f};
        const float gyro[3] = {0.02f * sinf((float)i), 0.01f * cosf((float)i), 0};
        orb_game_point_t output = sample(trace, force, gyro);
        if (!lock_us && exact_clock(&trace->game)) lock_us = i * trace->cadence_us;
        if (i % 10 == 0) record("desk-settle", trace, output.x);
    }
    CHECK(lock_us > 0 && lock_us <= max_us);
    CHECK(exact_clock(&trace->game));
    return lock_us;
}

static void desk_settle_and_pickup(void)
{
    const float angles[] = {0, 35, 85, 120};
    const uint32_t cadences[] = {5000, 10000, 20000};
    for (unsigned a = 0; a < sizeof(angles) / sizeof(angles[0]); ++a) {
        for (unsigned c = 0; c < sizeof(cadences) / sizeof(cadences[0]); ++c) {
            rest_trace_t trace;
            init_trace(&trace, cadences[c], true);
            const float pose[3] = {0, 0.925f * sinf(angles[a] * RAD),
                0.925f * cosf(angles[a] * RAD)};
            unsigned lock_us = wait_for_clock(&trace, pose, 2000000);
            CHECK(!trace.tilt.moving);
            for (unsigned i = 0; i < 1000000u / trace.cadence_us; ++i) {
                const float gyro[3] = {0, 0, 0};
                (void)sample(&trace, pose, gyro);
                CHECK(exact_clock(&trace.game));
            }
            /* Yaw can pick up the device while leaving acceleration unchanged. */
            const float pickup[3] = {0, 0, 12};
            for (unsigned i = 0; i < 10; ++i) (void)sample(&trace, pose, pickup);
            CHECK(trace.tilt.moving && !trace.game.game.beads.settled);
            printf("beads desk: pose %.0f deg cadence %u us exact clock %u ms; gyro pickup releases\n",
                (double)angles[a], cadences[c], lock_us / 1000);
        }
    }
}

static void pickup_then_new_stand(void)
{
    const float lead_rest[3] = {-0.087f, 0.040f, 0.916f};
    rest_trace_t trace;
    init_trace(&trace, 10000, true);
    const float flat[3] = {0, 0, 0.925f};
    (void)wait_for_clock(&trace, flat, 2000000);
    for (unsigned i = 0; i < 50; ++i) (void)hand_sample(&trace, 25);
    CHECK(!trace.game.game.beads.settled);
    unsigned lock_us = wait_for_clock(&trace, lead_rest, 3000000);
    printf("beads set-down: lead (-0.087,0.040,0.916) g pose forms exact clock %u ms after play\n",
        lock_us / 1000);
}

static void hand_noise_never_settles(void)
{
    rest_trace_t trace;
    init_trace(&trace, 10000, true);
    for (unsigned i = 0; i < 3000; ++i) {
        float degrees = 0.35f * signed_noise(&trace);
        (void)hand_sample(&trace, degrees);
        CHECK(!trace.game.game.beads.settled);
        CHECK(trace.tilt.still_us < 1200000u);
    }
    printf("beads hand: 30 s random +/-0.35 deg and +/-0.8 deg/s gyro jitter never settles\n");
}

static void missing_gyro_and_invalid_samples(void)
{
    const float pose[3] = {0.2f, 0, sqrtf(0.96f)};
    const float gyro[3] = {0, 0, 0};
    rest_trace_t trace;
    init_trace(&trace, 10000, true);
    for (unsigned i = 0; i < 300; ++i) {
        orb_game_point_t output = sample(&trace, pose, NULL);
        CHECK(fabsf(output.x - 0.2f) < 0.00001f);
        CHECK(!trace.game.game.beads.settled);
    }
    (void)wait_for_clock(&trace, pose, 2000000);
    uint32_t ticks = trace.game.ticks;
    const float invalid_force[3] = {NAN, 0, 1};
    const float invalid_gyro[3] = {0, NAN, 0};
    trace.cadence_us = 200000;
    (void)sample(&trace, pose, gyro);
    CHECK(exact_clock(&trace.game) && trace.tilt.still_us == 0);
    trace.cadence_us = 0;
    (void)sample(&trace, pose, gyro);
    trace.cadence_us = 10000;
    (void)sample(&trace, invalid_force, gyro);
    (void)sample(&trace, pose, invalid_gyro);
    CHECK(exact_clock(&trace.game) && trace.game.ticks == ticks);
    const float impact[3] = {1.8f, -0.8f, 0.7f};
    orb_game_point_t rejected = sample(&trace, impact, gyro);
    CHECK(trace.tilt.moving && !trace.game.game.beads.settled);
    CHECK(trace.game.ticks == ticks && rejected.x == 0 && rejected.y == 0);
    printf("beads sensor: missing gyro keeps fluid; gaps/NaN do not invent rest or pickup; impact releases before physics\n");
}

static void resting_pile(void)
{
    rest_trace_t trace;
    init_trace(&trace, 10000, false);
    /* A dense full-count pile must not hide instability behind short digits. */
    trace.game.game.beads.count = ORB_GAME_BEAD_COUNT;
    for (unsigned i = 0; i < 3000; ++i) (void)hand_sample(&trace, 35);
    pile_stats_t before = pile_stats(&trace.game, -1);
    float max_speed = 0, p95_speed = 0, worst_speed = 0, max_motion = 0;
    float x[ORB_GAME_BEAD_COUNT], y[ORB_GAME_BEAD_COUNT];
    for (unsigned i = 0; i < trace.game.game.beads.count; ++i) {
        x[i] = trace.game.game.beads.beads[i].x;
        y[i] = trace.game.game.beads.beads[i].y;
    }
    for (unsigned i = 0; i < 500; ++i) {
        orb_game_point_t output = hand_sample(&trace, 35);
        pile_stats_t stats = pile_stats(&trace.game, -1);
        max_speed = fmaxf(max_speed, stats.mean_speed);
        p95_speed = fmaxf(p95_speed, stats.p95_speed);
        worst_speed = fmaxf(worst_speed, stats.worst_speed);
        if (i % 10 == 9) record("rest-pile", &trace, output.x);
        for (unsigned j = 0; j < trace.game.game.beads.count; ++j) {
            const orb_game_bead_t *bead = &trace.game.game.beads.beads[j];
            max_motion = fmaxf(max_motion, hypotf(bead->x - x[j], bead->y - y[j]));
        }
    }
    printf("beads rest: %u particles, 30 s pile mean speed %.5f px/s, next 5 s peak mean speed %.5f px/s, p95 %.5f, worst %.5f px/s, max displacement %.5f px\n",
        trace.game.game.beads.count, (double)before.mean_speed, (double)max_speed, (double)p95_speed,
        (double)worst_speed, (double)max_motion);
    CHECK(before.mean_speed < 0.1f && max_speed < 0.1f);
    CHECK(max_motion < 0.5f);
}

static void maximum_particle_cpu_benchmark(void)
{
    enum { STEPS = 1200 };
    orb_game_state_t game;
    orb_game_init(&game, ORB_GAME_BEADS, 0x70657266u);
    game.game.beads.count = ORB_GAME_BEAD_COUNT;
    game.game.beads.time_valid = false;
    float samples[STEPS];
    double total = 0;
    for (unsigned i = 0; i < 60; ++i)
        orb_game_step(&game, 0.5f, 0.1f, true, false);
    for (unsigned i = 0; i < STEPS; ++i) {
        struct timespec before, after;
        CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &before) == 0);
        orb_game_step(&game, (i / 300u) % 2u ? -0.5f : 0.5f, 0.1f, true, false);
        CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &after) == 0);
        double us = (double)(after.tv_sec - before.tv_sec) * 1000000 +
            (double)(after.tv_nsec - before.tv_nsec) / 1000;
        samples[i] = (float)us;
        total += us;
    }
    qsort(samples, STEPS, sizeof(samples[0]), float_order);
    printf("beads solver CPU: %u particles, %u steps, 2 substeps / 6 constraint passes, "
        "mean %.3f us, p95 %.3f us, max %.3f us (host process CPU; not board)\n",
        game.game.beads.count, STEPS, total / STEPS,
        (double)samples[(STEPS - 1u) * 95u / 100u], (double)samples[STEPS - 1u]);
}

static void downward_pool(void)
{
    rest_trace_t trace;
    init_trace(&trace, 10000, false);
    const float pour[3] = {0.5f, -0.1f, 0.86023253f};
    const float down[3] = {0, 0.5f, 0.8660254f}, gyro[3] = {0};
    /* Returning from a real pour must produce a free surface under gravity.
     * No clock is available to replace physics with authored target sites. */
    for (unsigned i = 0; i < 400; ++i) (void)sample(&trace, pour, gyro);
    for (unsigned i = 0; i < 3000; ++i) (void)sample(&trace, down, gyro);
    CHECK(!trace.game.game.beads.time_valid && !trace.game.game.beads.settled);
    pile_stats_t stats = pile_stats(&trace.game, 1);
    CHECK(stats.mean_y > 100 && stats.mean_speed < 0.1f);
    float surface[7] = {999, 999, 999, 999, 999, 999, 999};
    float separation = 999;
    for (unsigned i = 0; i < trace.game.game.beads.count; ++i) {
        const orb_game_bead_t *b = &trace.game.game.beads.beads[i];
        CHECK(b->y > 0);
        if (b->x >= -140 && b->x < 140) {
            unsigned bin = (unsigned)((b->x + 140) / 40);
            surface[bin] = fminf(surface[bin], b->y);
        }
        for (unsigned j = 0; j < i; ++j) {
            const orb_game_bead_t *a = &trace.game.game.beads.beads[j];
            separation = fminf(separation, hypotf(a->x - b->x, a->y - b->y));
        }
    }
    float top = surface[0], bottom = surface[0];
    for (unsigned i = 0; i < 7; ++i) {
        CHECK(surface[i] < 999);
        top = fminf(top, surface[i]);
        bottom = fmaxf(bottom, surface[i]);
    }
    /* Discrete particles leave bead-scale ripples, not a diagonal lattice or
     * a disc-filling weave. Contact ink stays separated even under pressure. */
    CHECK(bottom - top < 4.0f * ORB_GAME_BEAD_RADIUS);
    CHECK(separation > 2.0f * ORB_GAME_BEAD_DRAW_RADIUS + 1);
    printf("beads downward pool: mean y %.3f px, free-surface range %.3f px, "
        "minimum centre separation %.3f px, mean speed %.5f px/s\n",
        (double)stats.mean_y, (double)(bottom - top), (double)separation,
        (double)stats.mean_speed);
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        s_csv = fopen(argv[1], "w");
        CHECK(s_csv != NULL);
        fputs("phase,time_s,gravity_x_g,mean_x_px,mean_y_px,mean_speed_px_s,rms_speed_px_s,low_rim_fraction,clock_held,quiet_us,axis_x_deg,effective_ax_px_s2,gravity_y_g,effective_ay_px_s2\n", s_csv);
    } else CHECK(argc == 1);
    pour_left_right();
    level_sweep();
    response_sweeps();
    desk_settle_and_pickup();
    pickup_then_new_stand();
    hand_noise_never_settles();
    missing_gyro_and_invalid_samples();
    resting_pile();
    downward_pool();
    maximum_particle_cpu_benchmark();
    if (s_csv) CHECK(fclose(s_csv) == 0);
    printf("games resting traces: %u checks passed\n", s_checks);
    return 0;
}
