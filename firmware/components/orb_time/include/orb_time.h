#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define ORB_TIME_LAPS_MAX 16u
#define ORB_TIME_REMINDERS_MAX 8u
#define ORB_TIME_TIMER_MAX_SECONDS 86400u
#define ORB_TIME_STOPWATCH_MAX_US INT64_C(359999999000)
#define ORB_TIME_CLOCK_MIN INT64_C(1577836800)
#define ORB_TIME_CLOCK_MAX INT64_C(253402300799)

typedef enum {
    ORB_TIME_IDLE, ORB_TIME_RUNNING, ORB_TIME_PAUSED, ORB_TIME_FINISHED,
} orb_time_state_t;

typedef enum {
    ORB_TIME_REPEAT_ONCE, ORB_TIME_REPEAT_WEEKDAYS, ORB_TIME_REPEAT_WEEKENDS,
    ORB_TIME_REPEAT_DAILY, ORB_TIME_REPEAT_CUSTOM,
} orb_time_repeat_t;

/* Day masks use libc's weekday order: Sunday = bit 0, Saturday = bit 6. */
#define ORB_TIME_DAYS_WEEKDAYS UINT8_C(0x3e)
#define ORB_TIME_DAYS_WEEKENDS UINT8_C(0x41)
#define ORB_TIME_DAYS_ALL UINT8_C(0x7f)

typedef struct {
    uint32_t id;
    char title[64];
    time_t due_time;
    bool done, due;
} orb_reminder_t;

typedef struct {
    uint32_t generation; /* State changes only; never a periodic invalidation. */
    orb_time_state_t timer_state;
    int64_t timer_duration_us, timer_remaining_us;
    uint32_t timer_last_seconds;
    orb_time_state_t stopwatch_state;
    int64_t stopwatch_elapsed_us;
    unsigned lap_count, lap_total;
    /* Chronological retained splits, in microseconds, newest at count - 1. */
    int64_t laps_us[ORB_TIME_LAPS_MAX];
    unsigned alarm_hour, alarm_minute;
    orb_time_repeat_t alarm_repeat;
    uint8_t alarm_days;
    uint32_t alarm_snooze_seconds;
    bool alarm_enabled, alarm_ringing, clock_valid;
    bool hour24, saving, save_error;
    time_t alarm_next, clock_now; /* Last injected wall-clock sample. */
    orb_reminder_t active_reminder;
    bool reminder_due;
    unsigned reminder_count; /* Outstanding entries; no per-view array allocation. */
} orb_time_snapshot_t;

/* Device-lifetime, allocation-free service. Init is an explicit reset for boot
 * and isolated tests; app entry/exit and screen sleep never call it. All API
 * commands are thread-safe and never touch LVGL, flash, the hub or network.
 * Init loads the durable record once after app storage is mounted. */
void orb_time_init(void);
/* Main-task only: flush a coalesced atomic app-storage record outside LVGL and
 * the model lock. A failed save stays visible and can be retried explicitly. */
void orb_time_persist_service(void);
void orb_time_persist_retry(void);
/* Main calls this even while another app is active or the screen is asleep.
 * Finish cues are attempted once, outside the state lock, through orb_sound. */
void orb_time_poll(int64_t now_us, time_t wall_now);
/* Absolute monotonic service deadline, INT64_MAX when none. Query after poll;
 * callers still poll wall-clock schedules at least once/second for clock jumps. */
int64_t orb_time_next_deadline_us(int64_t now_us, int64_t wall_us);
/* Latest service timestamp: UI and fake-board clocks use the same epoch. */
int64_t orb_time_now_us(void);
void orb_time_snapshot(int64_t now_us, orb_time_snapshot_t *out);

/* Glanceable elapsed/split text: M:SS below an hour, H:MM:SS thereafter.
 * Stopwatch readouts use orb_time_format_stopwatch. Fractions are truncated; countdown
 * callers round up before formatting. Negative values display zero, and a
 * missing/zero-capacity output is ignored. No service state is accessed. */
void orb_time_format_duration(char *out, size_t capacity, int64_t us, bool hundredths);
/* Stopwatch readouts (counter, pinned header, lap rows and splits) share one
 * precision: MM:SS.hh below an hour, H:MM:SS.hh from an hour, as Apple's
 * Stopwatch shows 02:03.14. Truncates; negative values display zero. */
void orb_time_format_stopwatch(char *out, size_t capacity, int64_t us);
void orb_time_set_clock_format(bool hour24);
/* Hello refreshes the Mac's default until the owner explicitly chooses a
 * format on Orb. The local choice survives reboot and wins on reconnect. */
void orb_time_set_clock_default(bool hour24);
void orb_time_format_clock(char *out, size_t capacity, unsigned hour, unsigned minute);
void orb_time_format_repeat(char *out, size_t capacity, orb_time_repeat_t repeat, uint8_t days);

bool orb_time_timer_set(uint32_t seconds); /* 1 second through 24 hours, idle only. */
bool orb_time_timer_start(int64_t now_us); /* Idle/paused; finished needs Reset. */
void orb_time_timer_pause(int64_t now_us);
void orb_time_timer_reset(void);
bool orb_time_stopwatch_start(int64_t now_us);
void orb_time_stopwatch_pause(int64_t now_us);
void orb_time_stopwatch_reset(void);
bool orb_time_stopwatch_lap(int64_t now_us);

/* Local wall-clock alarm, next matching civil day and hour/minute. A clock
 * supplied by the hub/SNTP is required to arm it; Timer/Stopwatch remain offline.
 * Navigation and timeouts never acknowledge a finish. Snooze is explicit and
 * uses monotonic elapsed time, preserving the configured civil alarm time. */
bool orb_time_alarm_set(unsigned hour, unsigned minute);
bool orb_time_alarm_repeat_set(orb_time_repeat_t repeat, uint8_t days);
bool orb_time_alarm_snooze_set(uint32_t seconds); /* 1 minute through 1 hour. */
bool orb_time_alarm_enable(bool enabled, time_t wall_now);
/* Reschedule a ringing alarm after 1..86400 seconds. No wall clock required. */
bool orb_time_alarm_snooze(uint32_t seconds);
void orb_time_dismiss(bool timer, bool alarm);

/* Local reminder store with voice-set path. Due reminders latch until explicit
 * Done or Snooze (+1 hour). Sound cue attempted once on due boundary. */
bool orb_time_reminder_set(const char *title, time_t due_time, uint32_t *out_id);
/* Outstanding reminders sorted by due time, then insertion order. */
bool orb_time_reminder_get(unsigned index, orb_reminder_t *out);
bool orb_time_reminder_snooze(uint32_t id, uint32_t seconds);
bool orb_time_reminder_done(uint32_t id);
void orb_time_reminder_clear_all(void);
