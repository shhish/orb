#include "orb_time.h"
#include "orb_apps.h"
#include "orb_fs.h"
#include "orb_sound.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks, s_cues;
static bool s_muted, s_dismiss_on_sound;
static bool s_storage_ready, s_storage_fail;
static unsigned s_writes, s_stops;
static unsigned char s_record[1024];
static size_t s_record_size;
#define CHECK(value) do { ++s_checks; assert(value); } while (0)

bool orb_sound_play(orb_sound_cue_t cue)
{
    assert(cue == ORB_SOUND_TIMER || cue == ORB_SOUND_ALARM || cue == ORB_SOUND_NEEDS_YOU);
    ++s_cues;
    if (s_dismiss_on_sound) orb_time_dismiss(cue == ORB_SOUND_TIMER, cue == ORB_SOUND_ALARM);
    return !s_muted;
}
bool orb_fs_ready(void) { return s_storage_ready; }
void orb_sound_stop(orb_sound_cue_t cue)
{
    assert(cue == ORB_SOUND_TIMER || cue == ORB_SOUND_ALARM);
    ++s_stops;
}
bool orb_fs_valid_app_id(const char *id) { return id && *id && strlen(id) <= ORB_FS_APP_ID_MAX; }
esp_err_t orb_fs_app_prepare(const char *id) { (void)id; return ESP_OK; }
esp_err_t orb_fs_read(const char *id, const char *key, void *out, size_t capacity, size_t *size)
{
    assert(!strcmp(id, "time") && !strcmp(key, "state-v1"));
    *size = s_record_size;
    if (!s_record_size) return ESP_ERR_NOT_FOUND;
    if (capacity < s_record_size) return ESP_ERR_INVALID_SIZE;
    memcpy(out, s_record, s_record_size);
    return ESP_OK;
}
esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size)
{
    assert(!strcmp(id, "time") && !strcmp(key, "state-v1") && size <= sizeof(s_record));
    ++s_writes;
    if (s_storage_fail) return ESP_FAIL;
    memcpy(s_record, data, size); s_record_size = size;
    return ESP_OK;
}

static orb_time_snapshot_t snapshot(int64_t now)
{
    orb_time_snapshot_t value;
    orb_time_snapshot(now, &value);
    return value;
}

static void duration_format(void)
{
    static const struct {
        int64_t us;
        const char *counter, *lap;
    } cases[] = {
        {-1, "0:00", "0:00.00"},
        {0, "0:00", "0:00.00"},
        {9999, "0:00", "0:00.00"},
        {10000, "0:00", "0:00.01"},
        {999999, "0:00", "0:00.99"},
        {1000000, "0:01", "0:01.00"},
        {INT64_C(59999999), "0:59", "0:59.99"},
        {INT64_C(60000000), "1:00", "1:00.00"},
        {INT64_C(82000000), "1:22", "1:22.00"},
        {INT64_C(210000000), "3:30", "3:30.00"},
        {INT64_C(300000000), "5:00", "5:00.00"},
        {INT64_C(600000000), "10:00", "10:00.00"},
        {INT64_C(3599999999), "59:59", "59:59.99"},
        {INT64_C(3600000000), "1:00:00", "1:00:00.00"},
        {INT64_C(3661999999), "1:01:01", "1:01:01.99"},
        {INT64_C(36000000000), "10:00:00", "10:00:00.00"},
        {INT64_C(86400000000), "24:00:00", "24:00:00.00"},
        {ORB_TIME_STOPWATCH_MAX_US, "99:59:59", "99:59:59.99"},
        {INT64_MAX, "2562047788:00:54", "2562047788:00:54.77"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char value[32];
        orb_time_format_duration(value, sizeof(value), cases[i].us, false);
        CHECK(!strcmp(value, cases[i].counter));
        orb_time_format_duration(value, sizeof(value), cases[i].us, true);
        CHECK(!strcmp(value, cases[i].lap));
        /* Every capacity preserves the formatted prefix and a terminator,
         * including truncation in either a digit group or the fraction. */
        for (size_t capacity = 1; capacity <= strlen(cases[i].lap) + 1; ++capacity) {
            memset(value, 'X', sizeof(value));
            orb_time_format_duration(value, capacity, cases[i].us, true);
            CHECK(value[capacity - 1] == '\0');
            CHECK(!strncmp(value, cases[i].lap, capacity - 1));
            CHECK(value[capacity] == 'X');
        }
    }
    char untouched = 'X';
    orb_time_format_duration(&untouched, 0, 0, false);
    CHECK(untouched == 'X');
    orb_time_format_duration(NULL, 0, INT64_MAX, true);
    orb_time_format_duration(NULL, 10, 0, false);
}

static void timer(void)
{
    orb_time_init();
    s_cues = 0;
    orb_time_snapshot_t s = snapshot(0);
    CHECK(s.timer_state == ORB_TIME_IDLE && s.timer_remaining_us == 300000000);
    CHECK(!orb_time_timer_set(0));
    CHECK(!orb_time_timer_set(ORB_TIME_TIMER_MAX_SECONDS + 1));
    CHECK(orb_time_timer_set(ORB_TIME_TIMER_MAX_SECONDS));
    CHECK(snapshot(0).timer_remaining_us == INT64_C(86400000000));
    CHECK(orb_time_timer_set(2));
    CHECK(orb_time_timer_start(1000000));
    CHECK(!orb_time_timer_start(1000001));
    CHECK(!orb_time_timer_set(3));
    CHECK(snapshot(1500000).timer_remaining_us == 1500000);
    orb_time_timer_pause(1500000);
    s = snapshot(100000000);
    CHECK(s.timer_state == ORB_TIME_PAUSED && s.timer_remaining_us == 1500000);
    CHECK(!orb_time_timer_set(3));
    CHECK(orb_time_timer_start(100000000));
    CHECK(snapshot(100750000).timer_remaining_us == 750000);
    CHECK(snapshot(100500000).timer_remaining_us == 750000); /* Stale poll cannot add time. */
    orb_time_poll(101500000, 0);
    s = snapshot(101500000);
    CHECK(s.timer_state == ORB_TIME_FINISHED && s.timer_remaining_us == 0);
    CHECK(s_cues == 1);
    CHECK(!orb_time_timer_start(101500000));
    uint32_t generation = s.generation;
    for (unsigned i = 0; i < 1000; ++i) {
        orb_time_poll(101500000 + i * 250000LL, 0);
        s = snapshot(101500000 + i * 250000LL);
        CHECK(s.timer_state == ORB_TIME_FINISHED && s.generation == generation);
    }
    CHECK(s_cues == 1);
    orb_time_dismiss(true, false);
    CHECK(snapshot(orb_time_now_us()).timer_state == ORB_TIME_IDLE);
    CHECK(snapshot(orb_time_now_us()).timer_remaining_us == 2000000);
    generation = snapshot(orb_time_now_us()).generation;
    orb_time_timer_reset();
    CHECK(snapshot(orb_time_now_us()).generation == generation);

    orb_time_init();
    s_muted = true;
    CHECK(orb_time_timer_set(1));
    CHECK(orb_time_timer_start(0));
    orb_time_poll(1000000, 0);
    CHECK(s_cues == 2 && snapshot(1000000).timer_state == ORB_TIME_FINISHED);
    s_muted = false;
    orb_time_poll(2000000, 0);
    CHECK(s_cues == 2); /* A Silent-dropped cue is never replayed. */

    orb_time_init();
    CHECK(orb_time_timer_set(1));
    CHECK(orb_time_timer_start(0));
    CHECK(snapshot(1000000).timer_state == ORB_TIME_FINISHED);
    orb_time_timer_reset();
    orb_time_poll(1000000, 0);
    CHECK(s_cues == 2); /* Reset before the sound drain cancels stale sound. */
}

static void sound_dismiss_race(void)
{
    orb_time_init();
    CHECK(orb_time_timer_set(1) && orb_time_timer_start(0));
    s_dismiss_on_sound = true;
    unsigned stops = s_stops;
    orb_time_poll(1000000, 0);
    CHECK(snapshot(1000000).timer_state == ORB_TIME_IDLE && s_stops >= stops + 2);
    s_dismiss_on_sound = false;
}

static void stopwatch(void)
{
    orb_time_init();
    CHECK(!orb_time_stopwatch_lap(0));
    CHECK(orb_time_stopwatch_start(0));
    CHECK(!orb_time_stopwatch_start(1));
    CHECK(orb_time_stopwatch_lap(1000000));
    CHECK(orb_time_stopwatch_lap(2500000));
    orb_time_stopwatch_pause(3000000);
    orb_time_snapshot_t s = snapshot(INT64_C(1000000000));
    CHECK(s.stopwatch_state == ORB_TIME_PAUSED && s.stopwatch_elapsed_us == 3000000);
    CHECK(s.lap_count == 2 && s.laps_us[0] == 1000000 && s.laps_us[1] == 1500000);
    CHECK(!orb_time_stopwatch_lap(INT64_C(1000000000)));
    CHECK(orb_time_stopwatch_start(INT64_C(1000000000)));
    CHECK(orb_time_stopwatch_lap(INT64_C(1001000000)));
    CHECK(snapshot(INT64_C(1001000000)).laps_us[2] == 1500000);
    for (unsigned i = 2; i <= 100; ++i)
        CHECK(orb_time_stopwatch_lap(INT64_C(1000000000) + (int64_t)i * 1000000));
    s = snapshot(INT64_C(1100000000));
    CHECK(s.lap_count == ORB_TIME_LAPS_MAX && s.lap_total == 102);
    for (unsigned i = 0; i < ORB_TIME_LAPS_MAX; ++i) CHECK(s.laps_us[i] == 1000000);
    orb_time_stopwatch_reset();
    s = snapshot(INT64_C(1100000000));
    CHECK(s.stopwatch_state == ORB_TIME_IDLE && !s.lap_count && !s.lap_total && !s.stopwatch_elapsed_us);

    CHECK(orb_time_stopwatch_start(INT64_C(1100000000)));
    s = snapshot(INT64_MAX);
    CHECK(s.stopwatch_state == ORB_TIME_PAUSED && s.stopwatch_elapsed_us == ORB_TIME_STOPWATCH_MAX_US);
    CHECK(!orb_time_stopwatch_start(INT64_MAX));
}

static void alarm(void)
{
    orb_time_init();
    s_cues = 0;
    CHECK(!orb_time_alarm_set(24, 0));
    CHECK(!orb_time_alarm_set(0, 60));
    CHECK(!orb_time_alarm_enable(true, (time_t)INT64_MAX));
    CHECK(!orb_time_alarm_enable(true, 0));
    CHECK(!snapshot(0).alarm_enabled && !snapshot(0).clock_valid);
    CHECK(orb_time_alarm_enable(false, 0));
    /* 2024-01-01 06:59:30 UTC; fixed synthetic time, no network or real clock. */
    time_t before = 1704092370;
    orb_time_poll(0, before);
    CHECK(orb_time_alarm_set(7, 0));
    CHECK(orb_time_alarm_enable(true, before));
    orb_time_snapshot_t s = snapshot(0);
    CHECK(s.clock_valid && s.alarm_enabled && !s.alarm_ringing);
    CHECK(s.clock_now == before && s.alarm_next == before + 30);
    orb_time_poll(1000000, before + 29);
    CHECK(!snapshot(1000000).alarm_ringing && s_cues == 0);
    orb_time_poll(2000000, before + 30);
    s = snapshot(2000000);
    CHECK(!s.alarm_enabled && s.alarm_ringing && s_cues == 1);
    for (unsigned i = 0; i < 10; ++i) orb_time_poll(2000000, before + 30 + i * 86400);
    CHECK(snapshot(2000000).alarm_ringing && s_cues == 1);
    CHECK(orb_time_alarm_enable(false, before + 31));
    CHECK(snapshot(2000000).alarm_ringing); /* Disabling is not acknowledgement. */
    orb_time_dismiss(false, true);
    CHECK(!snapshot(2000000).alarm_ringing);
    orb_time_poll(2000000, before + 31);
    CHECK(s_cues == 1);

    CHECK(orb_time_alarm_enable(true, before + 30));
    CHECK(snapshot(2000000).alarm_next == before + 30 + 86400);
    CHECK(orb_time_alarm_set(6, 59));
    CHECK(snapshot(2000000).alarm_next == before - 30 + 86400);
    /* Losing clock validity holds the armed deadline, and a later valid clock
     * rings it once after screen sleep or a wall-clock jump. */
    orb_time_poll(3000000, 0);
    CHECK(!snapshot(3000000).clock_valid && snapshot(3000000).alarm_enabled);
    orb_time_poll(4000000, before + 2 * 86400);
    CHECK(snapshot(4000000).alarm_ringing && s_cues == 2);

    /* Acknowledging another source cannot replay an already-drained cue. */
    CHECK(orb_time_timer_set(1));
    CHECK(orb_time_timer_start(4000000));
    orb_time_poll(5000000, before + 2 * 86400);
    CHECK(s_cues == 3);
    orb_time_dismiss(false, true);
    orb_time_poll(6000000, before + 2 * 86400);
    CHECK(s_cues == 3 && snapshot(6000000).timer_state == ORB_TIME_FINISHED);
}

static void alarm_snooze(void)
{
    orb_time_init();
    s_cues = 0;
    time_t before = 1735714740; /* 06:59 UTC. */
    orb_time_poll(0, before);
    CHECK(!orb_time_alarm_snooze(540));
    CHECK(orb_time_alarm_set(7, 0));
    CHECK(orb_time_alarm_enable(true, before));
    orb_time_poll(60000000, before + 60);
    CHECK(snapshot(60000000).alarm_ringing);
    CHECK(!orb_time_alarm_snooze(0));
    CHECK(!orb_time_alarm_snooze(86401));
    CHECK(orb_time_alarm_snooze(540));
    orb_time_snapshot_t s = snapshot(60000000);
    CHECK(s.alarm_enabled && !s.alarm_ringing && s.alarm_next == before + 600);
    CHECK(s.alarm_hour == 7 && s.alarm_minute == 0);
    /* A wall-clock correction cannot shorten a chosen snooze. */
    orb_time_poll(599000000, before + 86400);
    CHECK(!snapshot(599000000).alarm_ringing && s_cues == 1);
    /* Loss of wall time also cannot prevent the monotonic deadline. */
    orb_time_poll(600000000, 0);
    CHECK(snapshot(600000000).alarm_ringing && s_cues == 2);
    orb_time_dismiss(false, true);
    CHECK(!snapshot(600000000).alarm_ringing);
}

static void lifecycle(void)
{
    orb_time_init();
    s_cues = 0;
    orb_apps_init(0);
    const orb_app_manifest_t *app = orb_apps_find("time");
    CHECK(app && !strcmp(app->name, "Time") && app->colour == 0x00D2E0 && app->needs == 0);
    CHECK(orb_apps_count() == 2);
    CHECK(orb_apps_enter("time") == ESP_OK);
    CHECK(orb_time_timer_set(2));
    CHECK(orb_time_timer_start(0));
    CHECK(orb_time_stopwatch_start(0));
    orb_apps_poll(1000000, false, false, 0, 0, 1, false);
    CHECK(orb_apps_paused() && snapshot(1000000).timer_remaining_us == 1000000);
    orb_apps_exit();
    CHECK(!orb_apps_active());
    orb_apps_poll(2000000, false, false, 0, 0, 1, false);
    CHECK(snapshot(2000000).timer_state == ORB_TIME_FINISHED && s_cues == 1);
    CHECK(snapshot(2000000).stopwatch_elapsed_us == 2000000);
    CHECK(orb_apps_enter("time") == ESP_OK);
    CHECK(snapshot(2000000).timer_state == ORB_TIME_FINISHED);
    orb_apps_exit();
    orb_apps_init(0);
    CHECK(snapshot(2000000).timer_state == ORB_TIME_FINISHED); /* Registry reset is not service reset. */
}

static void external_clock(void)
{
    orb_time_init();
    s_cues = 0;
    /* An offline boot must not invent a date or enable Alarm. Relative clocks
     * continue immediately while the first hub/SNTP sample is outstanding. */
    orb_time_poll(0, 0);
    CHECK(!snapshot(0).clock_valid);
    CHECK(!orb_time_alarm_enable(true, 0));
    CHECK(orb_time_timer_set(2));
    CHECK(orb_time_timer_start(0));
    CHECK(orb_time_stopwatch_start(0));
    orb_time_poll(1000000, 0);
    orb_time_snapshot_t s = snapshot(1000000);
    CHECK(s.timer_state == ORB_TIME_RUNNING && s.timer_remaining_us == 1000000);
    CHECK(s.stopwatch_elapsed_us == 1000000 && !s.clock_valid);
    orb_time_poll(2000000, 0);
    CHECK(snapshot(2000000).timer_state == ORB_TIME_FINISHED && s_cues == 1);

    /* A first authoritative sample unlocks Alarm without resetting either
     * monotonic clock. Disconnecting after sync keeps using system time. */
    time_t external = 1735714740; /* 2025-01-01 06:59 UTC. */
    orb_time_poll(3000000, external);
    s = snapshot(3000000);
    CHECK(s.clock_valid && s.clock_now == external);
    CHECK(s.timer_state == ORB_TIME_FINISHED && s.stopwatch_elapsed_us == 3000000);
    CHECK(orb_time_alarm_set(7, 0));
    CHECK(orb_time_alarm_enable(true, s.clock_now));
    CHECK(snapshot(3000000).alarm_next == external + 60);
    orb_time_poll(63000000, external + 60);
    CHECK(snapshot(63000000).alarm_ringing && s_cues == 2);
    orb_time_dismiss(false, true);
    orb_time_poll(64000000, external + 61);
    CHECK(!snapshot(64000000).alarm_ringing && s_cues == 2);
}

static void reminders(void)
{
    orb_time_init();
    s_cues = 0;
    time_t base = 1761600000; /* Valid wall clock */
    /* The proposal refuses a reminder until the clock is trustworthy. */
    CHECK(!orb_time_reminder_set("No clock", base + 100, NULL));
    orb_time_poll(0, base);
    orb_time_snapshot_t s = snapshot(0);
    CHECK(!s.reminder_due);

    CHECK(!orb_time_reminder_set(NULL, base + 100, NULL));
    CHECK(!orb_time_reminder_set("", base + 100, NULL));
    CHECK(!orb_time_reminder_set("Test", 0, NULL));

    uint32_t id1 = 0;
    CHECK(orb_time_reminder_set("Take the second Orb to the office", base + 300, &id1));
    CHECK(id1 > 0);
    s = snapshot(0);
    CHECK(!s.reminder_due);

    /* Advance to 100s before due */
    orb_time_poll(200000000, base + 200);
    s = snapshot(200000000);
    CHECK(!s.reminder_due && s_cues == 0);

    /* Due boundary */
    orb_time_poll(300000000, base + 300);
    s = snapshot(300000000);
    CHECK(s.reminder_due && s_cues == 1);
    CHECK(!strcmp(s.active_reminder.title, "Take the second Orb to the office"));
    CHECK(s.active_reminder.id == id1);

    /* Snooze +3600 seconds */
    CHECK(orb_time_reminder_snooze(id1, 3600));
    s = snapshot(300000000);
    CHECK(!s.reminder_due);

    /* Advance before snoozed due */
    orb_time_poll(400000000, base + 300 + 1000);
    s = snapshot(400000000);
    CHECK(!s.reminder_due && s_cues == 1);

    /* Reach snoozed due */
    orb_time_poll(500000000, base + 300 + 3600);
    s = snapshot(500000000);
    CHECK(s.reminder_due && s_cues == 2);

    /* Mark done */
    CHECK(orb_time_reminder_done(id1));
    s = snapshot(500000000);
    CHECK(!s.reminder_due);

    /* A clipped multibyte title stops on a character boundary. */
    orb_time_reminder_clear_all();
    char long_title[128];
    for (unsigned i = 0; i < 40; ++i) { long_title[i * 2] = (char)0xC3; long_title[i * 2 + 1] = (char)0xA9; }
    long_title[80] = '\0';
    CHECK(orb_time_reminder_set(long_title, base, NULL));
    s = snapshot(500000000);
    CHECK(strlen(s.active_reminder.title) <= 63 && strlen(s.active_reminder.title) % 2 == 0);
    orb_time_reminder_clear_all();

    /* Capacity check */
    orb_time_reminder_clear_all();
    for (unsigned i = 0; i < ORB_TIME_REMINDERS_MAX; ++i) {
        char title[32]; snprintf(title, sizeof(title), "Reminder %u", i);
        CHECK(orb_time_reminder_set(title, base + 1000 + i, NULL));
    }
    CHECK(!orb_time_reminder_set("Overflow", base + 2000, NULL));
    /* Mark one done to allow eviction */
    CHECK(orb_time_reminder_done(1));
    CHECK(orb_time_reminder_set("Evicted OK", base + 2000, NULL));
}

static time_t local_epoch(int year, int month, int day, int hour, int minute, int dst)
{
    struct tm value = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
        .tm_hour = hour, .tm_min = minute, .tm_isdst = dst};
    return mktime(&value);
}

static void repeat_alarm(void)
{
    CHECK(setenv("TZ", "UTC0", 1) == 0); tzset();
    orb_time_init();
    time_t friday = local_epoch(2026, 10, 30, 23, 59, -1);
    orb_time_poll(0, friday);
    CHECK(orb_time_alarm_set(0, 1));
    CHECK(!orb_time_alarm_repeat_set((orb_time_repeat_t)99, 0));
    CHECK(!orb_time_alarm_repeat_set(ORB_TIME_REPEAT_CUSTOM, 0));
    CHECK(!orb_time_alarm_repeat_set(ORB_TIME_REPEAT_CUSTOM, 0x80));
    CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_WEEKDAYS, 0));
    CHECK(orb_time_alarm_enable(true, friday));
    CHECK(snapshot(0).alarm_next == local_epoch(2026, 11, 2, 0, 1, -1));
    CHECK(snapshot(0).alarm_days == ORB_TIME_DAYS_WEEKDAYS);
    CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_WEEKENDS, 0));
    CHECK(snapshot(0).alarm_next == local_epoch(2026, 10, 31, 0, 1, -1));
    CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_CUSTOM, (1u << 2) | (1u << 4)));
    CHECK(snapshot(0).alarm_next == local_epoch(2026, 11, 3, 0, 1, -1));
    CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_DAILY, 0));
    time_t saturday = snapshot(0).alarm_next;
    s_cues = 0;
    orb_time_poll(120000000, saturday);
    CHECK(snapshot(120000000).alarm_ringing && snapshot(120000000).alarm_enabled && s_cues == 1);
    CHECK(snapshot(120000000).alarm_next == saturday + 86400);
    orb_time_dismiss(false, true);
    /* A correction back over today's alarm cannot ring that civil occurrence twice. */
    orb_time_poll(121000000, saturday - 30);
    CHECK(!snapshot(121000000).alarm_ringing && snapshot(121000000).alarm_next == saturday + 86400);
    orb_time_poll(122000000, saturday + 10);
    CHECK(!snapshot(122000000).alarm_ringing && s_cues == 1);
    orb_time_poll(123000000, saturday + 86400);
    CHECK(snapshot(123000000).alarm_ringing && s_cues == 2);
    CHECK(!orb_time_alarm_snooze_set(59));
    CHECK(!orb_time_alarm_snooze_set(3601));
    CHECK(orb_time_alarm_snooze_set(300));
    CHECK(snapshot(123000000).alarm_snooze_seconds == 300);
    CHECK(orb_time_alarm_snooze(300));
    orb_time_poll(423000000, saturday + 86700);
    CHECK(snapshot(423000000).alarm_ringing && snapshot(423000000).alarm_enabled && s_cues == 3);
    orb_time_dismiss(false, true);
    CHECK(snapshot(423000000).alarm_next == saturday + 2 * 86400);

    /* Disabling never accidentally acknowledges a visible ringing receipt. */
    orb_time_poll(INT64_C(86400000000), saturday + 2 * 86400);
    CHECK(snapshot(INT64_C(86400000000)).alarm_ringing);
    CHECK(orb_time_alarm_enable(false, saturday + 2 * 86400));
    CHECK(snapshot(INT64_C(86400000000)).alarm_ringing);
    orb_time_dismiss(false, true);

    /* Applying a new system timezone preserves the selected civil hour. */
    orb_time_init();
    time_t morning = local_epoch(2026, 10, 28, 6, 0, -1);
    orb_time_poll(0, morning);
    CHECK(orb_time_alarm_set(7, 0) && orb_time_alarm_enable(true, morning));
    CHECK(setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1) == 0); tzset();
    orb_time_poll(1000000, morning + 60);
    CHECK(!snapshot(1000000).alarm_ringing);
    CHECK(snapshot(1000000).alarm_next == local_epoch(2026, 10, 28, 7, 0, -1));
    CHECK(setenv("TZ", "UTC0", 1) == 0); tzset();
}

static void large_backward_clock_corrections(void)
{
    CHECK(setenv("TZ", "UTC0", 1) == 0); tzset();
    orb_time_init();
    time_t before = local_epoch(2026, 10, 28, 6, 59, -1);
    orb_time_poll(0, before);
    CHECK(orb_time_alarm_set(7, 0));
    CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_DAILY, 0));
    CHECK(orb_time_alarm_enable(true, before));
    orb_time_poll(60000000, before + 60);
    CHECK(snapshot(60000000).alarm_ringing);
    orb_time_dismiss(false, true);

    /* A corrected calendar must not wait until the old date catches up. */
    time_t week_earlier = before - 7 * 86400;
    orb_time_poll(61000000, week_earlier);
    CHECK(snapshot(61000000).alarm_enabled && !snapshot(61000000).alarm_ringing);
    CHECK(snapshot(61000000).alarm_next == week_earlier + 60);
    orb_time_poll(62000000, week_earlier + 60);
    CHECK(snapshot(62000000).alarm_ringing);
    orb_time_dismiss(false, true);

    time_t year_earlier = local_epoch(2025, 1, 1, 6, 59, -1);
    orb_time_poll(63000000, year_earlier);
    CHECK(snapshot(63000000).alarm_enabled && !snapshot(63000000).alarm_ringing);
    CHECK(snapshot(63000000).alarm_next == year_earlier + 60);
    orb_time_poll(64000000, year_earlier + 60);
    CHECK(snapshot(64000000).alarm_ringing);
    orb_time_dismiss(false, true);
}

static void timezone_relocation(void)
{
    CHECK(setenv("TZ", "UTC0", 1) == 0); tzset();
    orb_time_init(); s_cues = 0;
    time_t now = local_epoch(2026, 10, 28, 0, 30, -1);
    orb_time_poll(0, now);
    CHECK(orb_time_alarm_set(7, 0));
    CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_DAILY, 0));
    CHECK(orb_time_alarm_enable(true, now));
    uint32_t generation = snapshot(0).generation;
    /* A move east past today's selected hour schedules tomorrow. Merely
     * changing timezone is not an overdue alarm occurrence. */
    CHECK(setenv("TZ", "EAST-14", 1) == 0); tzset();
    orb_time_poll(1000000, now + 1);
    CHECK(!snapshot(1000000).alarm_ringing && s_cues == 0);
    CHECK(snapshot(1000000).alarm_next == local_epoch(2026, 10, 29, 7, 0, -1));
    CHECK(snapshot(1000000).generation > generation);

    /* Crossing the date line west must include the new local date, rather
     * than retaining tomorrow's date from the previous timezone. */
    CHECK(setenv("TZ", "UTC0", 1) == 0); tzset();
    orb_time_init();
    now = local_epoch(2026, 10, 28, 0, 30, -1);
    orb_time_poll(0, now);
    CHECK(orb_time_alarm_set(23, 0) && orb_time_alarm_enable(true, now));
    CHECK(setenv("TZ", "WEST12", 1) == 0); tzset();
    orb_time_poll(1000000, now + 1);
    CHECK(!snapshot(1000000).alarm_ringing);
    CHECK(snapshot(1000000).alarm_next == local_epoch(2026, 10, 27, 23, 0, -1));

    /* Equal current abbreviations/offsets can still have different rules
     * at tomorrow's DST boundary. Compare the timezone rules as well. */
    CHECK(setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1) == 0); tzset();
    orb_time_init();
    now = local_epoch(2026, 3, 7, 23, 30, -1);
    orb_time_poll(0, now);
    CHECK(orb_time_alarm_set(3, 30) && orb_time_alarm_enable(true, now));
    CHECK(snapshot(0).alarm_next == local_epoch(2026, 3, 8, 3, 30, -1));
    CHECK(setenv("TZ", "EST5", 1) == 0); tzset();
    orb_time_poll(1000000, now + 1);
    CHECK(!snapshot(1000000).alarm_ringing);
    CHECK(snapshot(1000000).alarm_next == local_epoch(2026, 3, 8, 3, 30, -1));
    CHECK(setenv("TZ", "UTC0", 1) == 0); tzset();
}

static void current_due_reminder(void)
{
    orb_time_init();
    time_t now = local_epoch(2026, 10, 28, 12, 0, -1);
    orb_time_poll(0, now);
    uint32_t later, earlier;
    CHECK(orb_time_reminder_set("Later, inserted first", now + 120, &later));
    CHECK(orb_time_reminder_set("Earlier, inserted second", now + 60, &earlier));
    orb_time_poll(180000000, now + 180);
    CHECK(snapshot(180000000).active_reminder.id == earlier);
    /* The current-due sentinel must resolve to the receipt actually shown. */
    CHECK(orb_time_reminder_done(0));
    CHECK(snapshot(180000000).active_reminder.id == later);
    orb_time_reminder_clear_all();
    CHECK(orb_time_reminder_set("Later", now + 120, &later));
    CHECK(orb_time_reminder_set("Earlier", now + 60, &earlier));
    CHECK(snapshot(180000000).active_reminder.id == earlier);
    CHECK(orb_time_reminder_snooze(0, 3600));
    CHECK(snapshot(180000000).active_reminder.id == later);
    orb_reminder_t item;
    CHECK(orb_time_reminder_get(1, &item) && item.id == earlier && item.due_time == now + 3780);
}

static void daylight_saving(void)
{
    CHECK(setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1) == 0); tzset();
    orb_time_init();
    time_t before = local_epoch(2026, 3, 7, 23, 59, -1);
    orb_time_poll(0, before);
    CHECK(orb_time_alarm_set(2, 30));
    CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_DAILY, 0));
    CHECK(orb_time_alarm_enable(true, before));
    time_t spring = local_epoch(2026, 3, 8, 3, 30, 1);
    CHECK(snapshot(0).alarm_next == spring);
    /* The UTC offset changes before the nonexistent 02:30 normalizes to 03:30. */
    orb_time_poll(1000000, spring - 1800);
    CHECK(!snapshot(1000000).alarm_ringing && snapshot(1000000).alarm_next == spring);
    orb_time_poll(2000000, spring);
    CHECK(snapshot(2000000).alarm_ringing);
    CHECK(snapshot(2000000).alarm_next == local_epoch(2026, 3, 9, 2, 30, 1));

    orb_time_init();
    before = local_epoch(2026, 10, 31, 23, 59, -1);
    orb_time_poll(0, before);
    CHECK(orb_time_alarm_set(1, 30));
    CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_DAILY, 0));
    CHECK(orb_time_alarm_enable(true, before));
    time_t first = local_epoch(2026, 11, 1, 1, 30, 1);
    time_t second = local_epoch(2026, 11, 1, 1, 30, 0);
    CHECK(second == first + 3600);
    CHECK(snapshot(0).alarm_next == first);
    s_cues = 0;
    orb_time_poll(1000000, first);
    CHECK(snapshot(1000000).alarm_ringing && s_cues == 1);
    orb_time_dismiss(false, true);
    orb_time_poll(2000000, second);
    CHECK(!snapshot(2000000).alarm_ringing && s_cues == 1);
    CHECK(snapshot(2000000).alarm_next == local_epoch(2026, 11, 2, 1, 30, 0));
    /* Arming during the second 01:00 does not choose a duplicate 01:30. */
    orb_time_init();
    orb_time_poll(0, second - 1800);
    CHECK(orb_time_alarm_set(1, 30));
    CHECK(orb_time_alarm_enable(true, second - 1800));
    CHECK(snapshot(0).alarm_next == local_epoch(2026, 11, 2, 1, 30, 0));
    CHECK(setenv("TZ", "UTC0", 1) == 0); tzset();
}

static void clock_format(void)
{
    orb_time_init();
    char text[80];
    orb_time_set_clock_default(false);
    CHECK(!snapshot(0).hour24);
    orb_time_set_clock_default(true);
    CHECK(snapshot(0).hour24);
    orb_time_set_clock_format(true); /* Even selecting the existing value owns it. */
    orb_time_set_clock_default(false);
    CHECK(snapshot(0).hour24);
    orb_time_format_clock(text, sizeof(text), 0, 5); CHECK(!strcmp(text, "00:05"));
    orb_time_format_clock(text, sizeof(text), 23, 59); CHECK(!strcmp(text, "23:59"));
    orb_time_set_clock_format(false);
    orb_time_format_clock(text, sizeof(text), 0, 5); CHECK(!strcmp(text, "12:05 AM"));
    orb_time_format_clock(text, sizeof(text), 12, 0); CHECK(!strcmp(text, "12:00 PM"));
    orb_time_format_clock(text, sizeof(text), 23, 59); CHECK(!strcmp(text, "11:59 PM"));
    orb_time_format_clock(NULL, 0, 0, 0);
    orb_time_format_repeat(text, sizeof(text), ORB_TIME_REPEAT_WEEKDAYS, 0);
    CHECK(!strcmp(text, "Weekdays"));
    orb_time_format_repeat(text, sizeof(text), ORB_TIME_REPEAT_WEEKENDS, 0);
    CHECK(!strcmp(text, "Weekends"));
    orb_time_format_repeat(text, sizeof(text), ORB_TIME_REPEAT_DAILY, 0);
    CHECK(!strcmp(text, "Every day"));
    orb_time_format_repeat(text, sizeof(text), ORB_TIME_REPEAT_ONCE, 0);
    CHECK(!strcmp(text, "One time"));
    orb_time_format_repeat(text, sizeof(text), ORB_TIME_REPEAT_CUSTOM, 0x45);
    CHECK(!strcmp(text, "Tue, Sat, Sun"));
}

static void reminder_real_data(void)
{
    orb_time_init();
    time_t now = local_epoch(2026, 10, 28, 12, 0, -1);
    orb_time_poll(0, now);
    uint32_t late, early;
    CHECK(!orb_time_reminder_set(" \n\t", now, NULL));
    CHECK(orb_time_reminder_set("Late", now + 300, &late));
    CHECK(orb_time_reminder_set("Early", now + 60, &early));
    orb_reminder_t item;
    CHECK(orb_time_reminder_get(0, &item) && item.id == early);
    CHECK(orb_time_reminder_get(1, &item) && item.id == late);
    CHECK(!orb_time_reminder_get(2, &item));
    CHECK(!orb_time_reminder_get(0, NULL));
    orb_time_poll(1000000, now + 7200);
    CHECK(snapshot(1000000).active_reminder.id == early);
    CHECK(!orb_time_reminder_snooze(early, 0));
    CHECK(!orb_time_reminder_snooze(early, 86401));
    CHECK(orb_time_reminder_snooze(early, 3600));
    CHECK(snapshot(1000000).reminder_due && snapshot(1000000).active_reminder.id == late);
    CHECK(orb_time_reminder_get(1, &item) && item.id == early && item.due_time == now + 10800);
    CHECK(orb_time_reminder_done(late));
    CHECK(!orb_time_reminder_done(late));
    CHECK(!orb_time_reminder_snooze(late, 60));
    CHECK(!snapshot(1000000).reminder_due && snapshot(1000000).reminder_count == 1);
    CHECK(orb_time_reminder_set("bad\xF0\x80\x80\x80\n\xC0\xAF", now + 18000, NULL));
    CHECK(orb_time_reminder_get(1, &item));
    CHECK(!strcmp(item.title, "bad???? ??"));
    CHECK(orb_time_reminder_set("Same time, later insertion", now + 18000, NULL));
    CHECK(orb_time_reminder_get(1, &item) && !strcmp(item.title, "bad???? ??"));
    CHECK(orb_time_reminder_get(2, &item) && !strcmp(item.title, "Same time, later insertion"));
    /* Done/snooze before main drains a due transition cancels its obsolete cue. */
    orb_time_reminder_clear_all(); s_cues = 0;
    CHECK(orb_time_reminder_set("Already due", now, &early));
    CHECK(orb_time_reminder_done(early));
    orb_time_poll(2000000, now + 7201);
    CHECK(s_cues == 0);
}

static void persistence(void)
{
    s_storage_ready = true; s_record_size = 0; s_writes = 0; s_storage_fail = false;
    orb_time_init();
    time_t before = local_epoch(2026, 10, 28, 6, 59, -1);
    orb_time_poll(0, before);
    CHECK(orb_time_alarm_set(7, 0));
    CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_CUSTOM, 0x3e));
    CHECK(orb_time_alarm_snooze_set(300));
    CHECK(orb_time_alarm_enable(true, before));
    orb_time_set_clock_format(false);
    CHECK(orb_time_timer_set(1500));
    CHECK(orb_time_timer_start(0));
    uint32_t id;
    CHECK(orb_time_reminder_set("Persisted café reminder", before + 600, &id));
    CHECK(snapshot(0).saving && !snapshot(0).save_error);
    CHECK(s_writes == 0); /* All commands avoid flash and LVGL completely. */
    orb_time_persist_service(); CHECK(s_writes == 0);
    (void)snapshot(499999);
    orb_time_persist_service(); CHECK(s_writes == 0);
    (void)snapshot(500000);
    orb_time_persist_service();
    CHECK(s_writes == 1 && s_record_size == 684);
    CHECK(!snapshot(0).saving && !snapshot(0).save_error);
    (void)snapshot(orb_time_now_us() + 500000);
    orb_time_persist_service(); CHECK(s_writes == 1);
    orb_time_init();
    orb_time_snapshot_t restored = snapshot(0);
    CHECK(restored.alarm_repeat == ORB_TIME_REPEAT_CUSTOM && restored.alarm_days == 0x3e);
    CHECK(restored.alarm_enabled && restored.alarm_hour == 7 && restored.alarm_minute == 0);
    CHECK(restored.alarm_snooze_seconds == 300 && !restored.hour24);
    orb_time_set_clock_default(true);
    CHECK(!snapshot(0).hour24); /* The restored local preference wins on reconnect. */
    CHECK(restored.timer_last_seconds == 1500 && restored.timer_duration_us == INT64_C(1500000000));
    CHECK(restored.timer_state == ORB_TIME_IDLE && restored.reminder_count == 1);
    orb_reminder_t reminder;
    CHECK(orb_time_reminder_get(0, &reminder) && reminder.id == id && !strcmp(reminder.title, "Persisted café reminder"));
    /* The first valid post-boot clock rings an overdue alarm once. */
    s_cues = 0;
    orb_time_poll(1000000, before + 120);
    CHECK(snapshot(1000000).alarm_ringing && s_cues == 1);
    CHECK(orb_time_alarm_snooze(300));
    (void)snapshot(orb_time_now_us() + 500000);
    orb_time_persist_service();
    orb_time_init();
    orb_time_poll(0, before + 200);
    CHECK(!snapshot(0).alarm_ringing);
    orb_time_poll(220000000, before + 420);
    CHECK(snapshot(220000000).alarm_ringing);
    orb_time_dismiss(false, true);
    orb_time_poll(221000000, before + 600);
    CHECK(snapshot(221000000).reminder_due);
    CHECK(orb_time_reminder_snooze(id, 3600));
    (void)snapshot(orb_time_now_us() + 500000);
    orb_time_persist_service();
    orb_time_init();
    CHECK(orb_time_reminder_get(0, &reminder) && reminder.due_time == before + 4200 && !reminder.due);
    CHECK(orb_time_reminder_done(id));
    s_storage_fail = true;
    (void)snapshot(orb_time_now_us() + 500000);
    orb_time_persist_service();
    CHECK(snapshot(0).save_error && !snapshot(0).saving);
    unsigned writes = s_writes;
    (void)snapshot(orb_time_now_us() + 500000);
    orb_time_persist_service(); CHECK(s_writes == writes); /* no retry loop */
    s_storage_fail = false;
    orb_time_persist_retry();
    CHECK(snapshot(0).saving);
    (void)snapshot(orb_time_now_us() + 500000);
    orb_time_persist_service();
    CHECK(!snapshot(0).saving && !snapshot(0).save_error);
    orb_time_init();
    CHECK(snapshot(0).reminder_count == 0);
    /* Corrupt/truncated/future records fail closed without erasing storage. */
    s_record[42] ^= 0x80;
    orb_time_init();
    CHECK(snapshot(0).save_error && !snapshot(0).alarm_enabled);
    CHECK(snapshot(0).alarm_repeat == ORB_TIME_REPEAT_ONCE && snapshot(0).timer_last_seconds == 0);
    writes = s_writes;
    (void)snapshot(orb_time_now_us() + 500000);
    orb_time_persist_service(); CHECK(s_writes == writes);
    s_record_size = 8;
    orb_time_init(); CHECK(snapshot(0).save_error && !snapshot(0).alarm_enabled);
    s_record_size = 0; s_storage_ready = false;
    orb_time_init();
}

static time_t restore_snoozed_alarm(void)
{
    s_storage_ready = true; s_record_size = 0; s_storage_fail = false;
    orb_time_init();
    time_t before = local_epoch(2026, 10, 28, 6, 59, -1);
    orb_time_poll(0, before);
    CHECK(orb_time_alarm_set(7, 0) && orb_time_alarm_enable(true, before));
    orb_time_poll(60000000, before + 60);
    CHECK(orb_time_alarm_snooze(300));
    (void)snapshot(60500000);
    orb_time_persist_service();
    orb_time_init();
    return before;
}

static void restored_snooze_commands(void)
{
    time_t before = restore_snoozed_alarm();
    /* Offline reboot does not stop an explicit Disable from cancelling the
     * durable snooze. The first clock sync must not resurrect that alarm. */
    CHECK(orb_time_alarm_enable(false, 0));
    orb_time_poll(0, before + 120);
    CHECK(!snapshot(0).alarm_ringing && !snapshot(0).alarm_enabled);

    before = restore_snoozed_alarm();
    CHECK(orb_time_alarm_set(8, 0));
    orb_time_poll(0, before + 120);
    CHECK(!snapshot(0).alarm_ringing && snapshot(0).alarm_enabled);
    CHECK(snapshot(0).alarm_next == local_epoch(2026, 10, 28, 8, 0, -1));

    before = restore_snoozed_alarm();
    CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_DAILY, 0));
    orb_time_poll(0, before + 120);
    CHECK(!snapshot(0).alarm_ringing && snapshot(0).alarm_enabled);
    CHECK(snapshot(0).alarm_next == local_epoch(2026, 10, 29, 7, 0, -1));
    s_record_size = 0; s_storage_ready = false;
    orb_time_init();
}

static void snooze_clock_range(void)
{
    orb_time_init();
    time_t now = (time_t)ORB_TIME_CLOCK_MAX - 30;
    orb_time_poll(0, now);
    uint32_t id;
    CHECK(orb_time_reminder_set("Due at end of supported calendar", now, &id));
    CHECK(!orb_time_reminder_snooze(id, 60));
    CHECK(snapshot(0).reminder_due && snapshot(0).active_reminder.id == id);
}

static void calendar_boundaries_and_overlap(void)
{
    CHECK(setenv("TZ", "UTC0", 1) == 0); tzset();
    static const struct { int year, month, day, next_year, next_month, next_day; } dates[] = {
        {2026, 1, 31, 2026, 2, 1}, {2028, 2, 28, 2028, 2, 29},
        {2028, 2, 29, 2028, 3, 1}, {2026, 12, 31, 2027, 1, 1},
    };
    for (unsigned i = 0; i < sizeof(dates) / sizeof(dates[0]); ++i) {
        orb_time_init();
        time_t now = local_epoch(dates[i].year, dates[i].month, dates[i].day, 23, 59, -1);
        time_t midnight = local_epoch(dates[i].next_year, dates[i].next_month, dates[i].next_day, 0, 0, -1);
        orb_time_poll(0, now);
        CHECK(orb_time_alarm_set(0, 0));
        CHECK(orb_time_alarm_repeat_set(ORB_TIME_REPEAT_DAILY, 0));
        CHECK(orb_time_alarm_enable(true, now));
        CHECK(snapshot(0).alarm_next == midnight);
        CHECK(orb_time_timer_set(60) && orb_time_timer_start(0));
        uint32_t id;
        CHECK(orb_time_reminder_set("Midnight reminder", midnight, &id));
        s_cues = 0;
        orb_time_poll(60000000, midnight);
        orb_time_snapshot_t state = snapshot(60000000);
        CHECK(state.timer_state == ORB_TIME_FINISHED && state.alarm_ringing && state.reminder_due);
        CHECK(state.active_reminder.id == id && s_cues == 3);
        orb_time_dismiss(true, false);
        state = snapshot(60000000);
        CHECK(state.timer_state == ORB_TIME_IDLE && state.alarm_ringing && state.reminder_due);
        CHECK(orb_time_alarm_snooze(60));
        CHECK(snapshot(60000000).reminder_due);
        CHECK(orb_time_reminder_done(id));
        orb_time_poll(120000000, midnight + 60);
        CHECK(snapshot(120000000).alarm_ringing && !snapshot(120000000).reminder_due && s_cues == 4);
    }

    CHECK(setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1) == 0); tzset();
    orb_time_init();
    time_t before = local_epoch(2026, 3, 8, 1, 59, 0);
    time_t boundary = local_epoch(2026, 3, 8, 3, 0, 1);
    orb_time_poll(0, before);
    CHECK(orb_time_alarm_set(3, 0) && orb_time_alarm_enable(true, before));
    s_cues = 0;
    orb_time_poll(60000000, boundary);
    CHECK(snapshot(60000000).alarm_ringing && s_cues == 1);
    CHECK(setenv("TZ", "UTC0", 1) == 0); tzset();
}

static void *reader(void *context)
{
    (void)context;
    for (unsigned i = 0; i < 10000; ++i) {
        orb_time_snapshot_t s = snapshot((int64_t)i * 1000);
        assert(s.timer_remaining_us >= 0 && s.timer_remaining_us <= s.timer_duration_us);
        assert(s.lap_count <= ORB_TIME_LAPS_MAX);
        assert(s.stopwatch_elapsed_us >= 0 && s.stopwatch_elapsed_us <= ORB_TIME_STOPWATCH_MAX_US);
    }
    return NULL;
}

static void concurrent(void)
{
    orb_time_init();
    CHECK(orb_time_stopwatch_start(0));
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, reader, NULL) == 0);
    for (unsigned i = 0; i < 1000; ++i) {
        orb_time_timer_reset();
        (void)orb_time_timer_set(1 + i % ORB_TIME_TIMER_MAX_SECONDS);
        (void)orb_time_timer_start((int64_t)i * 10000);
        orb_time_timer_pause((int64_t)i * 10000 + 1000);
        (void)orb_time_stopwatch_lap((int64_t)i * 10000);
        orb_time_poll((int64_t)i * 10000, 0);
    }
    CHECK(pthread_join(thread, NULL) == 0);
}

static void sleep_deadlines(void)
{
    orb_time_init();
    CHECK(orb_time_next_deadline_us(123456, 0) == INT64_MAX);
    CHECK(orb_time_timer_set(1) && orb_time_timer_start(123456));
    CHECK(orb_time_next_deadline_us(223456, 0) == 1123456);
    orb_time_timer_pause(223456);
    CHECK(orb_time_next_deadline_us(223456, 0) == INT64_MAX);
    CHECK(orb_time_timer_start(400000));
    CHECK(orb_time_next_deadline_us(400000, 0) == 1300000);
    orb_time_poll(1300000, 0);
    CHECK(snapshot(1300000).timer_state == ORB_TIME_FINISHED);
    CHECK(orb_time_next_deadline_us(1300000, 0) == INT64_MAX);
    orb_time_init();
    CHECK(orb_time_timer_set(1) && orb_time_timer_start(INT64_MAX - 500000));
    CHECK(orb_time_next_deadline_us(INT64_MAX - 500000, 0) == INT64_MAX);

    orb_time_init();
    time_t wall = local_epoch(2026, 10, 31, 7, 0, 0);
    orb_time_poll(987654, wall);
    CHECK(orb_time_alarm_set(7, 1) && orb_time_alarm_enable(true, wall));
    /* Wall and uptime fractions deliberately differ. No 100ms/1s rounding. */
    int64_t wall_us = (int64_t)wall * 1000000 + 234567;
    CHECK(orb_time_next_deadline_us(987654, wall_us) == 987654 + 60000000 - 234567);
    uint32_t id;
    CHECK(orb_time_reminder_set("Deadline", wall + 1, &id));
    CHECK(orb_time_next_deadline_us(987654, wall_us) == 987654 + 1000000 - 234567);
    orb_time_poll(1753087, wall + 1);
    CHECK(snapshot(1753087).reminder_due);
    CHECK(orb_time_next_deadline_us(1753087, (int64_t)(wall + 1) * 1000000) == 1753087 + 59000000);
    orb_time_poll(60753087, wall + 60);
    CHECK(snapshot(60753087).alarm_ringing);
    CHECK(orb_time_alarm_snooze(60));
    CHECK(orb_time_next_deadline_us(60753087, 0) == 120753087);
}

int main(void)
{
    sleep_deadlines();
    duration_format();
    timer();
    sound_dismiss_race();
    stopwatch();
    alarm();
    alarm_snooze();
    reminders();
    external_clock();
    repeat_alarm();
    large_backward_clock_corrections();
    timezone_relocation();
    current_due_reminder();
    daylight_saving();
    clock_format();
    reminder_real_data();
    persistence();
    restored_snooze_commands();
    snooze_clock_range();
    calendar_boundaries_and_overlap();
    lifecycle();
    concurrent();
    orb_time_snapshot(0, NULL);
    printf("orb_time: %u checks passed; 10000 concurrent snapshot invariant checks passed\n", s_checks);
    return 0;
}
