#include "orb_weather_host.h"
#include "orb_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

_Static_assert(ORB_SHELL_WEATHER == ORB_WEATHER_SCREEN_ID, "Weather descriptor is keyed by its shell place");

/* Hour labels follow the owner's clock format; the presentation receives
 * finished text so it never depends on the process time zone itself. */
static void hour_label(char *out, size_t capacity, uint32_t when, bool hour24)
{
    time_t value = (time_t)when;
    struct tm local;
    if (!localtime_r(&value, &local)) { out[0] = '\0'; return; }
    if (hour24) { snprintf(out, capacity, "%02d", local.tm_hour); return; }
    char period[8] = "";
    (void)strftime(period, sizeof(period), "%p", &local);
    snprintf(out, capacity, "%d%s%s", local.tm_hour % 12 ? local.tm_hour % 12 : 12, period[0] ? " " : "", period);
}

/* "9:41 AM" today; a reading from another local day names its weekday, so a
 * stale value is never mistaken for this morning's (hig.md §8). */
static void updated_label(char *out, size_t capacity, uint32_t when, const orb_time_snapshot_t *clock)
{
    out[0] = '\0';
    time_t value = (time_t)when;
    struct tm local;
    if (!when || !localtime_r(&value, &local)) return;
    char time_text[16];
    orb_time_format_clock(time_text, sizeof(time_text), (unsigned)local.tm_hour, (unsigned)local.tm_min);
    struct tm today;
    time_t now = clock->clock_now;
    if (clock->clock_valid && localtime_r(&now, &today) &&
        (today.tm_yday != local.tm_yday || today.tm_year != local.tm_year)) {
        char day[8] = "";
        (void)strftime(day, sizeof(day), "%a", &local);
        snprintf(out, capacity, "%s %s", day, time_text);
    } else snprintf(out, capacity, "%s", time_text);
}

void orb_weather_host_project(const orb_model_snapshot_t *model, orb_weather_screen_model_t *out)
{
    memset(out, 0, sizeof(*out));
    const orb_weather_snapshot_t *weather = &model->weather;
    orb_time_snapshot_t clock;
    orb_time_snapshot(orb_time_now_us(), &clock);
    out->online = model->online;
    out->synced = model->synced;
    out->supported = model->weather_supported;
    out->configured = weather->configured;
    out->available = weather->available;
    out->stale = weather->stale;
    snprintf(out->place, sizeof(out->place), "%s", weather->place);
    if (!weather->available) return;
    out->temperature = weather->temperature;
    out->high = weather->high;
    out->low = weather->low;
    out->condition = weather->condition;
    out->is_day = weather->is_day;
    updated_label(out->updated, sizeof(out->updated), weather->updated_at, &clock);
    out->hourly_count = (uint32_t)(weather->hourly_count < ORB_MODEL_MAX_WEATHER_HOURS ?
        weather->hourly_count : ORB_MODEL_MAX_WEATHER_HOURS);
    for (uint32_t i = 0; i < out->hourly_count; i++) {
        const orb_weather_hour_t *hour = &weather->hourly[i];
        hour_label(out->hourly[i].time, sizeof(out->hourly[i].time), hour->hour, clock.hour24);
        out->hourly[i].temperature = hour->temperature;
        out->hourly[i].condition = hour->condition;
        out->hourly[i].is_day = hour->is_day;
    }
}

bool orb_settings_host_stage_weather(orb_settings_host_t *host, lv_obj_t *parent, const orb_model_snapshot_t *model)
{
    if (!host || !model) return false;
    orb_weather_screen_model_t projection;
    orb_weather_host_project(model, &projection);
    return orb_settings_host_stage(host, parent, ORB_WEATHER_SCREEN_ID,
        &(orb_screen_model_t){&projection, sizeof(projection)});
}

bool orb_settings_host_update_weather(orb_settings_host_t *host, const orb_model_snapshot_t *model)
{
    if (!model) return false;
    orb_weather_screen_model_t projection;
    orb_weather_host_project(model, &projection);
    return orb_settings_host_update(host, ORB_WEATHER_SCREEN_ID, &(orb_screen_model_t){&projection, sizeof(projection)});
}
