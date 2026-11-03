#include "orb_time.h"
#include "orb_time_internal.h"
#include "orb_sound.h"
#if defined(ESP_PLATFORM) || defined(ORB_TIME_TEST_STORAGE)
#include "orb_fs.h"
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&s_lock)
#define UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() ((void)pthread_mutex_lock(&s_lock))
#define UNLOCK() ((void)pthread_mutex_unlock(&s_lock))
#endif

/* Deadlines belong to the service, never the visible app. Keeping only this
 * small static model means leaving Time cannot cancel a timer (PLAN Stream F).
 * No lock protects an I/O operation, sound call, allocation or UI operation. */
static struct {
    orb_time_snapshot_t view;
    int64_t now, timer_started, stopwatch_started, stopwatch_accumulated;
    int64_t last_lap, alarm_snooze_until, save_after;
    unsigned finish_pending;
    int alarm_day, alarm_last_day;
    char zone[24];
    uint32_t zone_rules;
    bool persist_pending, restored_snooze, clock_override;
} s_time = {.view = {.timer_duration_us = INT64_C(300000000),
    .timer_remaining_us = INT64_C(300000000), .alarm_hour = 7,
    .alarm_snooze_seconds = 540, .hour24 = true}};

static orb_reminder_t s_reminders[ORB_TIME_REMINDERS_MAX];
static unsigned s_reminder_count = 0;
static uint32_t s_next_reminder_id = 1;

static void changed(void) { ++s_time.view.generation; }
static void durable_changed(void)
{
    s_time.persist_pending = true;
    s_time.save_after = s_time.now <= INT64_MAX - 500000 ? s_time.now + 500000 : INT64_MAX;
    s_time.view.saving = true;
    s_time.view.save_error = false;
    changed();
}

static void reminder_summary(void)
{
    s_time.view.reminder_due = false;
    s_time.view.reminder_count = 0;
    memset(&s_time.view.active_reminder, 0, sizeof(s_time.view.active_reminder));
    for (unsigned i = 0; i < s_reminder_count; ++i) {
        orb_reminder_t *r = &s_reminders[i];
        if (r->done) continue;
        ++s_time.view.reminder_count;
        if (r->due && (!s_time.view.reminder_due || r->due_time < s_time.view.active_reminder.due_time)) {
            s_time.view.active_reminder = *r;
            s_time.view.reminder_due = true;
        }
    }
}

static int64_t observe(int64_t now)
{
    /* An old main poll can race a newer UI event. A timestamp must never make
     * a countdown grow, and a negative fake timestamp is not elapsed time. */
    if (now > s_time.now) s_time.now = now;
    return s_time.now;
}

static int64_t stopwatch_elapsed(int64_t now)
{
    int64_t elapsed = s_time.stopwatch_accumulated;
    if (s_time.view.stopwatch_state == ORB_TIME_RUNNING) {
        int64_t delta = now - s_time.stopwatch_started;
        if (delta >= ORB_TIME_STOPWATCH_MAX_US - elapsed) return ORB_TIME_STOPWATCH_MAX_US;
        elapsed += delta;
    }
    return elapsed;
}

static void update(int64_t now)
{
    now = observe(now);
    if (s_time.alarm_snooze_until && now >= s_time.alarm_snooze_until) {
        s_time.alarm_snooze_until = 0;
        s_time.view.alarm_enabled = s_time.view.alarm_days != 0;
        s_time.view.alarm_next = 0;
        s_time.alarm_day = 0;
        s_time.view.alarm_ringing = true;
        s_time.finish_pending |= 2u;
        durable_changed();
    }
    if (s_time.view.timer_state == ORB_TIME_RUNNING) {
        int64_t elapsed = now - s_time.timer_started;
        if (elapsed >= s_time.view.timer_remaining_us) {
            s_time.view.timer_remaining_us = 0;
            s_time.view.timer_state = ORB_TIME_FINISHED;
            s_time.finish_pending |= 1u;
            changed();
        }
    }
    if (s_time.view.stopwatch_state == ORB_TIME_RUNNING &&
        stopwatch_elapsed(now) >= ORB_TIME_STOPWATCH_MAX_US) {
        s_time.stopwatch_accumulated = ORB_TIME_STOPWATCH_MAX_US;
        s_time.view.stopwatch_state = ORB_TIME_PAUSED;
        changed();
    }
}

void orb_time_set_clock_format(bool hour24)
{
    LOCK();
    if (s_time.view.hour24 != hour24 || !s_time.clock_override) {
        s_time.view.hour24 = hour24; s_time.clock_override = true; durable_changed();
    }
    UNLOCK();
}

void orb_time_set_clock_default(bool hour24)
{
    LOCK();
    if (!s_time.clock_override && s_time.view.hour24 != hour24) {
        s_time.view.hour24 = hour24; durable_changed();
    }
    UNLOCK();
}

void orb_time_format_clock(char *out, size_t capacity, unsigned hour, unsigned minute)
{
    if (!out || !capacity) return;
    LOCK(); bool hour24 = s_time.view.hour24; UNLOCK();
    if (hour24) snprintf(out, capacity, "%02u:%02u", hour % 24, minute % 60);
    else {
        struct tm value = {.tm_hour = (int)(hour % 24), .tm_min = (int)(minute % 60)};
        char period[24] = "";
        (void)strftime(period, sizeof(period), "%p", &value);
        snprintf(out, capacity, "%u:%02u%s%s", hour % 12 ? hour % 12 : 12,
            minute % 60, period[0] ? " " : "", period);
    }
}

static void persist_load(void);

void orb_time_init(void)
{
    LOCK();
    memset(&s_time, 0, sizeof(s_time));
    memset(s_reminders, 0, sizeof(s_reminders));
    s_reminder_count = 0;
    s_next_reminder_id = 1;
    s_time.view.timer_duration_us = INT64_C(300000000);
    s_time.view.timer_remaining_us = s_time.view.timer_duration_us;
    s_time.view.alarm_hour = 7;
    s_time.view.alarm_snooze_seconds = 540;
    s_time.view.hour24 = true;
    UNLOCK();
    persist_load();
}

int64_t orb_time_now_us(void)
{
    LOCK();
    int64_t now = s_time.now;
    UNLOCK();
    return now;
}

void orb_time_poll(int64_t now_us, time_t wall_now)
{
    bool valid = orb_time_clock_valid(wall_now);
    /* Civil conversion may take libc locks. Capture its small input, convert
     * outside the spinlock and discard a calculation raced by a command. */
    LOCK();
    if (valid && s_time.restored_snooze) {
        int64_t remaining = (int64_t)s_time.view.alarm_next - (int64_t)wall_now;
        remaining = remaining > 86400 ? 86400 : remaining;
        int64_t base = now_us > s_time.now ? now_us : s_time.now;
        if (remaining > 0 && base <= INT64_MAX - remaining * 1000000) {
            s_time.alarm_snooze_until = base + remaining * 1000000;
        } else {
            s_time.view.alarm_enabled = s_time.view.alarm_days != 0;
            s_time.view.alarm_next = 0; s_time.alarm_day = 0;
            s_time.view.alarm_ringing = true; s_time.finish_pending |= 2u;
            durable_changed();
        }
        s_time.restored_snooze = false;
    }
    update(now_us);
    orb_time_snapshot_t before = s_time.view;
    int day = s_time.alarm_day, last_day = s_time.alarm_last_day;
    bool snoozed = s_time.alarm_snooze_until != 0;
    char old_zone[sizeof(s_time.zone)]; memcpy(old_zone, s_time.zone, sizeof(old_zone));
    uint32_t old_rules = s_time.zone_rules;
    UNLOCK();
    char zone[sizeof(s_time.zone)] = "";
    uint32_t rules = UINT32_C(2166136261);
    const char *tz = getenv("TZ");
    if (tz) while (*tz) rules = (rules ^ (unsigned char)*tz++) * UINT32_C(16777619);
    struct tm local;
    if (valid && localtime_r(&wall_now, &local)) (void)strftime(zone, sizeof(zone), "%Z%z", &local);
    time_t next = before.alarm_next;
    if (valid && before.alarm_enabled && !snoozed) {
        if (!next || (orb_time_clock_valid(before.clock_now) && wall_now < before.clock_now))
            next = orb_time_next_alarm(wall_now, before.alarm_hour, before.alarm_minute,
                before.alarm_days, last_day, &day);
        else if (day && (strcmp(old_zone, zone) || old_rules != rules) &&
            orb_time_civil_alarm(day, before.alarm_hour, before.alarm_minute) != next) {
            /* A timezone relocation changes the next matching local date as
             * well as its UTC offset. Reusing the old civil date could ring
             * immediately after an eastward move, or skip today westward.
             * Natural DST changes preserve the already calculated instant,
             * including an alarm due on the offset-change boundary. */
            next = orb_time_next_alarm(wall_now, before.alarm_hour, before.alarm_minute,
                before.alarm_days, last_day, &day);
        }
    }
    bool firing = valid && before.alarm_enabled && !snoozed && next && wall_now >= next;
    int following_day = day;
    time_t following = firing && before.alarm_days ? orb_time_next_alarm(wall_now, before.alarm_hour,
        before.alarm_minute, before.alarm_days, day, &following_day) : 0;
    LOCK();
    /* A simultaneous UI command owns its newer schedule. Next poll reconciles. */
    bool same = s_time.view.generation == before.generation;
    if (valid != s_time.view.clock_valid) { s_time.view.clock_valid = valid; changed(); }
    s_time.view.clock_now = wall_now;
    /* Retain the old zone when a command invalidates this calculation, so
     * the next poll retries its schedule instead of losing the zone change. */
    if (valid && same) { memcpy(s_time.zone, zone, sizeof(zone)); s_time.zone_rules = rules; }
    if (same && valid && before.alarm_enabled && !snoozed) {
        bool schedule_changed = s_time.view.alarm_next != next || s_time.alarm_day != day;
        s_time.view.alarm_next = next;
        s_time.alarm_day = day;
        if (firing) {
            s_time.alarm_last_day = day;
            s_time.view.alarm_enabled = before.alarm_days != 0;
            s_time.view.alarm_next = following;
            s_time.alarm_day = following_day;
            if (!s_time.view.alarm_ringing) {
                s_time.view.alarm_ringing = true;
                s_time.finish_pending |= 2u;
            }
            durable_changed();
        } else if (schedule_changed) durable_changed();
    }
    if (valid) {
        for (unsigned i = 0; i < s_reminder_count; ++i) {
            if (!s_reminders[i].done && !s_reminders[i].due && wall_now >= s_reminders[i].due_time) {
                s_reminders[i].due = true;
                s_time.finish_pending |= 4u;
                durable_changed();
            }
        }
    }
    reminder_summary();
    unsigned finish = s_time.finish_pending;
    s_time.finish_pending = 0;
    UNLOCK();
    if (finish & 1u) (void)orb_sound_play(ORB_SOUND_TIMER);
    if (finish & 2u) (void)orb_sound_play(ORB_SOUND_ALARM);
    if (finish & 4u) (void)orb_sound_play(ORB_SOUND_NEEDS_YOU);
    /* Dismiss can race the sound submission. Revoke an obsolete loop after
     * submission as well, so this race can never leave an invisible alarm. */
    LOCK();
    bool timer_active = s_time.view.timer_state == ORB_TIME_FINISHED;
    bool alarm_active = s_time.view.alarm_ringing;
    UNLOCK();
    if ((finish & 1u) && !timer_active) orb_sound_stop(ORB_SOUND_TIMER);
    if ((finish & 2u) && !alarm_active) orb_sound_stop(ORB_SOUND_ALARM);
}

void orb_time_snapshot(int64_t now_us, orb_time_snapshot_t *out)
{
    if (!out) return;
    LOCK();
    update(now_us);
    *out = s_time.view;
    if (out->timer_state == ORB_TIME_RUNNING)
        out->timer_remaining_us -= s_time.now - s_time.timer_started;
    out->stopwatch_elapsed_us = stopwatch_elapsed(s_time.now);
    UNLOCK();
}

int64_t orb_time_next_deadline_us(int64_t now_us, int64_t wall_us)
{
    LOCK();
    int64_t next = INT64_MAX;
    if (s_time.view.timer_state == ORB_TIME_RUNNING)
        next = s_time.timer_started <= INT64_MAX - s_time.view.timer_remaining_us ?
            s_time.timer_started + s_time.view.timer_remaining_us : INT64_MAX;
    if (s_time.alarm_snooze_until && s_time.alarm_snooze_until < next)
        next = s_time.alarm_snooze_until;
    time_t civil = s_time.view.alarm_enabled && !s_time.alarm_snooze_until ? s_time.view.alarm_next : 0;
    for (unsigned i = 0; i < s_reminder_count; ++i) {
        const orb_reminder_t *r = &s_reminders[i];
        if (!r->done && !r->due && (!civil || r->due_time < civil)) civil = r->due_time;
    }
    if (civil && orb_time_clock_valid((time_t)(wall_us / 1000000))) {
        int64_t left = (int64_t)civil * 1000000 - wall_us;
        if (left <= 0) next = now_us;
        else if (now_us <= INT64_MAX - left && now_us + left < next) next = now_us + left;
    }
    UNLOCK();
    return next < now_us ? now_us : next;
}

bool orb_time_timer_set(uint32_t seconds)
{
    if (!seconds || seconds > ORB_TIME_TIMER_MAX_SECONDS) return false;
    LOCK();
    bool allowed = s_time.view.timer_state == ORB_TIME_IDLE;
    if (allowed && s_time.view.timer_duration_us != (int64_t)seconds * 1000000) {
        s_time.view.timer_duration_us = (int64_t)seconds * 1000000;
        s_time.view.timer_remaining_us = s_time.view.timer_duration_us;
        changed();
    }
    UNLOCK();
    return allowed;
}

bool orb_time_timer_start(int64_t now_us)
{
    LOCK();
    update(now_us);
    bool allowed = s_time.view.timer_state == ORB_TIME_IDLE || s_time.view.timer_state == ORB_TIME_PAUSED;
    if (allowed) {
        if (s_time.view.timer_state == ORB_TIME_IDLE) {
            s_time.view.timer_last_seconds = (uint32_t)(s_time.view.timer_duration_us / 1000000);
            durable_changed();
        }
        s_time.timer_started = s_time.now;
        s_time.view.timer_state = ORB_TIME_RUNNING;
        changed();
    }
    UNLOCK();
    return allowed;
}

void orb_time_timer_pause(int64_t now_us)
{
    LOCK();
    update(now_us);
    if (s_time.view.timer_state == ORB_TIME_RUNNING) {
        s_time.view.timer_remaining_us -= s_time.now - s_time.timer_started;
        s_time.view.timer_state = ORB_TIME_PAUSED;
        changed();
    }
    UNLOCK();
}

void orb_time_timer_reset(void)
{
    LOCK();
    if (s_time.view.timer_state != ORB_TIME_IDLE) {
        s_time.view.timer_state = ORB_TIME_IDLE;
        s_time.view.timer_remaining_us = s_time.view.timer_duration_us;
        /* A Reset before main's sound drain cancels that obsolete timer cue.
         * Preserve a concurrently ringing alarm's finish notification. */
        s_time.finish_pending &= ~1u;
        changed();
    }
    UNLOCK();
    orb_sound_stop(ORB_SOUND_TIMER);
}

bool orb_time_stopwatch_start(int64_t now_us)
{
    LOCK();
    update(now_us);
    bool allowed = s_time.view.stopwatch_state != ORB_TIME_RUNNING &&
        s_time.stopwatch_accumulated < ORB_TIME_STOPWATCH_MAX_US;
    if (allowed) {
        s_time.stopwatch_started = s_time.now;
        s_time.view.stopwatch_state = ORB_TIME_RUNNING;
        changed();
    }
    UNLOCK();
    return allowed;
}

void orb_time_stopwatch_pause(int64_t now_us)
{
    LOCK();
    update(now_us);
    if (s_time.view.stopwatch_state == ORB_TIME_RUNNING) {
        s_time.stopwatch_accumulated = stopwatch_elapsed(s_time.now);
        s_time.view.stopwatch_state = ORB_TIME_PAUSED;
        changed();
    }
    UNLOCK();
}

void orb_time_stopwatch_reset(void)
{
    LOCK();
    if (s_time.view.stopwatch_state != ORB_TIME_IDLE || s_time.view.lap_count) {
        s_time.view.stopwatch_state = ORB_TIME_IDLE;
        s_time.stopwatch_accumulated = 0;
        s_time.last_lap = 0;
        s_time.view.lap_count = s_time.view.lap_total = 0;
        memset(s_time.view.laps_us, 0, sizeof(s_time.view.laps_us));
        changed();
    }
    UNLOCK();
}

bool orb_time_stopwatch_lap(int64_t now_us)
{
    LOCK();
    update(now_us);
    bool allowed = s_time.view.stopwatch_state == ORB_TIME_RUNNING;
    if (allowed) {
        int64_t elapsed = stopwatch_elapsed(s_time.now);
        if (s_time.view.lap_count == ORB_TIME_LAPS_MAX) {
            memmove(s_time.view.laps_us, s_time.view.laps_us + 1,
                (ORB_TIME_LAPS_MAX - 1) * sizeof(s_time.view.laps_us[0]));
        } else ++s_time.view.lap_count;
        s_time.view.laps_us[s_time.view.lap_count - 1] = elapsed - s_time.last_lap;
        s_time.last_lap = elapsed;
        if (s_time.view.lap_total < UINT_MAX) ++s_time.view.lap_total;
        changed();
    }
    UNLOCK();
    return allowed;
}

static bool alarm_configure(unsigned hour, unsigned minute, orb_time_repeat_t repeat, uint8_t days, bool set_time)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        LOCK();
        /* Preserve the other command's independently editable fields from
         * the same generation used to validate the civil-time calculation. */
        if (set_time) { repeat = s_time.view.alarm_repeat; days = s_time.view.alarm_days; }
        else { hour = s_time.view.alarm_hour; minute = s_time.view.alarm_minute; }
        time_t wall = s_time.view.clock_now;
        bool enabled = s_time.view.alarm_enabled;
        uint32_t generation = s_time.view.generation;
        UNLOCK();
        int day = 0;
        time_t next = enabled ? orb_time_next_alarm(wall, hour, minute, days, 0, &day) : 0;
        LOCK();
        if (generation != s_time.view.generation || wall != s_time.view.clock_now) { UNLOCK(); continue; }
        bool difference = hour != s_time.view.alarm_hour || minute != s_time.view.alarm_minute ||
            repeat != s_time.view.alarm_repeat || days != s_time.view.alarm_days;
        if (difference) {
            s_time.alarm_snooze_until = 0;
            s_time.restored_snooze = false;
            s_time.alarm_last_day = 0;
            s_time.alarm_day = day;
            s_time.view.alarm_hour = hour;
            s_time.view.alarm_minute = minute;
            s_time.view.alarm_repeat = repeat;
            s_time.view.alarm_days = days;
            s_time.view.alarm_next = next;
            durable_changed();
        }
        UNLOCK();
        return true;
    }
    return false;
}

bool orb_time_alarm_set(unsigned hour, unsigned minute)
{
    if (hour > 23 || minute > 59) return false;
    return alarm_configure(hour, minute, ORB_TIME_REPEAT_ONCE, 0, true);
}

bool orb_time_alarm_repeat_set(orb_time_repeat_t repeat, uint8_t days)
{
    if ((unsigned)repeat > ORB_TIME_REPEAT_CUSTOM ||
        (repeat == ORB_TIME_REPEAT_CUSTOM && (!days || (days & ~ORB_TIME_DAYS_ALL)))) return false;
    return alarm_configure(0, 0, repeat, orb_time_repeat_days(repeat, days), false);
}

bool orb_time_alarm_snooze_set(uint32_t seconds)
{
    if (seconds < 60 || seconds > 3600) return false;
    LOCK();
    if (s_time.view.alarm_snooze_seconds != seconds) {
        s_time.view.alarm_snooze_seconds = seconds;
        durable_changed();
    }
    UNLOCK();
    return true;
}

bool orb_time_alarm_enable(bool enabled, time_t wall_now)
{
    LOCK();
    unsigned hour = s_time.view.alarm_hour, minute = s_time.view.alarm_minute;
    uint8_t days = s_time.view.alarm_days;
    int last_day = s_time.alarm_last_day;
    UNLOCK();
    int day = 0;
    time_t next = enabled ? orb_time_next_alarm(wall_now, hour, minute, days, last_day, &day) : 0;
    LOCK();
    bool valid = orb_time_clock_valid(wall_now);
    bool allowed = !enabled || (next && hour == s_time.view.alarm_hour &&
        minute == s_time.view.alarm_minute && days == s_time.view.alarm_days);
    bool difference = s_time.view.clock_valid != valid;
    s_time.view.clock_valid = valid;
    s_time.view.clock_now = wall_now;
    if (allowed) {
        s_time.alarm_snooze_until = 0;
        s_time.restored_snooze = false;
    }
    if (allowed && (enabled != s_time.view.alarm_enabled || (enabled && next != s_time.view.alarm_next))) {
        s_time.view.alarm_enabled = enabled;
        s_time.view.alarm_next = next;
        s_time.alarm_day = day;
        durable_changed();
    } else if (difference) changed();
    UNLOCK();
    return allowed;
}

bool orb_time_alarm_snooze(uint32_t seconds)
{
    if (!seconds || seconds > 86400u) return false;
    LOCK();
    int64_t delay = (int64_t)seconds * 1000000;
    bool allowed = s_time.view.alarm_ringing && s_time.now <= INT64_MAX - delay;
    if (allowed) {
        /* Snooze is monotonic while awake; persist its wall deadline as the
         * best available recovery across reboot (this board has no RTC). */
        s_time.alarm_snooze_until = s_time.now + delay;
        s_time.view.alarm_ringing = false;
        s_time.view.alarm_enabled = true;
        s_time.view.alarm_next = s_time.view.clock_valid ? s_time.view.clock_now + seconds : 0;
        s_time.finish_pending &= ~2u;
        durable_changed();
    }
    UNLOCK();
    if (allowed) orb_sound_stop(ORB_SOUND_ALARM);
    return allowed;
}

void orb_time_dismiss(bool timer, bool alarm)
{
    if (timer) orb_time_timer_reset();
    LOCK();
    if (alarm && s_time.view.alarm_ringing) {
        s_time.view.alarm_ringing = false;
        s_time.finish_pending &= ~2u;
        durable_changed();
    }
    UNLOCK();
    if (alarm) orb_sound_stop(ORB_SOUND_ALARM);
}

bool orb_time_reminder_set(const char *title, time_t due_time, uint32_t *out_id)
{
    if (!title || !*title || !orb_time_clock_valid(due_time)) return false;
    char safe_title[sizeof(s_reminders[0].title)] = {0};
    orb_time_copy_utf8(safe_title, sizeof(safe_title), title);
    if (!safe_title[0]) return false;
    LOCK();
    if (!s_time.view.clock_valid) { UNLOCK(); return false; }
    unsigned slot = s_reminder_count;
    for (unsigned i = 0; i < s_reminder_count; ++i)
        if (s_reminders[i].done) { slot = i; break; }
    if (slot < s_reminder_count) {
        memmove(s_reminders + slot, s_reminders + slot + 1,
            (s_reminder_count - slot - 1) * sizeof(s_reminders[0]));
        slot = --s_reminder_count;
    }
    if (slot >= ORB_TIME_REMINDERS_MAX) { UNLOCK(); return false; }
    ++s_reminder_count;
    orb_reminder_t *rem = &s_reminders[slot];
    memset(rem, 0, sizeof(*rem));
    /* Wrap cannot produce ID zero (the public current-due sentinel) or reuse
     * any extant record. The store is bounded at eight, so search is cheap. */
    bool duplicate;
    do {
        if (!s_next_reminder_id) ++s_next_reminder_id;
        rem->id = s_next_reminder_id++;
        duplicate = false;
        for (unsigned i = 0; i < s_reminder_count; ++i)
            if (i != slot && s_reminders[i].id == rem->id) duplicate = true;
    } while (duplicate);
    memcpy(rem->title, safe_title, sizeof(safe_title));
    rem->due_time = due_time;
    rem->due = s_time.view.clock_now >= due_time;
    if (rem->due) s_time.finish_pending |= 4u;
    if (out_id) *out_id = rem->id;
    reminder_summary();
    durable_changed();
    UNLOCK();
    return true;
}

bool orb_time_reminder_get(unsigned index, orb_reminder_t *out)
{
    if (!out) return false;
    LOCK();
    bool found = false;
    if (index < s_time.view.reminder_count) {
        /* Rank without copying the entire array into each visible view. */
        for (unsigned i = 0; i < s_reminder_count; ++i) {
            if (s_reminders[i].done) continue;
            unsigned rank = 0;
            for (unsigned j = 0; j < s_reminder_count; ++j) {
                if (s_reminders[j].done) continue;
                if (s_reminders[j].due_time < s_reminders[i].due_time ||
                    (s_reminders[j].due_time == s_reminders[i].due_time && j < i)) ++rank;
            }
            if (rank == index) { *out = s_reminders[i]; found = true; break; }
        }
    }
    UNLOCK();
    return found;
}

bool orb_time_reminder_snooze(uint32_t id, uint32_t seconds)
{
    if (!seconds || seconds > 86400u) return false;
    LOCK();
    if (!id) id = s_time.view.active_reminder.id;
    for (unsigned i = 0; i < s_reminder_count; ++i) {
        orb_reminder_t *rem = &s_reminders[i];
        if (!rem->done && rem->id == id) {
            /* Every deadline we create must satisfy the same range accepted
             * on load; otherwise this action would invalidate all Time state
             * at the next reboot. Valid clocks are already far below INT64_MAX. */
            if (!s_time.view.clock_valid || s_time.view.clock_now > (time_t)(ORB_TIME_CLOCK_MAX - seconds)) break;
            /* An overdue reminder snoozes from now, never from its stale due
             * date: otherwise "In 1 hour" can ring again immediately. */
            rem->due_time = s_time.view.clock_now + (time_t)seconds;
            rem->due = false;
            reminder_summary();
            if (!s_time.view.reminder_due) s_time.finish_pending &= ~4u;
            durable_changed();
            UNLOCK();
            return true;
        }
    }
    UNLOCK();
    return false;
}

bool orb_time_reminder_done(uint32_t id)
{
    LOCK();
    if (!id) id = s_time.view.active_reminder.id;
    for (unsigned i = 0; i < s_reminder_count; ++i) {
        orb_reminder_t *rem = &s_reminders[i];
        if (!rem->done && rem->id == id) {
            rem->done = true; rem->due = false;
            reminder_summary();
            if (!s_time.view.reminder_due) s_time.finish_pending &= ~4u;
            durable_changed();
            UNLOCK();
            return true;
        }
    }
    UNLOCK();
    return false;
}

void orb_time_reminder_clear_all(void)
{
    LOCK();
    memset(s_reminders, 0, sizeof(s_reminders));
    s_reminder_count = 0;
    s_next_reminder_id = 1;
    s_time.finish_pending &= ~4u;
    reminder_summary();
    durable_changed();
    UNLOCK();
}

static void persist_load(void)
{
#if defined(ESP_PLATFORM) || defined(ORB_TIME_TEST_STORAGE)
    unsigned char record[RECORD_SIZE];
    size_t size = 0;
    if (!orb_fs_ready() || orb_fs_app_prepare("time") != ESP_OK) {
        LOCK(); s_time.view.save_error = true; UNLOCK(); return;
    }
    esp_err_t result = orb_fs_read("time", "state-v1", record, sizeof(record), &size);
    if (result == ESP_ERR_NOT_FOUND) return;
    if (result != ESP_OK || size != sizeof(record) || !orb_time_record_valid(record)) {
        LOCK(); s_time.view.save_error = true; UNLOCK(); return;
    }
    LOCK();
    s_time.view.alarm_hour = record[5]; s_time.view.alarm_minute = record[6];
    s_time.view.alarm_repeat = (orb_time_repeat_t)record[7]; s_time.view.alarm_days = record[8];
    s_time.view.alarm_enabled = (record[9] & 1u) != 0;
    s_time.view.alarm_ringing = (record[9] & 2u) != 0;
    s_time.view.hour24 = (record[9] & 4u) != 0;
    s_time.restored_snooze = (record[9] & 8u) != 0;
    s_time.clock_override = (record[9] & 16u) != 0;
    s_time.view.alarm_snooze_seconds = orb_time_get32(record + 12);
    s_time.view.timer_last_seconds = orb_time_get32(record + 16);
    if (s_time.view.timer_last_seconds) {
        s_time.view.timer_duration_us = (int64_t)s_time.view.timer_last_seconds * 1000000;
        s_time.view.timer_remaining_us = s_time.view.timer_duration_us;
    }
    s_next_reminder_id = orb_time_get32(record + 20);
    s_time.view.alarm_next = (time_t)orb_time_get64(record + 24);
    s_time.alarm_day = (int)orb_time_get32(record + 32); s_time.alarm_last_day = (int)orb_time_get32(record + 36);
    s_reminder_count = record[10];
    for (unsigned i = 0; i < s_reminder_count; ++i) {
        const unsigned char *r = record + RECORD_HEADER + i * RECORD_REMINDER;
        s_reminders[i].id = orb_time_get32(r); s_reminders[i].due_time = (time_t)orb_time_get64(r + 4);
        orb_time_copy_utf8(s_reminders[i].title, sizeof(s_reminders[i].title), (const char *)r + 12);
        s_reminders[i].done = (r[76] & 1u) != 0; s_reminders[i].due = (r[76] & 2u) != 0;
    }
    reminder_summary();
    if (s_time.view.alarm_ringing) s_time.finish_pending |= 2u;
    if (s_time.view.reminder_due) s_time.finish_pending |= 4u;
    changed();
    UNLOCK();
#endif
}

void orb_time_persist_service(void)
{
#if defined(ESP_PLATFORM) || defined(ORB_TIME_TEST_STORAGE)
    /* Main is the only caller. The 684-byte bounded stack record replaces an
     * extra task, queue and heap buffer. Never call this under the LVGL lock. */
    unsigned char record[RECORD_SIZE] = {'O', 'R', 'B', 'T', 1};
    LOCK();
    if (!s_time.persist_pending || s_time.now < s_time.save_after) { UNLOCK(); return; }
    s_time.persist_pending = false;
    record[5] = (unsigned char)s_time.view.alarm_hour;
    record[6] = (unsigned char)s_time.view.alarm_minute;
    record[7] = (unsigned char)s_time.view.alarm_repeat; record[8] = s_time.view.alarm_days;
    record[9] = (unsigned char)((s_time.view.alarm_enabled ? 1 : 0) | (s_time.view.alarm_ringing ? 2 : 0) |
        (s_time.view.hour24 ? 4 : 0) | (s_time.alarm_snooze_until || s_time.restored_snooze ? 8 : 0) |
        (s_time.clock_override ? 16 : 0));
    record[10] = (unsigned char)s_reminder_count;
    orb_time_put32(record + 12, s_time.view.alarm_snooze_seconds);
    orb_time_put32(record + 16, s_time.view.timer_last_seconds);
    orb_time_put32(record + 20, s_next_reminder_id);
    orb_time_put64(record + 24, (uint64_t)s_time.view.alarm_next);
    orb_time_put32(record + 32, (uint32_t)s_time.alarm_day); orb_time_put32(record + 36, (uint32_t)s_time.alarm_last_day);
    for (unsigned i = 0; i < s_reminder_count; ++i) {
        unsigned char *r = record + RECORD_HEADER + i * RECORD_REMINDER;
        orb_time_put32(r, s_reminders[i].id); orb_time_put64(r + 4, (uint64_t)s_reminders[i].due_time);
        memcpy(r + 12, s_reminders[i].title, sizeof(s_reminders[i].title));
        r[76] = (unsigned char)((s_reminders[i].done ? 1 : 0) | (s_reminders[i].due ? 2 : 0));
    }
    UNLOCK();
    orb_time_put32(record + RECORD_SIZE - 4, orb_time_checksum(record));
    esp_err_t result = !orb_fs_ready() ? ESP_ERR_INVALID_STATE : orb_fs_app_prepare("time");
    if (result == ESP_OK) result = orb_fs_write("time", "state-v1", record, sizeof(record));
    LOCK();
    s_time.view.saving = s_time.persist_pending;
    s_time.view.save_error = result != ESP_OK;
    changed();
    UNLOCK();
#else
    LOCK();
    if (s_time.persist_pending) {
        s_time.persist_pending = false; s_time.view.saving = false; changed();
    }
    UNLOCK();
#endif
}

void orb_time_persist_retry(void)
{
    LOCK();
    if (s_time.view.save_error) {
        s_time.persist_pending = true; s_time.save_after = s_time.now;
        s_time.view.saving = true; changed();
    }
    UNLOCK();
}
