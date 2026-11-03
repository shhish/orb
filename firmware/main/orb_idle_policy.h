#pragma once

#include "orb_power.h"
#include "orb_buttons.h"

typedef struct {
    uint32_t poll_ms, ui_ms;
    bool gyro, taps;
    bool minimal, face_down;
} orb_idle_policy_t;

static inline bool orb_idle_frame_needed(orb_power_snapshot_t power, uint32_t safe_revision)
{
    return power.panel_on && power.state == ORB_POWER_ACTIVE && !power.redact &&
        power.revision != safe_revision;
}

/* Physical tap detection needs >200 Hz acceleration. Keep that exception only
 * while a local safe action exists; the gyro is never needed to stop speech. */
static inline orb_idle_policy_t orb_idle_policy(orb_power_snapshot_t power,
                                               bool speech, bool timer_finished)
{
    bool interactive = power.state == ORB_POWER_ACTIVE && !power.redact;
    return (orb_idle_policy_t){
        .poll_ms = interactive ? 20 : 50,
        .ui_ms = power.panel_on ? 20 : 100,
        .gyro = interactive,
        .taps = (power.panel_on && power.brightness_pct > 0) || speech || timer_finished,
    };
}

/* Only the runtime sleep gate can admit this budget. A dark display alone
 * cannot suspend app/voice work or assume that GPIO/network wake is armed. */
static inline orb_idle_policy_t orb_idle_sleep_policy(orb_idle_policy_t policy,
                                                      orb_power_snapshot_t power, bool eligible)
{
    if (!eligible) return policy;
    policy.poll_ms = power.face_down ? 1000 : 100;
    policy.ui_ms = 1000;
    policy.gyro = policy.taps = false;
    policy.minimal = true;
    policy.face_down = power.face_down;
    return policy;
}

/* Edges wake the owner immediately. The debounce deadline must still wake it
 * while the pin remains steady, even with a slower sensor/service period. */
static inline uint32_t orb_idle_button_wait_limit(const orb_buttons_t *buttons, uint64_t now,
                                                  uint32_t maximum_ms)
{
    uint64_t next = now + maximum_ms;
    for (unsigned i = 0; i < ORB_BUTTON_COUNT; i++) {
        const orb_button_state_t *s = &buttons->buttons[i];
        if (s->raw != s->held && s->raw_at_ms + ORB_BUTTON_DEBOUNCE_MS < next)
            next = s->raw_at_ms + ORB_BUTTON_DEBOUNCE_MS;
        uint32_t hold = i == ORB_BUTTON_PWR ? ORB_BUTTON_PWR_HOLD_MS : ORB_BUTTON_BOOT_HOLD_MS;
        if (s->held && !s->consumed && !s->hold_sent && s->down_at_ms + hold < next)
            next = s->down_at_ms + hold;
        if (s->pending_short && !s->second_press && s->release_at_ms + ORB_BUTTON_DOUBLE_MS < next)
            next = s->release_at_ms + ORB_BUTTON_DOUBLE_MS;
    }
    if (buttons->synthetic_held && buttons->synthetic_release_ms < next) next = buttons->synthetic_release_ms;
    return next > now ? (uint32_t)(next - now) : 1;
}

static inline uint32_t orb_idle_button_wait(const orb_buttons_t *buttons, uint64_t now)
{
    return orb_idle_button_wait_limit(buttons, now, 50);
}

/* Confirmed face-down needs no 10 Hz pick-up arm. Resume that rate as soon
 * as a plausible exit pose arrives, so its 300 ms dwell still has <=250 ms
 * sample gaps; a one-second poll must never prove a continuous dwell. */
static inline uint32_t orb_idle_motion_wait(orb_idle_policy_t policy, bool valid,
                                           float x, float y, float z)
{
    if (policy.minimal && policy.face_down && valid) {
        float norm2 = x*x + y*y + z*z;
        if (norm2 >= 0.16f && norm2 <= 4.0f && (z >= 0 || z*z <= 0.36f * norm2)) return 100;
    }
    return policy.poll_ms;
}
