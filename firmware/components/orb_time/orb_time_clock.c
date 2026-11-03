#include "orb_time_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

bool orb_time_clock_valid(time_t now)
{
    /* Provider corruption must never overflow civil date arithmetic. */
    return now >= (time_t)ORB_TIME_CLOCK_MIN && (uint64_t)now <= ORB_TIME_CLOCK_MAX;
}

void orb_time_format_duration(char *out, size_t capacity, int64_t us, bool hundredths)
{
    if (!out || !capacity) return;
    uint64_t micros = us > 0 ? (uint64_t)us : 0;
    uint64_t seconds = micros / 1000000;
    unsigned minutes = (unsigned)(seconds / 60 % 60);
    unsigned remainder = (unsigned)(seconds % 60);
    int used;
    /* HIG §§1/4: omit redundant zero groups so the significant time leads.
     * The same boundary applies to the counter, pinned header and lap splits. */
    if (seconds >= 3600)
        used = snprintf(out, capacity, "%" PRIu64 ":%02u:%02u", seconds / 3600, minutes, remainder);
    else
        used = snprintf(out, capacity, "%u:%02u", minutes, remainder);
    if (hundredths && used >= 0 && (size_t)used < capacity)
        snprintf(out + used, capacity - (size_t)used, ".%02u", (unsigned)(micros / 10000 % 100));
}

void orb_time_format_stopwatch(char *out, size_t capacity, int64_t us)
{
    if (!out || !capacity) return;
    uint64_t micros = us > 0 ? (uint64_t)us : 0;
    uint64_t seconds = micros / 1000000;
    unsigned hundredths = (unsigned)(micros / 10000 % 100);
    /* The hour group appears only once it carries information; below it the
     * minutes keep two digits so a lap list's splits align (HIG §4). */
    if (seconds >= 3600)
        snprintf(out, capacity, "%" PRIu64 ":%02u:%02u.%02u", seconds / 3600,
                 (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60), hundredths);
    else
        snprintf(out, capacity, "%02u:%02u.%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60), hundredths);
}

uint8_t orb_time_repeat_days(orb_time_repeat_t repeat, uint8_t custom)
{
    switch (repeat) {
    case ORB_TIME_REPEAT_WEEKDAYS: return ORB_TIME_DAYS_WEEKDAYS;
    case ORB_TIME_REPEAT_WEEKENDS: return ORB_TIME_DAYS_WEEKENDS;
    case ORB_TIME_REPEAT_DAILY: return ORB_TIME_DAYS_ALL;
    case ORB_TIME_REPEAT_CUSTOM: return custom;
    default: return 0;
    }
}

static int civil_day(const struct tm *local)
{
    return (local->tm_year + 1900) * 10000 + (local->tm_mon + 1) * 100 + local->tm_mday;
}

/* A repeated hour rings at its first occurrence only. A nonexistent spring
 * hour follows libc's forward normalization (02:30 becomes 03:30). Checking
 * both DST interpretations avoids libc's history-dependent fold selection. */
time_t orb_time_civil_alarm(int day, unsigned hour, unsigned minute)
{
    struct tm selected = {.tm_year = day / 10000 - 1900, .tm_mon = day / 100 % 100 - 1,
        .tm_mday = day % 100, .tm_hour = (int)hour, .tm_min = (int)minute, .tm_isdst = -1};
    time_t normalized = mktime(&selected), first = 0, forward = 0;
    for (int dst = 0; dst <= 1; ++dst) {
        struct tm candidate = {.tm_year = day / 10000 - 1900, .tm_mon = day / 100 % 100 - 1,
            .tm_mday = day % 100, .tm_hour = (int)hour, .tm_min = (int)minute, .tm_isdst = dst};
        time_t epoch = mktime(&candidate);
        struct tm check;
        if (epoch != (time_t)-1 && localtime_r(&epoch, &check) && civil_day(&check) == day &&
            check.tm_hour == (int)hour && check.tm_min == (int)minute && (!first || epoch < first)) first = epoch;
        if (epoch != (time_t)-1 && localtime_r(&epoch, &check) &&
            (civil_day(&check) > day || (civil_day(&check) == day &&
            check.tm_hour * 60 + check.tm_min > (int)(hour * 60 + minute))) &&
            (!forward || epoch < forward)) forward = epoch;
    }
    return first ? first : forward ? forward : normalized;
}

time_t orb_time_next_alarm(time_t wall_now, unsigned hour, unsigned minute,
                         uint8_t days, int last_day, int *out_day)
{
    if (!orb_time_clock_valid(wall_now)) return 0;
    struct tm local;
    if (!localtime_r(&wall_now, &local)) return 0;
    /* Advancing a calendar day through noon, rather than adding 86400, keeps
     * the requested weekday and wall time across both DST boundaries. */
    for (unsigned offset = 0; offset < 8; ++offset) {
        struct tm date = local;
        date.tm_mday += (int)offset;
        date.tm_hour = 12; date.tm_min = date.tm_sec = 0; date.tm_isdst = -1;
        if (mktime(&date) == (time_t)-1) continue;
        int day = civil_day(&date);
        /* Suppress only the last occurrence, not all earlier dates: a
         * corrected clock may legitimately move back a week or a year. */
        if (day == last_day || (days && !(days & (1u << (unsigned)date.tm_wday)))) continue;
        time_t next = orb_time_civil_alarm(day, hour, minute);
        if (next > wall_now) { if (out_day) *out_day = day; return next; }
    }
    return 0;
}

void orb_time_format_repeat(char *out, size_t capacity, orb_time_repeat_t repeat, uint8_t days)
{
    if (!out || !capacity) return;
    const char *name = repeat == ORB_TIME_REPEAT_WEEKDAYS ? "Weekdays" :
        repeat == ORB_TIME_REPEAT_WEEKENDS ? "Weekends" :
        repeat == ORB_TIME_REPEAT_DAILY ? "Every day" : "One time";
    if (repeat != ORB_TIME_REPEAT_CUSTOM) { snprintf(out, capacity, "%s", name); return; }
    size_t used = 0;
    out[0] = '\0';
    /* Match the calendar's Monday-first work week, with localized day names. */
    for (unsigned index = 1; index <= 7; ++index) {
        unsigned day = index % 7;
        if (!(days & (1u << day))) continue;
        struct tm value = {.tm_wday = (int)day};
        char label[24] = "";
        (void)strftime(label, sizeof(label), "%a", &value);
        int wrote = snprintf(out + used, capacity - used, "%s%s", used ? ", " : "", label);
        if (wrote < 0 || (size_t)wrote >= capacity - used) return;
        used += (size_t)wrote;
    }
}

/* Keep a complete, valid UTF-8 title even for malformed voice/provider input.
 * Embedded controls collapse to spaces; labels never receive escape controls. */
void orb_time_copy_utf8(char *out, size_t capacity, const char *text)
{
    size_t used = 0;
    const unsigned char *source = (const unsigned char *)text;
    while (*source && used + 1 < capacity) {
        unsigned lead = *source;
        size_t length = lead < 0x80 ? 1 : lead >= 0xC2 && lead <= 0xDF ? 2 :
            lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
        bool valid = length != 0;
        for (size_t k = 1; valid && k < length; ++k)
            if (!source[k] || (source[k] & 0xC0) != 0x80) valid = false;
        if (valid && length >= 3 && ((lead == 0xE0 && source[1] < 0xA0) ||
            (lead == 0xED && source[1] >= 0xA0) || (lead == 0xF0 && source[1] < 0x90) ||
            (lead == 0xF4 && source[1] >= 0x90))) valid = false;
        if (!valid) { out[used++] = '?'; ++source; continue; }
        if (used + length >= capacity) break;
        if (lead < 0x20 || lead == 0x7F) out[used++] = ' ';
        else { memcpy(out + used, source, length); used += length; }
        source += length;
    }
    while (used && out[used - 1] == ' ') --used;
    out[used] = '\0';
}
