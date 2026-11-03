#include "orb_power_private.h"
#include <math.h>

static void reset_pickup(orb_power_motion_t *motion)
{
    motion->resting = motion->armed = motion->candidate = false;
    motion->acceleration_candidate = false;
}

static void reset_continuity(orb_power_motion_t *motion)
{
    motion->have_sample = false;
    reset_pickup(motion);
    motion->down_pending = motion->up_pending = false;
}

static unsigned sample_face_down(orb_power_motion_t *motion, const float unit[3],
                                 const float raw_unit[3], uint64_t now_ms)
{
    unsigned events = 0;
    if (!motion->face_down) {
        /* Raw agreement prevents the low-pass filter's tail from proving a
         * face-down dwell after Orb has already been turned back over. */
        if (unit[2] <= -0.80f && raw_unit[2] <= -0.80f) {
            if (!motion->down_pending) { motion->down_pending = true; motion->down_ms = now_ms; }
            if (now_ms - motion->down_ms >= 300) {
                motion->face_down = true;
                motion->down_pending = motion->up_pending = false;
                reset_pickup(motion);
                events |= ORB_POWER_MOTION_DOWN;
            }
        } else motion->down_pending = false;
    } else {
        if (unit[2] >= -0.60f && raw_unit[2] >= -0.60f) {
            if (!motion->up_pending) { motion->up_pending = true; motion->up_ms = now_ms; }
            if (now_ms - motion->up_ms >= 300) {
                motion->face_down = false;
                motion->up_pending = false;
                events |= ORB_POWER_MOTION_UP;
            }
        } else motion->up_pending = false;
    }
    return events;
}

unsigned orb_power_motion_sample(orb_power_motion_t *motion, float x, float y, float z, uint64_t now_ms)
{
    float raw[3] = {x, y, z};
    float norm = sqrtf(x*x + y*y + z*z);
    /* Plausibility is broader than rest: a vigorous lift must remain usable
     * evidence rather than disarming the detector at its first acceleration. */
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(norm) || norm < 0.40f || norm > 2.00f) {
        reset_continuity(motion);
        return ORB_POWER_MOTION_INVALID;
    }
    /* Reject duplicate/backward samples before unsigned gap arithmetic. */
    if (motion->have_sample && now_ms <= motion->sample_ms) return ORB_POWER_MOTION_INVALID;
    bool gap = motion->have_sample && now_ms - motion->sample_ms > 250;
    if (gap) reset_continuity(motion);
    bool first = !motion->have_sample;
    /* A duplicate sample cannot manufacture elapsed dwell, nor change the
     * direction of the previous timestamp's accepted measurement. */
    float delta2 = 0;
    if (first) {
        for (int i = 0; i < 3; i++) motion->filtered[i] = raw[i];
    } else {
        float dt = (float)(now_ms - motion->sample_ms);
        float alpha = dt / (150.0f + dt);
        for (int i = 0; i < 3; i++) {
            float delta = raw[i] - motion->previous[i];
            delta2 += delta * delta;
            motion->filtered[i] += alpha * (raw[i] - motion->filtered[i]);
        }
    }
    for (int i = 0; i < 3; i++) motion->previous[i] = raw[i];
    motion->have_sample = true;
    motion->sample_ms = now_ms;
    float filtered_norm = sqrtf(motion->filtered[0]*motion->filtered[0] + motion->filtered[1]*motion->filtered[1] + motion->filtered[2]*motion->filtered[2]);
    if (filtered_norm < 0.01f) {
        reset_continuity(motion);
        return ORB_POWER_MOTION_INVALID;
    }
    float unit[3], raw_unit[3];
    for (int i = 0; i < 3; i++) {
        unit[i] = motion->filtered[i] / filtered_norm;
        raw_unit[i] = raw[i] / norm;
    }
    unsigned events = gap ? ORB_POWER_MOTION_INVALID : 0;
    events |= sample_face_down(motion, unit, raw_unit, now_ms);
    if (motion->face_down) return events;
    if (!motion->armed) {
        /* The rest pose is arbitrary: a flat desk, tilted dock or edge-on
         * stand must all arm. Bound total drift as well as sample-to-sample
         * noise so a slow rotation cannot masquerade as stillness. */
        bool stable = norm >= 0.85f && norm <= 1.15f && delta2 <= 0.06f*0.06f;
        if (stable && motion->resting) {
            float dot = 0, raw_dot = 0;
            for (int i = 0; i < 3; i++) {
                dot += unit[i] * motion->baseline[i];
                raw_dot += raw_unit[i] * motion->baseline[i];
            }
            stable = dot >= 0.990268f && raw_dot >= 0.990268f; /* cos(8 degrees) */
        }
        if (stable) {
            if (!motion->resting) {
                motion->resting = true;
                motion->rest_ms = now_ms;
                for (int i = 0; i < 3; i++) motion->baseline[i] = unit[i];
            }
            if (now_ms - motion->rest_ms >= 800) {
                for (int i = 0; i < 3; i++) motion->baseline[i] = unit[i];
                motion->baseline_norm = filtered_norm;
                motion->armed = true;
                motion->candidate = false;
                motion->acceleration_candidate = false;
            }
        } else motion->resting = false;
    }
    if (motion->armed) {
        float dot = 0, raw_dot = 0;
        for (int i = 0; i < 3; i++) {
            dot += unit[i] * motion->baseline[i];
            raw_dot += raw_unit[i] * motion->baseline[i];
        }
        /* Suppress motion wake while a genuine face-down dwell is forming.
         * Its independent confirmation below is what forces silence/sleep. */
        if (raw_unit[2] <= -0.80f) {
            motion->candidate = motion->acceleration_candidate = false;
            return events;
        }
        float tilt_limit = motion->candidate ? 0.939693f : 0.906308f; /* 20 / 25 degrees */
        if (dot <= tilt_limit && raw_dot <= tilt_limit) {
            if (!motion->candidate) { motion->candidate = true; motion->candidate_ms = now_ms; }
        } else motion->candidate = false;

        /* A lift without tilt is meaningful too. A same-sign acceleration
         * dwell rejects short bumps and alternating typing/ringing impulses.
         * Separate entry/exit thresholds avoid chattering near the boundary. */
        float deviation = norm - motion->baseline_norm;
        if (motion->acceleration_candidate &&
            deviation * (float)motion->acceleration_direction < 0.10f)
            motion->acceleration_candidate = false;
        if (!motion->acceleration_candidate && fabsf(deviation) >= 0.18f) {
            motion->acceleration_candidate = true;
            motion->acceleration_direction = deviation >= 0 ? 1 : -1;
            motion->acceleration_ms = now_ms;
        }
        bool tilted = motion->candidate && now_ms - motion->candidate_ms >= 150;
        bool lifted = motion->acceleration_candidate && now_ms - motion->acceleration_ms >= 200;
        /* End in a viewing pose: neither vertical carrying nor a filter
         * tail after turning away proves someone is looking at the glass. */
        if ((tilted || lifted) && unit[2] >= 0.30f && raw_unit[2] >= 0.30f) {
            if (!motion->have_pickup || now_ms - motion->pickup_ms >= 10000) {
                events |= ORB_POWER_MOTION_PICKUP;
                motion->have_pickup = true;
                motion->pickup_ms = now_ms;
            }
            /* A cooldown-suppressed viewing gesture also needs a fresh rest. */
            reset_pickup(motion);
        }
    }
    return events;
}
