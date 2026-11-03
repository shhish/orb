#include "orb_senses.h"

#include <math.h>
#include <string.h>

#define DETENT_DEGREES 12.0f
#define SEED_MS 1000u

static void cancel_taps(orb_senses_t *s)
{
    s->first_tap_ms = 0;
    s->peak = s->second_peak = s->pending_double = false;
    s->settle_since_ms = 0;
}

static bool sample_taps(orb_senses_t *s, const orb_motion_sample_t *m, float excursion,
                         float lateral, float side_rate, bool flat, uint64_t now)
{
    bool double_tap = false;
    /* At 50 Hz a shock may occupy only one sample. Require isolated, strong
     * vertical shocks, 90..300 ms apart, with quiet before and after; typing
     * level vibration cannot arm a tap and sustained desk bumps cancel it. */
    bool shock = excursion >= 0.65f && excursion <= 2.5f && lateral < 0.25f && side_rate < 5.0f;
    if (excursion > 2.5f) {
        cancel_taps(s);
        s->desk_armed = false;
        s->guard_until_ms = now + 500;
    }
    if (s->first_tap_ms && now - s->first_tap_ms > 300 && !s->pending_double) cancel_taps(s);
    if (shock && s->desk_armed && now >= s->guard_until_ms && now >= s->refractory_until_ms) {
        if (!s->peak) {
            uint64_t interval = now - s->first_tap_ms;
            if (s->first_tap_ms && interval < 90) {
                cancel_taps(s);
                s->guard_until_ms = now + 400;
            } else {
                s->second_peak = s->first_tap_ms && interval <= 300;
                s->peak = true;
                s->peak_since_ms = now;
                if (!s->second_peak) s->first_tap_ms = now;
            }
        } else if (now - s->peak_since_ms > 40) {
            cancel_taps(s);
            s->guard_until_ms = now + 500;
            s->desk_armed = false;
        }
    } else if (s->peak) {
        if (excursion < 0.10f && now - s->peak_since_ms <= 60) {
            s->peak = false;
            if (s->second_peak) { s->pending_double = true; s->settle_since_ms = now; }
        } else {
            cancel_taps(s);
            s->guard_until_ms = now + 400;
        }
    } else if (excursion > 0.10f && excursion < 0.65f) {
        cancel_taps(s);
        s->guard_until_ms = now + 200;
    }
    /* The hardware engine catches peaks between host polls. Its quiet-window
     * validation is still subject to the same touch/motion/resting guard. */
    if (m->hardware_double_tap && s->desk_armed && flat && now >= s->guard_until_ms &&
        now >= s->refractory_until_ms) {
        s->pending_double = true;
        s->settle_since_ms = now;
    }
    if (s->pending_double && excursion < 0.10f && now - s->settle_since_ms >= 60) {
        double_tap = true;
        s->refractory_until_ms = now + 600;
        cancel_taps(s);
    }
    return double_tap;
}

orb_senses_result_t orb_senses_update(orb_senses_t *s, const orb_motion_sample_t *m,
                                     bool touching, uint64_t now)
{
    orb_senses_result_t out = {0};
    if (!s || !m) return out;
    bool finite = isfinite(m->ax) && isfinite(m->ay) && isfinite(m->az) &&
                  isfinite(m->gx) && isfinite(m->gy) && isfinite(m->gz);
    if (!finite || !s->initialized || now <= s->previous_ms || now - s->previous_ms > 100) {
        float bias[3] = {s->bias_x, s->bias_y, s->bias_z};
        bool bias_valid = s->bias_valid;
        memset(s, 0, sizeof(*s));
        s->bias_x = bias[0]; s->bias_y = bias[1]; s->bias_z = bias[2];
        s->bias_valid = bias_valid;
        s->initialized = finite;
        s->previous_ax = m->ax; s->previous_ay = m->ay;
        s->previous_g[0] = m->gx; s->previous_g[1] = m->gy; s->previous_g[2] = m->gz;
        s->previous_ms = now;
        s->flat_since_ms = s->quiet_since_ms = now;
        s->previous_z = finite ? m->az : 1.0f;
        s->baseline_z = s->previous_z;
        return out;
    }
    float dt = (float)(now - s->previous_ms) * 0.001f;
    s->previous_ms = now;
    float z_delta = fabsf(m->az - s->previous_z);
    /* Seed the bias from the first second the sensor is plainly still: the
     * accelerometer is steady and the gyro output is constant (a hand-held
     * Orb wobbles both). Until then nothing arms, so no gesture can fire. */
    bool steady = z_delta < 0.02f && fabsf(m->ax - s->previous_ax) < 0.02f &&
                  fabsf(m->ay - s->previous_ay) < 0.02f && fabsf(m->gx - s->previous_g[0]) < 1.5f &&
                  fabsf(m->gy - s->previous_g[1]) < 1.5f && fabsf(m->gz - s->previous_g[2]) < 1.5f;
    s->previous_ax = m->ax; s->previous_ay = m->ay;
    s->previous_g[0] = m->gx; s->previous_g[1] = m->gy; s->previous_g[2] = m->gz;
    if (!s->bias_valid) {
        if (!steady || !s->seed_count) {
            memset(s->seed_sum, 0, sizeof(s->seed_sum));
            s->seed_count = 0;
            s->seed_since_ms = now;
        }
        if (steady || !s->seed_count) {
            s->seed_sum[0] += m->gx; s->seed_sum[1] += m->gy; s->seed_sum[2] += m->gz;
            s->seed_count++;
        }
        if (s->seed_count > 1 && now - s->seed_since_ms >= SEED_MS) {
            s->bias_x = s->seed_sum[0] / (float)s->seed_count;
            s->bias_y = s->seed_sum[1] / (float)s->seed_count;
            s->bias_z = s->seed_sum[2] / (float)s->seed_count;
            s->bias_valid = true;
        }
    }
    float gx = m->gx - s->bias_x, gy = m->gy - s->bias_y, gz = m->gz - s->bias_z;
    float az = fabsf(m->az), lateral = hypotf(m->ax, m->ay);
    float side_rate = hypotf(gx, gy);
    bool flat = az > 0.82f && az < 1.14f && lateral < 0.22f;
    /* Startup/pickup may leave the baseline in an arbitrary pose. Reacquire
     * gravity while unarmed, otherwise starting upright would permanently
     * prevent desk qualification. Once armed, shocks cannot move the base. */
    if (!s->desk_armed && flat && side_rate < 2.5f && fabsf(gz) < 2.5f && z_delta < 0.04f)
        s->baseline_z = m->az;
    float excursion = fabsf(m->az - s->baseline_z);
    bool quiet = s->bias_valid && flat && excursion < 0.08f && side_rate < 2.5f &&
                 fabsf(gz) < 2.5f && z_delta < 0.04f;
    bool movement = lateral > 0.32f || (s->bias_valid && side_rate > 8.0f) || fabsf(gz) > 150.0f;
    s->previous_z = m->az;

    if (touching || movement) {
        s->desk_armed = false;
        s->flat_since_ms = s->quiet_since_ms = now;
        s->guard_until_ms = now + 500;
        s->angle = 0;
        cancel_taps(s);
        return out;
    }
    if (!flat && excursion < 0.60f) {
        s->flat_since_ms = now;
        s->quiet_since_ms = now;
        s->desk_armed = false;
        s->angle = 0;
        cancel_taps(s);
    }
    if (!quiet) s->quiet_since_ms = now;
    if (quiet && now - s->quiet_since_ms >= 400 && now >= s->guard_until_ms) s->desk_armed = true;
    out.resting = quiet && s->desk_armed;
    if (quiet && now - s->quiet_since_ms >= 500) {
        /* Learn bias only at rest: learning while rotating would swallow a
         * deliberate slow turn. The dead band rejects remaining gyro drift. */
        float alpha = dt / (2.0f + dt);
        /* A 20 s time constant follows thermal drift; quiet already bounds the
         * residual below the dead band, so a slow turn cannot be learned away. */
        float drift = dt / (20.0f + dt);
        s->bias_x += drift * gx; s->bias_y += drift * gy; s->bias_z += drift * gz;
        s->baseline_z += alpha * (m->az - s->baseline_z);
    }
    if (flat && s->desk_armed && now >= s->guard_until_ms && side_rate < 5.0f) {
        /* Screen +z points out of the face, so positive right-hand rotation
         * is counterclockwise. Match the rim's clockwise-positive owner. */
        float rate = -gz;
        if (fabsf(rate) > 2.5f) s->angle += rate * dt;
        while (s->angle >= DETENT_DEGREES) { out.detents++; s->angle -= DETENT_DEGREES; }
        while (s->angle <= -DETENT_DEGREES) { out.detents--; s->angle += DETENT_DEGREES; }
    } else s->angle = 0;

    out.double_tap = sample_taps(s, m, excursion, lateral, side_rate, flat, now);
    return out;
}
