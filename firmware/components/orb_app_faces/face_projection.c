#include "orb_app_faces.h"
#include "orb_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static bool clock_parts(const char *clock, unsigned *hour, unsigned *minute)
{
    unsigned h, m;
    char period[24] = "", extra;
    int fields = sscanf(clock, "%u:%u %23s %c", &h, &m, period, &extra);
    if (fields < 2 || fields > 3 || m > 59) return false;
    if (fields == 3) {
        if (h < 1 || h > 12) return false;
        struct tm sample = {.tm_hour = 1};
        char am[24] = "", pm[24] = "";
        (void)strftime(am, sizeof(am), "%p", &sample);
        sample.tm_hour = 13;
        (void)strftime(pm, sizeof(pm), "%p", &sample);
        if (am[0] && !strcmp(period, am)) h %= 12;
        else if (pm[0] && !strcmp(period, pm)) h = h % 12 + 12;
        else return false;
    } else if (h > 23) return false;
    if (hour) *hour = h;
    if (minute) *minute = m;
    return true;
}

void orb_app_faces_snapshot(orb_faces_snapshot_t *snapshot, const orb_model_snapshot_t *model,
    const orb_home_snapshot_t *home, const char *clock, const char *date, int battery,
    bool charging, bool online)
{
    if (!snapshot) return;
    *snapshot = (orb_faces_snapshot_t){.now = time(NULL),
        .battery_percent = battery, .charging = charging};
    /* Home retains confirmed objects on disconnect. Keep their last summary
     * marked stale; climate/security accessories never dilute its fraction. */
    snapshot->home_stale = !(home && home->online && home->synced && home->available);
    if (home) {
        snapshot->home_on = (unsigned)orb_home_on_count(home);
        for (size_t i = 0; i < home->accessory_count; i++)
            snapshot->home_total += orb_home_summary_accessory(&home->accessories[i]) &&
                (home->accessories[i].state.present & ORB_HOME_ON);
    }
    unsigned hour, minute;
    if (clock_parts(clock, &hour, &minute)) orb_time_format_clock(snapshot->clock, sizeof(snapshot->clock), hour, minute);
    snapshot->clock[strcspn(snapshot->clock, " ")] = 0; /* Like a watch face, 12-hour without AM/PM: sized for 88:88. */
    const char *comma = strchr(date, ',');
    const char *day = strrchr(date, ' ');
    if (comma && day && day[1]) {
        snprintf(snapshot->weekday, sizeof(snapshot->weekday), "%.*s",
                 (int)LV_MIN((size_t)(comma - date), sizeof(snapshot->weekday) - 1),
                 date);
        snprintf(snapshot->day, sizeof(snapshot->day), "%s", day + 1);
    }
    orb_faces_snapshot_model(snapshot, model);
    snapshot->online &= online;
    orb_time_snapshot_t utility;
    orb_time_snapshot(orb_time_now_us(), &utility);
    if (utility.timer_state == ORB_TIME_RUNNING || utility.timer_state == ORB_TIME_PAUSED) {
        snapshot->next_seconds = (uint32_t)((utility.timer_remaining_us + 999999) / 1000000);
        snapshot->next_total_seconds = (uint32_t)((utility.timer_duration_us + 999999) / 1000000);
    } else if (utility.clock_valid && utility.alarm_enabled && utility.alarm_next > utility.clock_now)
        snapshot->next_seconds = (uint32_t)(utility.alarm_next - utility.clock_now);
}

void orb_app_faces_public_projection(orb_faces_snapshot_t *snapshot)
{
    if (!snapshot) return;
    snapshot->home_on = snapshot->home_total = 0;
    if (!snapshot->weather_override) {
        orb_weather_snapshot_t weather = {0};
        if (orb_model_weather_snapshot(&weather)) snapshot->weather = weather;
        else snapshot->weather = (orb_weather_snapshot_t){0};
    }
    snapshot->weather_override = true;
    memset(snapshot->weather.place, 0, sizeof(snapshot->weather.place));
}
