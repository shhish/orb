#include "orb_faces_data.h"
#include "orb_kit.h"
#include "orb_weather_icons.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


bool orb_faces_clock_valid(time_t now)
{
    /* Match the system clock's synchronisation floor. Seconds since boot are
     * not a civil date, even once localtime() can convert them successfully. */
    return now > 1700000000;
}

/* Polish #14-16: one rule on every face. A value the model still holds is
 * shown dimmed beside the shared stale mark (the clock glyph); "—" beside
 * the provider's own glyph appears only when there is no value at all. The
 * model's temperature is valid only while the provider is available. */
orb_faces_weather_t orb_faces_weather_state(const orb_faces_snapshot_t *s)
{
    const orb_weather_snapshot_t *weather = &s->weather;
    if (!weather->configured || !weather->available || !isfinite(weather->temperature)) return ORB_FACES_WEATHER_NONE;
    return s->online && !weather->stale ? ORB_FACES_WEATHER_CURRENT : ORB_FACES_WEATHER_STALE;
}

static const char *const s_weekdays[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

/* Polish #37: one date format per face. The shell may supply "Wednesday"
 * or a fixture "WED"; both become the canonical name, so the Orb rim always
 * reads the full weekday and the subdial faces always its three letters. */
static void weekday_canonical(char *weekday, size_t capacity)
{
    for (unsigned i = 0; i < 7 && strlen(weekday) >= 3; i++) {
        bool match = true;
        for (unsigned j = 0; weekday[j] && match; j++)
            match = s_weekdays[i][j] && (weekday[j] | 0x20) == (s_weekdays[i][j] | 0x20);
        if (match) { snprintf(weekday, capacity, "%s", s_weekdays[i]); return; }
    }
}

void orb_faces_weekday_short(char *weekday)
{
    /* Three uppercase letters, never shortened again at AX3 (polish #11). */
    for (unsigned i = 0; i < 3 && weekday[i]; i++)
        if (weekday[i] >= 'a' && weekday[i] <= 'z') weekday[i] -= 'a' - 'A';
    if (strlen(weekday) > 3) weekday[3] = '\0';
}

void orb_faces_day_parts(const orb_faces_snapshot_t *s, char *weekday, size_t weekday_size,
                         char *day, size_t day_size, char *clock, size_t clock_size)
{
    struct tm local = {0};
    bool valid = orb_faces_clock_valid(s->now) && localtime_r(&s->now, &local);
    snprintf(weekday, weekday_size, "%s", s->weekday);
    snprintf(day, day_size, "%s", s->day);
    snprintf(clock, clock_size, "%s", s->clock);
    if (!weekday[0]) {
        if (valid) strftime(weekday, weekday_size, "%A", &local);
        else snprintf(weekday, weekday_size, "Calendar");
    }
    weekday_canonical(weekday, weekday_size);
    if (!day[0] && valid) strftime(day, day_size, "%d", &local);
    if (!clock[0] && valid) strftime(clock, clock_size, "%H:%M", &local);
}

const char *orb_faces_weather_glyph(const orb_faces_snapshot_t *s)
{
    const orb_weather_snapshot_t *weather = &s->weather;
    orb_faces_weather_t state = orb_faces_weather_state(s);
    if (state == ORB_FACES_WEATHER_STALE) return ORB_FACES_STALE_GLYPH;
    const char *glyph = state == ORB_FACES_WEATHER_CURRENT ? orb_weather_icon(weather->condition, weather->is_day) : "";
    /* An unknown condition still names the complication with its own glyph. */
    return glyph[0] ? glyph : ORB_ICON_CLOUD;
}

void orb_faces_weather_short(const orb_faces_snapshot_t *s, char *value, size_t capacity)
{
    const orb_weather_snapshot_t *weather = &s->weather;
    if (orb_faces_weather_state(s) == ORB_FACES_WEATHER_NONE) snprintf(value, capacity, "—");
    else snprintf(value, capacity, "%.0f°", (double)weather->temperature);
}

bool orb_faces_complication_data(const orb_faces_snapshot_t *s, bool locked, orb_face_provider_t provider,
                                 orb_face_complication_t *out)
{
    if (!s || !out || provider >= ORB_FACE_PROVIDER_COUNT) return false;
    *out = (orb_face_complication_t){.percent = -1, .accent = ORB_LABEL};
    switch (provider) {
    case ORB_FACE_CLAUDE:
    case ORB_FACE_CODEX: {
        unsigned index = provider == ORB_FACE_CLAUDE ? 0 : 1;
        out->name = index ? "Codex usage" : "Claude usage";
        /* The shell's provider identity glyphs: shape, not hue, tells them apart. */
        out->glyph = index ? ORB_ICON_CODE : ORB_ICON_PROMPT;
        out->accent = index ? ORB_KIT_CODEX : ORB_KIT_CLAUDE;
        out->target = index ? ORB_FACES_OPEN_CODEX : ORB_FACES_OPEN_CLAUDE;
        out->percent = s->usage_percent[index];
        out->stale = out->percent >= 0 && (!s->online || s->usage_stale[index]);
        if (out->percent >= 0) snprintf(out->text, sizeof(out->text), "%d", out->percent);
        else snprintf(out->text, sizeof(out->text), "—");
        break;
    }
    case ORB_FACE_HOME:
        out->name = "Home"; out->glyph = ORB_ICON_HOUSE; out->accent = ORB_KIT_HOME;
        out->target = ORB_FACES_OPEN_HOME;
        /* Polish #42 (P3), decided: how many accessories are on is a presence
         * signal for anyone near a locked desk device, so Lock/Always On
         * keep the Home glyph and withhold the count and its ring. */
        if (locked) snprintf(out->text, sizeof(out->text), "%s", "");
        else if (s->home_total) {
            out->stale = !s->online || s->home_stale;
            out->percent = (int)(100u * s->home_on / s->home_total);
            snprintf(out->text, sizeof(out->text), "%u on", s->home_on);
        } else snprintf(out->text, sizeof(out->text), "—");
        break;
    case ORB_FACE_BATTERY:
        out->name = "Battery";
        out->glyph = s->charging ? ORB_ICON_BATTERY_CHARGING :
            s->battery_percent < 0 ? ORB_ICON_BATTERY_WARNING :
            s->battery_percent == 0 ? ORB_ICON_BATTERY_EMPTY :
            s->battery_percent <= 20 ? ORB_ICON_BATTERY_LOW :
            s->battery_percent <= 50 ? ORB_ICON_BATTERY_MEDIUM :
            s->battery_percent <= 80 ? ORB_ICON_BATTERY_HIGH : ORB_ICON_BATTERY_FULL;
        out->accent = !s->charging && s->battery_percent >= 0 && s->battery_percent <= 20 ? ORB_RED : ORB_GREEN; out->target = ORB_FACES_OPEN_BATTERY;
        out->percent = s->battery_percent;
        if (out->percent >= 0) snprintf(out->text, sizeof(out->text), "%d", out->percent);
        else snprintf(out->text, sizeof(out->text), "—");
        break;
    case ORB_FACE_WEATHER:
        out->name = "Weather"; out->glyph = orb_faces_weather_glyph(s);
        out->accent = ORB_KIT_HOME; out->target = ORB_FACES_OPEN_WEATHER;
        out->stale = orb_faces_weather_state(s) == ORB_FACES_WEATHER_STALE;
        orb_faces_weather_short(s, out->text, sizeof(out->text));
        /* A stale observation keeps the same geometry as its last reading.
         * An absent/flat range stays explicitly unavailable. */
        if (orb_faces_weather_state(s) != ORB_FACES_WEATHER_NONE && isfinite(s->weather.low) &&
            isfinite(s->weather.high) && s->weather.high > s->weather.low)
            out->percent = LV_CLAMP(0, (int)lroundf(100.f * (s->weather.temperature - s->weather.low) /
                                                (s->weather.high - s->weather.low)), 100);
        break;
    case ORB_FACE_NEXT:
        out->name = "Timer / alarm"; out->glyph = ORB_ICON_TIMER;
        out->accent = ORB_KIT_TIME; out->target = ORB_FACES_OPEN_TIME;
        if (s->next_seconds) {
            if (s->next_total_seconds)
                out->percent = LV_CLAMP(0, (int)(100ull * s->next_seconds / s->next_total_seconds), 100);
            /* Glossary §1.6 compact units, with a non-breaking space. */
            uint32_t minutes = (s->next_seconds + 59) / 60;
            if (minutes < 60) snprintf(out->text, sizeof(out->text), "%u\xC2\xA0min", (unsigned)minutes);
            else snprintf(out->text, sizeof(out->text), "%u\xC2\xA0hr", (unsigned)(minutes / 60));
        } else snprintf(out->text, sizeof(out->text), "—");
        break;
    case ORB_FACE_DATE: {
        char weekday[24], clock[16];
        orb_faces_day_parts(s, weekday, sizeof(weekday), out->text, sizeof(out->text), clock, sizeof(clock));
        out->name = "Calendar date"; out->glyph = ORB_ICON_CALENDAR;
        out->accent = ORB_KIT_DATE; out->target = ORB_FACES_OPEN_DATE;
        if (!out->text[0]) snprintf(out->text, sizeof(out->text), "—");
        struct tm local = {0};
        if (orb_faces_clock_valid(s->now) && localtime_r(&s->now, &local)) {
            static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            unsigned year = (unsigned)local.tm_year + 1900;
            unsigned total = days[local.tm_mon] + (local.tm_mon == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
            out->percent = (int)(100u * (unsigned)local.tm_mday / total);
        }
        break;
    }
    default: return false;
    }
    return true;
}

void orb_faces_activity_summary(const orb_faces_snapshot_t *s, char *out, size_t capacity)
{
    /* Polish #16: "0 working" is not a count of anything when nothing is
     * listed; offline uses the same words as the other faces' cue. */
    if (!s->online) snprintf(out, capacity, "Mac offline");
    else if (!s->sessions && !s->working) snprintf(out, capacity, "No sessions");
    else if (!s->working) snprintf(out, capacity, "All idle");
    else snprintf(out, capacity, "%u working", s->working);
}

