#include "orb_time.h"
#include "orb_time_voice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); } } while (0)

static time_t civil(int year, int month, int day, int hour, int minute, int dst)
{
    struct tm value = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
        .tm_hour = hour, .tm_min = minute, .tm_isdst = dst};
    return mktime(&value);
}

static void reject_at(const char *text, time_t now, const char *guidance)
{
    orb_time_init();
    orb_time_poll(1000, now);
    char reason[128];
    CHECK(!orb_time_reminder_from_voice(text, reason, sizeof(reason)));
    CHECK(reason[0]);
    if (guidance) CHECK(strstr(reason, guidance) != NULL);
    orb_reminder_t reminder;
    CHECK(!orb_time_reminder_get(0, &reminder));
}

static orb_reminder_t parse_at(const char *text, time_t now)
{
    orb_time_init();
    orb_time_poll(1000, now);
    char reason[128];
    CHECK(orb_time_reminder_from_voice(text, reason, sizeof(reason)));
    CHECK(!strcmp(reason, "Added to Time on this Orb."));
    orb_reminder_t reminder;
    CHECK(orb_time_reminder_get(0, &reminder));
    return reminder;
}

int main(void)
{
    setenv("TZ", "UTC0", 1); tzset();
    time_t now = civil(2026, 10, 28, 23, 59, -1);
    orb_reminder_t reminder = parse_at("Remind me in five minutes to Take a break", now);
    CHECK(reminder.due_time == now + 300 && !strcmp(reminder.title, "Take a break"));
    reminder = parse_at("remind me in an hour to Call home", now);
    CHECK(reminder.due_time == now + 3600);
    reminder = parse_at("remind me at 12:01 AM to Lock up", now);
    CHECK(reminder.due_time == now + 120);
    reminder = parse_at("remind me at 00:01 to Lock up", now);
    CHECK(reminder.due_time == now + 120);
    struct {
        const char *text;
        unsigned seconds;
    } relative[] = {
        {"  REMIND me in a second to Check  ", 1},
        {"remind me in twelve seconds to Check", 12},
        {"remind me in 90 minutes to Check", 5400},
        {"remind me in 24 hours to Check", 86400},
        {"remind me in 365 days to Check", 365u * 86400u},
    };
    for (unsigned i = 0; i < sizeof(relative) / sizeof(relative[0]); ++i) {
        reminder = parse_at(relative[i].text, now);
        CHECK(reminder.due_time == now + relative[i].seconds);
        CHECK(!strcmp(reminder.title, "Check"));
    }
    const char *invalid[] = {NULL, "", "in five minutes", "remind me in 0 minutes to Wait",
        "remind me in 9999999999 hours to Wait", "remind me in 5 bananas to Wait",
        "remind me at 25:00 to Wait", "remind me at 6:99 PM to Wait", "remind me at 6 PM to Wait",
        "remind me at 00:00 AM to Wait", "remind me in 1 minute to ",
        "remind me in -1 minutes to Wait", "remind me in 1.5 hours to Wait",
        "remind me in 366 days to Wait", "remind me in 31536001 seconds to Wait",
        "remind me at 6:30 UTC to Wait", "remind me at 6:3 PM to Wait",
        "remind me at 6:300 PM to Wait", "remind me at 123:00 to Wait",
        "remind me at 13:00 AM to Wait", "remind me at 1:00 PM tomorrow to Wait",
        "remind me in two hundred minutes to Wait",
        "remind me in 1 minute", "remind me in 1 minute to 1234567890123456789012345678901234567890123456789012345678901234"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        orb_time_init(); orb_time_poll(1000, now);
        char reason[128];
        CHECK(!orb_time_reminder_from_voice(invalid[i], reason, sizeof(reason)));
        CHECK(reason[0]);
        CHECK(!orb_time_reminder_get(0, &reminder));
    }
    orb_time_init(); orb_time_poll(1000, now); orb_time_set_clock_format(false);
    char reason[128];
    CHECK(!orb_time_reminder_from_voice("remind me at 6:30 to Call home", reason, sizeof(reason)));
    CHECK(strstr(reason, "AM or PM") != NULL);
    orb_time_init();
    CHECK(!orb_time_reminder_from_voice("remind me in 5 minutes to Call home", reason, sizeof(reason)));
    CHECK(strstr(reason, "clock") != NULL);
    /* Civil clocks roll through calendar boundaries, independent of duration
     * parsing; an exact current minute is tomorrow, never immediately due. */
    now = civil(2026, 12, 31, 23, 59, -1);
    reminder = parse_at("remind me at 12:00 a.m. to New year", now);
    CHECK(reminder.due_time == civil(2027, 1, 1, 0, 0, -1));
    now = civil(2028, 2, 28, 23, 59, -1);
    reminder = parse_at("remind me at 00:01 to Leap", now);
    CHECK(reminder.due_time == civil(2028, 2, 29, 0, 1, -1));
    now = civil(2026, 4, 30, 12, 0, -1);
    reminder = parse_at("remind me at 12:00 p.m. to Tomorrow", now);
    CHECK(reminder.due_time == civil(2026, 5, 1, 12, 0, -1));
    char text[128] = "remind me in 1 second to ";
    size_t prefix_length = strlen(text);
    memset(text + prefix_length, 'x', 63);
    text[prefix_length + 63] = 0;
    reminder = parse_at(text, now);
    CHECK(strlen(reminder.title) == 63);
    strcpy(text, "remind me in 1 second to ");
    memset(text + prefix_length, 'x', 61);
    strcpy(text + prefix_length + 61, "é");
    reminder = parse_at(text, now);
    CHECK(strlen(reminder.title) == 63 && !strcmp(reminder.title + 61, "é"));
    reminder = parse_at("remind me in 1 second to Bad \377 input", now);
    CHECK(!strcmp(reminder.title, "Bad ? input"));
    orb_time_init(); orb_time_poll(1000, now);
    for (unsigned i = 0; i < ORB_TIME_REMINDERS_MAX; ++i)
        CHECK(orb_time_reminder_from_voice("remind me in 1 minute to Wait", NULL, 0));
    CHECK(!orb_time_reminder_from_voice("remind me in 1 minute to Full", reason, sizeof(reason)));
    CHECK(strstr(reason, "full") != NULL);
    CHECK(orb_time_reminder_get(0, &reminder) && orb_time_reminder_done(reminder.id));
    CHECK(orb_time_reminder_from_voice("remind me in 1 minute to Reused", reason, 1));
    CHECK(reason[0] == 0);
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1); tzset();
    now = civil(2026, 3, 8, 1, 50, -1);
    reminder = parse_at("remind me at 2:30 AM to Spring", now);
    CHECK(reminder.due_time == civil(2026, 3, 8, 3, 30, -1));
    now = civil(2026, 11, 1, 1, 40, 1);
    reminder = parse_at("remind me at 1:30 AM to Fall", now);
    CHECK(reminder.due_time == civil(2026, 11, 1, 1, 30, 0));
    /* Half-hour DST changes must preserve the same civil normalization. */
    setenv("TZ", "LHST-10:30LHDT-11,M10.1.0/2,M4.1.0/2", 1); tzset();
    now = civil(2026, 10, 4, 1, 50, -1);
    reminder = parse_at("remind me at 2:15 AM to Spring", now);
    CHECK(reminder.due_time == civil(2026, 10, 4, 2, 45, -1));
    now = civil(2026, 4, 5, 1, 50, 1);
    reminder = parse_at("remind me at 1:45 AM to Fall", now);
    CHECK(reminder.due_time == civil(2026, 4, 5, 1, 45, 0));
    setenv("TZ", "UTC0", 1); tzset();
    /* A valid source clock can still produce a due date outside the model's
     * supported civil range. It must not be diagnosed as a full empty list. */
    reminder = parse_at("remind me in 1 second to Last valid", (time_t)(ORB_TIME_CLOCK_MAX - 1));
    CHECK((uint64_t)reminder.due_time == ORB_TIME_CLOCK_MAX);
    reject_at("remind me in 2 seconds to Wait", (time_t)(ORB_TIME_CLOCK_MAX - 1), "supported");
    reject_at("remind me at 00:00 to Wait", (time_t)ORB_TIME_CLOCK_MAX, "supported");
    reject_at("remind me in 1 minute to \001\002\177", now, "title");
    char oversized[282];
    memset(oversized, ' ', sizeof(oversized));
    const char suffix[] = "remind me in 1 minute to Wait";
    memcpy(oversized + sizeof(oversized) - sizeof(suffix), suffix, sizeof(suffix));
    reject_at(oversized, now, "Say:");
    printf("Time voice: %u checks passed\n", s_checks);
    return 0;
}
