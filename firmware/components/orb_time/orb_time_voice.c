#include "orb_time_voice.h"
#include "orb_time.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool result(char *reason, size_t capacity, const char *message, bool ok)
{
    if (reason && capacity) snprintf(reason, capacity, "%s", message);
    return ok;
}

static const char *spaces(const char *p)
{
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

static bool word(const char **p, const char *value)
{
    size_t length = strlen(value);
    if (strncasecmp(*p, value, length) || ((*p)[length] && !isspace((unsigned char)(*p)[length]))) return false;
    *p = spaces(*p + length);
    return true;
}

static unsigned number(const char **p)
{
    static const char *const words[] = {"one", "two", "three", "four", "five", "six",
        "seven", "eight", "nine", "ten", "eleven", "twelve"};
    if (word(p, "a") || word(p, "an")) return 1;
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        if (word(p, words[i])) return i + 1;
    unsigned value = 0, count = 0;
    while (isdigit((unsigned char)**p)) {
        if (++count > 6) return 0;
        value = value * 10 + (unsigned)(*(*p)++ - '0');
    }
    if (!count) return 0;
    *p = spaces(*p);
    return value;
}

static time_t next_clock(time_t now, unsigned hour, unsigned minute)
{
    struct tm today;
    if (!localtime_r(&now, &today)) return 0;
    /* Enumerate both sides of a fall-back fold, and normalize a spring gap
     * through libc's local timezone rules rather than adding 86400 seconds. */
    for (unsigned day = 0; day < 3; ++day) {
        time_t best = 0, normalized = 0;
        for (int dst = -1; dst <= 1; ++dst) {
            struct tm candidate = today;
            candidate.tm_mday += (int)day;
            candidate.tm_hour = (int)hour;
            candidate.tm_min = (int)minute;
            candidate.tm_sec = 0;
            candidate.tm_isdst = dst;
            time_t value = mktime(&candidate);
            if (dst == -1 && value > now) normalized = value;
            if (value <= now || candidate.tm_hour != (int)hour || candidate.tm_min != (int)minute) continue;
            if (!best || value < best) best = value;
        }
        if (best) return best;
        if (normalized) return normalized;
    }
    return 0;
}

bool orb_time_reminder_from_voice(const char *text, char *reason, size_t capacity)
{
    const char *usage = "Say: remind me in 5 minutes to take a break.";
    if (!text || !text[0] || strnlen(text, 281) > 280) return result(reason, capacity, usage, false);
    const char *p = spaces(text);
    if (!word(&p, "remind") || !word(&p, "me")) return result(reason, capacity, usage, false);
    orb_time_snapshot_t state;
    orb_time_snapshot(orb_time_now_us(), &state);
    if (!state.clock_valid) return result(reason, capacity, "Connect to your Mac to set the clock first.", false);
    time_t due = 0;
    if (word(&p, "in")) {
        unsigned amount = number(&p), unit = 0;
        if (word(&p, "second") || word(&p, "seconds")) unit = 1;
        else if (word(&p, "minute") || word(&p, "minutes")) unit = 60;
        else if (word(&p, "hour") || word(&p, "hours")) unit = 3600;
        else if (word(&p, "day") || word(&p, "days")) unit = 86400;
        uint64_t delay = (uint64_t)amount * unit;
        if (!delay || delay > 365u * 86400u || (uint64_t)state.clock_now > INT64_MAX - delay)
            return result(reason, capacity, "Choose a delay from 1 second to 365 days.", false);
        due = state.clock_now + (time_t)delay;
    } else if (word(&p, "at")) {
        unsigned hour = 0, minute = 0, digits = 0;
        while (isdigit((unsigned char)*p) && digits++ < 2) hour = hour * 10 + (unsigned)(*p++ - '0');
        if (!digits || *p++ != ':' || !isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]))
            return result(reason, capacity, "Say a clock time, such as 6:30 PM.", false);
        minute = (unsigned)(p[0] - '0') * 10 + (unsigned)(p[1] - '0');
        p = spaces(p + 2);
        bool pm = false, meridiem = false;
        if (word(&p, "AM") || word(&p, "a.m.")) meridiem = true;
        else if (word(&p, "PM") || word(&p, "p.m.")) meridiem = pm = true;
        if (minute > 59 || hour > 23 || (meridiem && (!hour || hour > 12)))
            return result(reason, capacity, "That clock time is out of range.", false);
        if (!meridiem && !state.hour24 && hour > 0 && hour <= 12)
            return result(reason, capacity, "Include AM or PM in the time.", false);
        if (meridiem) hour = hour % 12 + (pm ? 12 : 0);
        due = next_clock(state.clock_now, hour, minute);
        if (!due) return result(reason, capacity, "That local time is unavailable. Try a delay instead.", false);
    } else return result(reason, capacity, usage, false);
    if (!word(&p, "to")) return result(reason, capacity, usage, false);
    /* The relative-delay bound alone cannot protect the supported civil range
     * near its end. Report the date problem before the store's capacity check. */
    if (due < (time_t)ORB_TIME_CLOCK_MIN || (uint64_t)due > ORB_TIME_CLOCK_MAX)
        return result(reason, capacity, "That due date is outside the supported clock range.", false);
    size_t title_length = strlen(p);
    while (title_length && isspace((unsigned char)p[title_length - 1])) --title_length;
    if (!title_length || title_length >= sizeof(((orb_reminder_t *)0)->title))
        return result(reason, capacity, "Use a reminder title of 1 to 63 UTF-8 bytes.", false);
    bool visible_title = false;
    for (size_t i = 0; i < title_length; ++i)
        if ((unsigned char)p[i] > 0x20 && (unsigned char)p[i] != 0x7F) visible_title = true;
    if (!visible_title)
        return result(reason, capacity, "Use a reminder title of 1 to 63 UTF-8 bytes.", false);
    char title[64];
    memcpy(title, p, title_length);
    title[title_length] = 0;
    if (!orb_time_reminder_set(title, due, NULL))
        return result(reason, capacity, "Reminder list is full. Complete one and try again.", false);
    return result(reason, capacity, "Added to Time on this Orb.", true);
}
