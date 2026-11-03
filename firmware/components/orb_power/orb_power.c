#include "orb_power.h"
#include "orb_power_private.h"
#include <stddef.h>

static uint32_t idle_inhibitors(uint32_t inhibitors)
{
    return inhibitors & (ORB_POWER_INHIBIT_VOICE | ORB_POWER_INHIBIT_PLAYBACK |
        ORB_POWER_INHIBIT_OTA | ORB_POWER_INHIBIT_APP);
}

static uint32_t moment_duration(const orb_power_controller_t *power)
{
    return power->preferences.reduce_motion ? 150 : power->moment == ORB_POWER_MOMENT_WAKE ? 400 : 250;
}

void orb_power_changed(orb_power_controller_t *power)
{
    if (++power->revision == 0) power->revision = 1;
}

static bool set_time(orb_power_controller_t *power, uint64_t now_ms)
{
    if (!power || now_ms < power->now_ms) return false;
    power->now_ms = now_ms;
    if (power->moment != ORB_POWER_MOMENT_NONE && now_ms - power->moment_ms >= moment_duration(power)) {
        power->moment = ORB_POWER_MOMENT_NONE;
        orb_power_changed(power);
    }
    return true;
}

static uint64_t add_deadline(uint64_t base, uint64_t elapsed)
{
    return UINT64_MAX - base < elapsed ? UINT64_MAX : base + elapsed;
}

static void idle_limits(const orb_power_controller_t *power, uint32_t *dim, uint32_t *aod, uint32_t *sleep)
{
    bool low = power->battery_mode != ORB_POWER_BATTERY_NORMAL;
    *dim = low ? 10000 : 20000;
    *aod = low ? 15000 : 30000;
    *sleep = low ? 60000 : power->usb ? 1800000 : 120000;
    if (!power->preferences.always_on || power->battery_mode >= ORB_POWER_BATTERY_VERY_LOW) *sleep = *aod;
    if (power->battery_mode == ORB_POWER_BATTERY_CRITICAL) *dim = *aod = *sleep = 10000;
}

static void set_state(orb_power_controller_t *power, orb_power_state_t state)
{
    if (power->state != state) {
        power->state = state;
        orb_power_changed(power);
    }
    if (state != ORB_POWER_ACTIVE && !power->redact) {
        power->redact = true;
        orb_power_changed(power);
    }
}

static void reset_pickup_budget(orb_power_controller_t *power)
{
    power->ignored_pickups = 0;
    power->pickup_suppressed_until_ms = 0;
    power->pickup_budgeted = false;
}

static uint32_t automatic_end(const orb_power_controller_t *power)
{
    return power->pickup_glance ? 9000 : 30000;
}

static void finish_automatic(orb_power_controller_t *power, uint64_t ended_ms)
{
    if (!power->automatic) return;
    uint64_t deadline = add_deadline(power->automatic_ms, automatic_end(power));
    if (ended_ms > deadline) ended_ms = deadline;
    if (power->pickup_budgeted && !power->usb) {
        if (power->ignored_pickups < 7) power->ignored_pickups++;
        if (power->ignored_pickups >= 3) {
            uint32_t duration = 300000u << (power->ignored_pickups - 3);
            if (duration > 3600000) duration = 3600000;
            /* An unrepresentable deadline must not become an infinite ban at
             * the end of the monotonic range. Expire this bounded lease. */
            power->pickup_suppressed_until_ms = UINT64_MAX - ended_ms < duration ? ended_ms : ended_ms + duration;
        }
    }
    power->automatic = false;
}

static void evaluate_cutoff(orb_power_controller_t *power)
{
    /* Unknown breaks evidence. A one-point gauge bounce cannot restart the
     * minute; recovery needs 4%, independently of the existing critical tier. */
    if (power->usb || power->battery_pct < 0 || power->battery_pct >= 4) {
        power->cutoff_pending = false;
        if (power->shutdown_requested) orb_power_changed(power);
        power->shutdown_requested = false;
    } else if (!power->cutoff_pending && power->battery_pct <= 2) {
        power->cutoff_pending = true;
        power->cutoff_ms = power->now_ms;
    }
    if (power->cutoff_pending && !power->shutdown_requested && power->now_ms - power->cutoff_ms >= 60000) {
        power->shutdown_requested = true;
        orb_power_changed(power);
    }
}

void orb_power_enter_sleep(orb_power_controller_t *power, bool immediate)
{
    finish_automatic(power, power->now_ms);
    bool entering = power->state != ORB_POWER_SLEEP;
    set_state(power, ORB_POWER_SLEEP);
    if (immediate) {
        if (power->moment != ORB_POWER_MOMENT_NONE) orb_power_changed(power);
        power->moment = ORB_POWER_MOMENT_NONE;
    } else if (entering) {
        power->moment = ORB_POWER_MOMENT_SLEEP;
        power->moment_ms = power->now_ms;
        orb_power_changed(power);
    }
}

static void enter_timed_sleep(orb_power_controller_t *power, uint64_t deadline)
{
    bool immediate = power->battery_mode == ORB_POWER_BATTERY_CRITICAL ||
        power->now_ms - deadline >= (power->preferences.reduce_motion ? 150u : 250u);
    orb_power_enter_sleep(power, immediate);
    if (power->moment == ORB_POWER_MOMENT_SLEEP) power->moment_ms = deadline;
}

static void enter_rest(orb_power_controller_t *power, orb_power_state_t state, uint64_t deadline)
{
    if (power->state == state) return;
    set_state(power, state);
    if (power->now_ms - deadline < (power->preferences.reduce_motion ? 150u : 250u)) {
        power->moment = ORB_POWER_MOMENT_REST;
        power->moment_ms = deadline;
    } else power->moment = ORB_POWER_MOMENT_NONE;
}

static void evaluate_idle(orb_power_controller_t *power)
{
    evaluate_cutoff(power);
    if (power->motion.face_down) {
        orb_power_enter_sleep(power, true);
        return;
    }
    if (!power->quiet && !power->display_locked && idle_inhibitors(power->inhibitors) && power->battery_mode != ORB_POWER_BATTERY_CRITICAL) {
        if (power->automatic && power->now_ms - power->automatic_ms >= automatic_end(power))
            finish_automatic(power, add_deadline(power->automatic_ms, automatic_end(power)));
        power->activity_ms = power->now_ms;
        return;
    }
    /* Once asleep, changing USB or battery facts cannot wake the display. */
    if (power->state == ORB_POWER_SLEEP) return;
    uint32_t dim, aod, sleep;
    idle_limits(power, &dim, &aod, &sleep);
    uint64_t idle = power->now_ms - power->activity_ms;
    if (power->automatic) {
        uint64_t elapsed = power->now_ms - power->automatic_ms;
        uint32_t active = power->pickup_glance ? 6000 : 20000;
        if (elapsed < active) return;
        uint64_t end = add_deadline(power->automatic_ms, automatic_end(power));
        if (elapsed < automatic_end(power)) {
            enter_rest(power, ORB_POWER_DIMMED, add_deadline(power->automatic_ms, active));
            return;
        }
        bool resume = power->automatic_return == ORB_POWER_ALWAYS_ON && idle < sleep &&
            power->preferences.always_on && power->battery_mode < ORB_POWER_BATTERY_VERY_LOW;
        finish_automatic(power, end);
        if (resume) enter_rest(power, ORB_POWER_ALWAYS_ON, end);
        else enter_timed_sleep(power, end);
        return;
    }
    if (idle >= sleep) {
        enter_timed_sleep(power, add_deadline(power->activity_ms, sleep));
    }
    else if (power->quiet || power->display_locked) {
        /* Quiet is an explicit desk preference. Crossing the ordinary Dimmed
         * deadline must not brighten/regress its already selected AOD face. */
        if (power->preferences.always_on && power->battery_mode < ORB_POWER_BATTERY_VERY_LOW)
            set_state(power, ORB_POWER_ALWAYS_ON);
        else orb_power_enter_sleep(power, power->motion.face_down ||
                                   power->battery_mode == ORB_POWER_BATTERY_CRITICAL);
    }
    else if (idle >= aod) {
        enter_rest(power, ORB_POWER_ALWAYS_ON, add_deadline(power->activity_ms, aod));
    }
    else if (idle >= dim && power->state != ORB_POWER_DIMMED) {
        enter_rest(power, ORB_POWER_DIMMED, add_deadline(power->activity_ms, dim));
    }
}

orb_power_preferences_t orb_power_default_preferences(void)
{
    return (orb_power_preferences_t){ .brightness_pct = 70, .always_on_brightness_pct = 9, .always_on = true, .motion_wake = true };
}

void orb_power_init(orb_power_controller_t *power, uint64_t now_ms)
{
    if (!power) return;
    *power = (orb_power_controller_t){ .preferences = orb_power_default_preferences(),
        .now_ms = now_ms, .activity_ms = now_ms, .state = ORB_POWER_ACTIVE,
        .redact = true, .battery_pct = -1, .revision = 1 };
}

bool orb_power_tick(orb_power_controller_t *power, uint64_t now_ms)
{
    if (!set_time(power, now_ms)) return false;
    evaluate_idle(power);
    return true;
}

bool orb_power_note_activity(orb_power_controller_t *power, uint64_t now_ms)
{
    if (!set_time(power, now_ms)) return false;
    reset_pickup_budget(power);
    power->automatic = false;
    power->activity_ms = now_ms;
    return true;
}

bool orb_power_request_sleep(orb_power_controller_t *power, uint64_t now_ms)
{
    if (!set_time(power, now_ms)) return false;
    orb_power_enter_sleep(power, power->motion.face_down || power->battery_mode == ORB_POWER_BATTERY_CRITICAL);
    return true;
}

orb_power_input_result_t orb_power_wake(orb_power_controller_t *power, orb_power_wake_t source, uint64_t now_ms)
{
    orb_power_input_result_t result = {0};
    if (!power || source > ORB_POWER_WAKE_ATTENTION || source < ORB_POWER_WAKE_TOUCH || now_ms < power->now_ms) return result;
    bool manual = source == ORB_POWER_WAKE_TOUCH || source == ORB_POWER_WAKE_PWR;
    if (!set_time(power, now_ms)) return result;
    evaluate_idle(power);
    bool consumed = power->state != ORB_POWER_ACTIVE || power->redact || power->quiet ||
        power->preferences.brightness_pct == 0;
    if (manual) reset_pickup_budget(power);
    if (power->motion.face_down) return result;
    if (!manual) {
        bool attention_over_glance = source == ORB_POWER_WAKE_ATTENTION && power->automatic && power->pickup_glance;
        if (power->quiet || power->battery_mode == ORB_POWER_BATTERY_CRITICAL ||
            (power->state == ORB_POWER_ACTIVE && !attention_over_glance)) return result;
        if (source == ORB_POWER_WAKE_PICKUP && (!power->preferences.motion_wake ||
            (!power->usb && now_ms < power->pickup_suppressed_until_ms) ||
            (power->have_pickup_wake && now_ms - power->pickup_wake_ms < 10000))) return result;
        if (source == ORB_POWER_WAKE_ATTENTION && power->have_attention && now_ms - power->attention_ms < 30000) return result;
    }
    bool was_sleep = power->state == ORB_POWER_SLEEP;
    bool lower_power = power->state != ORB_POWER_ACTIVE;
    orb_power_state_t previous = power->state;
    power->display_locked = false;
    set_state(power, ORB_POWER_ACTIVE);
    if (manual) {
        power->automatic = false;
        power->activity_ms = now_ms;
        power->quiet = false;
        if (power->redact) orb_power_changed(power);
        power->redact = false;
    } else {
        /* Automatic viewing gets its own short lease. It must never buy a
         * fresh Always On window. Finish any preceding pickup separately:
         * the attention window itself never spends the pickup budget. */
        orb_power_state_t resume = source == ORB_POWER_WAKE_ATTENTION && power->automatic && power->pickup_glance ?
            power->automatic_return : previous;
        finish_automatic(power, now_ms);
        power->automatic_return = resume;
        power->automatic = true;
        power->pickup_glance = source == ORB_POWER_WAKE_PICKUP;
        power->pickup_budgeted = power->pickup_glance && !power->usb && power->preferences.brightness_pct > 0;
        power->automatic_ms = now_ms;
        if (!power->redact) { power->redact = true; orb_power_changed(power); }
    }
    if (source == ORB_POWER_WAKE_PICKUP) {
        power->have_pickup_wake = true;
        power->pickup_wake_ms = now_ms;
    }
    if (source == ORB_POWER_WAKE_ATTENTION) {
        power->have_attention = true;
        power->attention_ms = now_ms;
    }
    /* In-place touches and bursts never replay the wake. Critical conserves even
     * this bounded moment. Lower-power wake never blocks subsequent input. */
    if (lower_power && power->battery_mode != ORB_POWER_BATTERY_CRITICAL) {
        power->wake_from_sleep = was_sleep;
        power->moment = ORB_POWER_MOMENT_WAKE;
        power->moment_ms = now_ms;
        orb_power_changed(power);
    } else if (was_sleep) power->moment = ORB_POWER_MOMENT_NONE;
    result.accepted = true;
    result.consume_input = manual && consumed;
    return result;
}

bool orb_power_set_preferences(orb_power_controller_t *power, const orb_power_preferences_t *preferences, uint64_t now_ms)
{
    if (!power || !preferences || now_ms < power->now_ms) return false;
    if (!set_time(power, now_ms)) return false;
    orb_power_preferences_t next = *preferences;
    if (next.brightness_pct > 100) next.brightness_pct = 100;
    if (next.always_on_brightness_pct < 1) next.always_on_brightness_pct = 1;
    if (next.always_on_brightness_pct > 15) next.always_on_brightness_pct = 15;
    bool changed = next.always_on_brightness_pct != power->preferences.always_on_brightness_pct ||
        next.brightness_pct != power->preferences.brightness_pct ||
        next.always_on != power->preferences.always_on || next.motion_wake != power->preferences.motion_wake ||
        next.reduce_motion != power->preferences.reduce_motion;
    /* Retarget from the renderer's current values; changing accessibility must
     * not cut a retained element or suddenly switch the panel off. */
    if (next.reduce_motion != power->preferences.reduce_motion && power->moment != ORB_POWER_MOMENT_NONE)
        power->moment_ms = now_ms;
    power->preferences = next;
    if (changed) orb_power_changed(power);
    evaluate_idle(power);
    return true;
}

bool orb_power_set_battery(orb_power_controller_t *power, int pct, bool usb, bool charging, uint64_t now_ms)
{
    if (!power || pct < -1 || pct > 100 || now_ms < power->now_ms) return false;
    if (!set_time(power, now_ms)) return false;
    orb_power_battery_mode_t old = power->battery_mode;
    orb_power_battery_mode_t mode = ORB_POWER_BATTERY_NORMAL;
    if (!usb && pct >= 0) {
        if (pct <= 5 || (old == ORB_POWER_BATTERY_CRITICAL && pct < 8)) mode = ORB_POWER_BATTERY_CRITICAL;
        else if (pct <= 10 || (old >= ORB_POWER_BATTERY_VERY_LOW && pct < 13)) mode = ORB_POWER_BATTERY_VERY_LOW;
        else if (pct <= 20 || (old >= ORB_POWER_BATTERY_LOW && pct < 23)) mode = ORB_POWER_BATTERY_LOW;
    }
    if (power->battery_pct != pct || power->usb != usb || power->charging != charging || mode != old) orb_power_changed(power);
    power->battery_pct = pct;
    power->usb = usb;
    power->charging = charging;
    power->battery_mode = mode;
    if (usb) reset_pickup_budget(power);
    if (mode == ORB_POWER_BATTERY_CRITICAL && old != mode) orb_power_enter_sleep(power, true);
    evaluate_idle(power);
    return true;
}

bool orb_power_set_inhibitors(orb_power_controller_t *power, uint32_t inhibitors, uint64_t now_ms)
{
    if (!set_time(power, now_ms)) return false;
    if (power->inhibitors != inhibitors) {
        if (idle_inhibitors(power->inhibitors) != idle_inhibitors(inhibitors)) {
            if (power->automatic && now_ms - power->automatic_ms >= automatic_end(power))
                finish_automatic(power, add_deadline(power->automatic_ms, automatic_end(power)));
            /* Work ends with the existing fresh ordinary budget, even if it
             * began during an automatic glance. It never reveals content. */
            if (!idle_inhibitors(inhibitors)) finish_automatic(power, now_ms);
            power->activity_ms = now_ms;
        }
        power->inhibitors = inhibitors;
        orb_power_changed(power);
    }
    evaluate_idle(power);
    return true;
}

bool orb_power_set_quiet(orb_power_controller_t *power, bool quiet, uint64_t now_ms)
{
    if (!set_time(power, now_ms)) return false;
    if (!quiet) return orb_power_wake(power, ORB_POWER_WAKE_TOUCH, now_ms).accepted;
    power->quiet = true;
    finish_automatic(power, now_ms);
    orb_power_changed(power);
    if (power->motion.face_down || !power->preferences.always_on || power->battery_mode >= ORB_POWER_BATTERY_VERY_LOW)
        orb_power_enter_sleep(power, power->motion.face_down || power->battery_mode == ORB_POWER_BATTERY_CRITICAL);
    else {
        set_state(power, ORB_POWER_ALWAYS_ON);
        power->moment = ORB_POWER_MOMENT_REST;
        power->moment_ms = now_ms;
    }
    return true;
}

bool orb_power_imu_sample(orb_power_controller_t *power, float x, float y, float z, uint64_t now_ms)
{
    if (!power || now_ms < power->now_ms) return false;
    if (!set_time(power, now_ms)) return false;
    unsigned events = orb_power_motion_sample(&power->motion, x, y, z, now_ms);
    if (events & ORB_POWER_MOTION_DOWN) {
        orb_power_changed(power);
        orb_power_enter_sleep(power, true);
    }
    if (events & ORB_POWER_MOTION_UP) orb_power_changed(power);
    if (events & ORB_POWER_MOTION_PICKUP) (void)orb_power_wake(power, ORB_POWER_WAKE_PICKUP, now_ms);
    evaluate_idle(power);
    return !(events & ORB_POWER_MOTION_INVALID);
}

orb_power_snapshot_t orb_power_get_snapshot(const orb_power_controller_t *power)
{
    if (!power) return (orb_power_snapshot_t){ .state = ORB_POWER_SLEEP, .redact = true, .silent = true, .battery_pct = -1, .next_deadline_ms = UINT64_MAX };
    orb_power_snapshot_t out = { .state = power->state, .moment = power->moment,
        .battery_mode = power->battery_mode, .revision = power->revision,
        .now_ms = power->now_ms, .redact = power->redact, .silent = power->motion.face_down || power->quiet,
        .face_down = power->motion.face_down, .wake_from_sleep = power->wake_from_sleep, .battery_pct = power->battery_pct,
        .usb = power->usb, .charging = power->charging,
        .shutdown_requested = power->shutdown_requested,
        .low_power = power->battery_mode != ORB_POWER_BATTERY_NORMAL, .next_deadline_ms = UINT64_MAX };
    out.panel_on = power->state != ORB_POWER_SLEEP || power->moment == ORB_POWER_MOMENT_SLEEP;
    unsigned brightness = power->preferences.brightness_pct;
    unsigned cap = power->battery_mode == ORB_POWER_BATTERY_CRITICAL ? 15 : out.low_power ? 30 : 100;
    if (power->state == ORB_POWER_DIMMED) cap = 15;
    if (power->state == ORB_POWER_ALWAYS_ON || power->moment == ORB_POWER_MOMENT_SLEEP) cap = power->preferences.always_on_brightness_pct;
    out.brightness_pct = (uint8_t)(out.panel_on ? brightness < cap ? brightness : cap : 0);
    if (power->state == ORB_POWER_ALWAYS_ON || power->state == ORB_POWER_SLEEP) {
        static const int8_t offsets[4][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
        unsigned corner = (unsigned)(power->now_ms / 60000 % 4);
        out.shift_x = offsets[corner][0];
        out.shift_y = offsets[corner][1];
        if (power->state == ORB_POWER_ALWAYS_ON) {
            out.frame_interval_ms = 1000;
            out.next_deadline_ms = add_deadline(power->now_ms - power->now_ms % 1000, 1000);
        }
    }
    if (power->moment != ORB_POWER_MOMENT_NONE) {
        out.moment_duration_ms = moment_duration(power);
        out.moment_elapsed_ms = (uint32_t)(power->now_ms - power->moment_ms);
        out.frame_interval_ms = 16;
        out.next_deadline_ms = add_deadline(power->moment_ms, out.moment_duration_ms);
    }
    if (power->state != ORB_POWER_SLEEP && (power->quiet || power->display_locked || !idle_inhibitors(power->inhibitors) || power->battery_mode == ORB_POWER_BATTERY_CRITICAL)) {
        uint32_t dim, aod, sleep;
        idle_limits(power, &dim, &aod, &sleep);
        uint64_t elapsed = power->now_ms - power->activity_ms;
        uint32_t boundary = elapsed < dim ? dim : elapsed < aod ? aod : sleep;
        uint64_t deadline = add_deadline(power->activity_ms, boundary);
        if (power->automatic) {
            uint32_t active = power->pickup_glance ? 6000 : 20000;
            boundary = power->now_ms - power->automatic_ms < active ? active : automatic_end(power);
            deadline = add_deadline(power->automatic_ms, boundary);
        }
        if (deadline < out.next_deadline_ms) out.next_deadline_ms = deadline;
    }
    if (power->cutoff_pending && !power->shutdown_requested) {
        uint64_t deadline = add_deadline(power->cutoff_ms, 60000);
        if (deadline < out.next_deadline_ms) out.next_deadline_ms = deadline;
    }
    if (power->automatic && idle_inhibitors(power->inhibitors)) {
        uint64_t deadline = add_deadline(power->automatic_ms, automatic_end(power));
        if (deadline < out.next_deadline_ms) out.next_deadline_ms = deadline;
    }
    return out;
}

bool orb_power_frame_due(const orb_power_controller_t *power, uint64_t last_frame_ms, uint32_t last_revision)
{
    if (!power || last_frame_ms > power->now_ms) return false;
    if (power->moment != ORB_POWER_MOMENT_NONE) return power->now_ms - last_frame_ms >= 16 || last_revision != power->revision;
    if (power->state == ORB_POWER_ALWAYS_ON) {
        uint64_t end = add_deadline(power->moment_ms, moment_duration(power));
        return last_revision == 0 || power->now_ms - last_frame_ms >= 1000 ||
            (last_frame_ms < end && power->now_ms >= end && last_revision != power->revision);
    }
    return power->revision != last_revision;
}
