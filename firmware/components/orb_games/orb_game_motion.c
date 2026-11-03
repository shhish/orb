#include "orb_game_logic.h"

#include <math.h>
#include <string.h>

#define TAU 6.2831853071795864769f

void orb_game_tilt_reset(orb_game_tilt_t *tilt)
{
    if (tilt) memset(tilt, 0, sizeof(*tilt));
}

static void tilt_frame(orb_game_tilt_t *tilt, float nx, float ny, float nz)
{
    tilt->neutral[0] = nx; tilt->neutral[1] = ny; tilt->neutral[2] = nz;
    /* Rodrigues' shortest rotation retains both axes near upright. A plain
     * x/y offset couples pitch into sideways play at a viewing angle. */
    if (nz > -0.9999f) {
        float scale = 1 / (1 + nz);
        tilt->right[0] = 1 - nx * nx * scale;
        tilt->right[1] = -nx * ny * scale;
        tilt->right[2] = -nx;
        tilt->up[0] = -nx * ny * scale;
        tilt->up[1] = 1 - ny * ny * scale;
        tilt->up[2] = -ny;
    } else {
        /* Antipodal neutral has no unique shortest rotation. */
        tilt->right[0] = 1; tilt->right[1] = tilt->right[2] = 0;
        tilt->up[1] = -1; tilt->up[0] = tilt->up[2] = 0;
    }
}

static void tilt_observe(orb_game_tilt_t *tilt, const float force[3],
                          const float gyro[3], uint32_t sample_us)
{
    float dt = (float)sample_us / 1000000.0f;
    float alpha = 1 - expf(-dt / 0.10f);
    float rate_alpha = 1 - expf(-dt / 0.08f);
    float coherent_alpha = 1 - expf(-dt / 0.20f);
    float variance = 0, rate = 0, coherent = 0, gyro_rate = 0, gyro_variance = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (!tilt->observed) tilt->pose_mean[axis] = force[axis];
        float delta = force[axis] - tilt->pose_mean[axis];
        tilt->pose_mean[axis] += alpha * delta;
        tilt->pose_rate[axis] += rate_alpha *
            (alpha * delta / dt - tilt->pose_rate[axis]);
        variance += delta * delta;
        rate += tilt->pose_rate[axis] * tilt->pose_rate[axis];
        /* Accumulate slow, coherent movement separately from alternating desk
         * vibration. Bound impulses so a pickup does not leave a long tail. */
        float slow_rate = fmaxf(-0.018f, fminf(0.018f, tilt->pose_rate[axis]));
        tilt->coherent_rate[axis] += coherent_alpha * (slow_rate - tilt->coherent_rate[axis]);
        coherent += tilt->coherent_rate[axis] * tilt->coherent_rate[axis];
        if (gyro) {
            if (!tilt->gyro_observed) tilt->gyro_mean[axis] = gyro[axis];
            float gd = gyro[axis] - tilt->gyro_mean[axis];
            tilt->gyro_mean[axis] += rate_alpha * gd;
            gyro_variance += gd * gd;
            gyro_rate += tilt->gyro_mean[axis] * tilt->gyro_mean[axis];
        }
    }
    tilt->observed = true;
    tilt->gyro_observed = gyro != NULL;
    tilt->pose_variance += alpha * (variance - tilt->pose_variance);
    tilt->gyro_variance = gyro ? tilt->gyro_variance +
        rate_alpha * (gyro_variance - tilt->gyro_variance) : 0;
    /* A short (~100 ms) motion window tolerates sensor noise and typing, but
     * a coherent change of pose/rate immediately interrupts a held clock.
     * Absolute angle relative to the play zero is deliberately absent. */
    float instant_gyro = gyro ? gyro[0] * gyro[0] + gyro[1] * gyro[1] + gyro[2] * gyro[2] : 0;
    bool active = tilt->pose_variance > 0.020f * 0.020f || rate > 0.028f * 0.028f ||
        coherent > 0.010f * 0.010f ||
        (gyro && (instant_gyro > 0.6f * 0.6f || gyro_rate > 0.5f * 0.5f ||
                  tilt->gyro_variance > 0.20f * 0.20f));
    bool quiet = tilt->pose_variance < 0.015f * 0.015f && rate < 0.020f * 0.020f &&
        coherent < 0.006f * 0.006f &&
        (!gyro || (instant_gyro < 0.35f * 0.35f && gyro_rate < 0.25f * 0.25f &&
                   tilt->gyro_variance < 0.15f * 0.15f));
    if (active) tilt->moving = true;
    else if (quiet) tilt->moving = false;
    if (!quiet) tilt->still_us = 0;
    else if (tilt->still_us < 10000000u)
        tilt->still_us += sample_us;
}

bool orb_game_tilt_update(orb_game_tilt_t *tilt, float ax, float ay, float az,
                          uint32_t sample_us, float *out_ax, float *out_ay)
{
    return orb_game_tilt_update_motion(tilt, ax, ay, az, NULL,
                                       sample_us, out_ax, out_ay);
}

bool orb_game_tilt_update_motion(orb_game_tilt_t *tilt, float ax, float ay, float az,
                                 const float gyro[3], uint32_t sample_us,
                                 float *out_ax, float *out_ay)
{
    if (out_ax) *out_ax = 0;
    if (out_ay) *out_ay = 0;
    if (!tilt || !out_ax || !out_ay) return false;
    float magnitude = sqrtf(ax * ax + ay * ay + az * az);
    bool gyro_finite = !gyro || (isfinite(gyro[0]) && isfinite(gyro[1]) && isfinite(gyro[2]));
    /* Sparse/invalid data cannot prove rest or pickup. A finite impact/free
     * fall does prove motion, even while its steering input is suppressed. */
    if (!sample_us || sample_us > 100000u || !isfinite(magnitude) ||
        magnitude < 0.5f || magnitude > 1.5f || !gyro_finite) {
        bool impact = isfinite(magnitude) && (magnitude < 0.5f || magnitude > 1.5f);
        if (!tilt->ready) orb_game_tilt_reset(tilt);
        tilt->still_us = 0;
        if (impact) {
            tilt->moving = true;
            tilt->observed = false;
        }
        tilt->gyro_observed = false;
        memset(tilt->pose_rate, 0, sizeof(tilt->pose_rate));
        memset(tilt->coherent_rate, 0, sizeof(tilt->coherent_rate));
        tilt->pose_variance = tilt->gyro_variance = 0;
        return false;
    }
    const float force[3] = {ax, ay, az};
    tilt_observe(tilt, force, gyro, sample_us);
    if (!tilt->ready) {
        if (tilt->average_us) {
            float scale = 1.0f / (float)tilt->average_us;
            float dx = ax - tilt->sum[0] * scale;
            float dy = ay - tilt->sum[1] * scale;
            float dz = az - tilt->sum[2] * scale;
            /* Keep the existing neutral until the held pose is actually still.
             * The tolerance admits sensor noise, not a moving 300 ms average. */
            if (dx * dx + dy * dy + dz * dz > 0.01f)
                orb_game_tilt_reset(tilt);
        }
        uint32_t weight = 300000u - tilt->average_us;
        if (weight > sample_us) weight = sample_us;
        tilt->sum[0] += ax * (float)weight;
        tilt->sum[1] += ay * (float)weight;
        tilt->sum[2] += az * (float)weight;
        tilt->average_us += weight;
        if (tilt->average_us < 300000u) return false;
        float length = sqrtf(tilt->sum[0] * tilt->sum[0] + tilt->sum[1] * tilt->sum[1] +
                             tilt->sum[2] * tilt->sum[2]);
        if (!isfinite(length) || length < 30000) {
            orb_game_tilt_reset(tilt);
            return false;
        }
        float nx = tilt->sum[0] / length, ny = tilt->sum[1] / length;
        float nz = tilt->sum[2] / length;
        tilt_frame(tilt, nx, ny, nz);
        tilt->ready = true;
        return true;
    }
    /* A held tilt is sustained input. Only round start/restart captures the
     * steering frame; desk stillness never changes gravity or the play zero. */
    ax /= magnitude; ay /= magnitude; az /= magnitude;
    float x = tilt->right[0] * ax + tilt->right[1] * ay + tilt->right[2] * az;
    float y = tilt->up[0] * ax + tilt->up[1] * ay + tilt->up[2] * az;
    float amount = hypotf(x, y);
    /* Smooth small-signal attenuation has no hard zero region: crossing
     * level keeps fine input continuous instead of sticking in a dead zone. */
    float response = 0.7f + 0.3f * amount;
    float target_x = x * response, target_y = y * response;
    /* One-euro style filtering (HIG §6): quiet near rest, but a flick opens
     * the cutoff immediately. One shared radial cutoff preserves diagonals;
     * every coefficient follows sensor time, never fixed-step catch-up time. */
    float dt = (float)sample_us / 1000000.0f;
    float derivative_alpha = 1 - expf(-TAU * 5 * dt);
    tilt->derivative_x += derivative_alpha *
        ((target_x - tilt->previous_x) / dt - tilt->derivative_x);
    tilt->derivative_y += derivative_alpha *
        ((target_y - tilt->previous_y) / dt - tilt->derivative_y);
    tilt->previous_x = target_x;
    tilt->previous_y = target_y;
    float cutoff = fminf(35, 4 + 12 * hypotf(tilt->derivative_x, tilt->derivative_y));
    float alpha = 1 - expf(-TAU * cutoff * dt);
    tilt->filtered_x += alpha * (target_x - tilt->filtered_x);
    tilt->filtered_y += alpha * (target_y - tilt->filtered_y);
    *out_ax = tilt->filtered_x;
    *out_ay = tilt->filtered_y;
    return true;
}

bool orb_game_gravity_update_motion(orb_game_tilt_t *tilt, float ax, float ay, float az,
                                    const float gyro[3], uint32_t sample_us,
                                    float *out_ax, float *out_ay)
{
    /* App input has already applied the board's calibrated axis frame. A
     * physical toy uses that frame, never the pose in which it was opened. */
    if (tilt && !tilt->ready) {
        tilt_frame(tilt, 0, 0, 1);
        tilt->ready = true;
    }
    bool valid = orb_game_tilt_update_motion(tilt, ax, ay, az, gyro,
                                             sample_us, out_ax, out_ay);
    if (valid) {
        float magnitude = sqrtf(ax * ax + ay * ay + az * az);
        *out_ax = ax / magnitude;
        *out_ay = ay / magnitude;
    }
    return valid;
}

void orb_game_set_motion(orb_game_state_t *state, const orb_game_tilt_t *tilt)
{
    if (!state) return;
    state->motion_moving = !tilt || tilt->moving;
    if (state->kind == ORB_GAME_BEADS) {
        /* Release in sensor time, including an impact whose physics step is
         * suppressed. The next valid, quiet sample must not erase the pickup. */
        if (state->motion_moving) state->game.beads.settled = false;
        uint32_t ticks = tilt && tilt->ready && tilt->gyro_observed && state->game.beads.time_valid ?
            tilt->still_us / ORB_GAME_STEP_US : 0;
        state->game.beads.settle_ticks = (uint16_t)(ticks < ORB_GAME_BEAD_SETTLE_STEPS ?
            ticks : ORB_GAME_BEAD_SETTLE_STEPS);
    }
}
