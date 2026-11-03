#pragma once

#include <stdbool.h>
#include <stdint.h>

/* One Time service change the presenter decided on. A host may carry it as a
 * typed screen intent; without a host the presenter calls the unchanged
 * orb_time setter directly. Times and IDs are the presenter's own values. */
typedef enum {
    /* Value edits of draft settings; none starts, saves or dismisses. */
    ORB_TIME_COMMAND_TIMER_SET = 1,       /* a: seconds */
    ORB_TIME_COMMAND_ALARM_SET,           /* a: hour, b: minute */
    ORB_TIME_COMMAND_ALARM_REPEAT,        /* a: orb_time_repeat_t, b: weekday mask */
    ORB_TIME_COMMAND_ALARM_SNOOZE_SET,    /* a: seconds */
    /* Explicit activations. */
    ORB_TIME_COMMAND_TIMER_START,         /* at: monotonic µs */
    ORB_TIME_COMMAND_TIMER_PAUSE,         /* at: monotonic µs */
    ORB_TIME_COMMAND_TIMER_RESET,
    ORB_TIME_COMMAND_STOPWATCH_START,     /* at: monotonic µs */
    ORB_TIME_COMMAND_STOPWATCH_PAUSE,     /* at: monotonic µs */
    ORB_TIME_COMMAND_STOPWATCH_RESET,
    ORB_TIME_COMMAND_STOPWATCH_LAP,       /* at: monotonic µs */
    ORB_TIME_COMMAND_ALARM_ENABLE,        /* a: enabled, at: wall clock */
    ORB_TIME_COMMAND_ALARM_SNOOZE,        /* a: seconds */
    ORB_TIME_COMMAND_DISMISS,             /* a: timer, b: alarm */
    ORB_TIME_COMMAND_REMINDER_SNOOZE,     /* a: reminder ID, b: seconds */
    ORB_TIME_COMMAND_REMINDER_DONE,       /* a: reminder ID */
    ORB_TIME_COMMAND_CLOCK_FORMAT,        /* a: 24-hour */
    ORB_TIME_COMMAND_PERSIST_RETRY,
} orb_time_command_kind_t;

typedef struct {
    orb_time_command_kind_t kind;
    int64_t at;
    uint32_t a, b;
} orb_time_command_t;

typedef struct {
    bool (*send)(void *context, const orb_time_command_t *command);
    void *context;
} orb_time_commands_t;

/* Returns the setter's result (true for setters without one), so presenter
 * feedback follows the same acceptance it always did. UI thread only. */
bool orb_time_command(const orb_time_commands_t *port, const orb_time_command_t *command);
bool orb_time_command_execute(const orb_time_command_t *command);
