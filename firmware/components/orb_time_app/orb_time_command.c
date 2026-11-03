#include "orb_time_command.h"
#include "orb_time.h"

bool orb_time_command_execute(const orb_time_command_t *c)
{
    if (!c) return false;
    switch (c->kind) {
    case ORB_TIME_COMMAND_TIMER_SET: return orb_time_timer_set(c->a);
    case ORB_TIME_COMMAND_ALARM_SET: return orb_time_alarm_set(c->a, c->b);
    case ORB_TIME_COMMAND_ALARM_REPEAT: return orb_time_alarm_repeat_set((orb_time_repeat_t)c->a, (uint8_t)c->b);
    case ORB_TIME_COMMAND_ALARM_SNOOZE_SET: return orb_time_alarm_snooze_set(c->a);
    case ORB_TIME_COMMAND_TIMER_START: return orb_time_timer_start(c->at);
    case ORB_TIME_COMMAND_TIMER_PAUSE: orb_time_timer_pause(c->at); return true;
    case ORB_TIME_COMMAND_TIMER_RESET: orb_time_timer_reset(); return true;
    case ORB_TIME_COMMAND_STOPWATCH_START: return orb_time_stopwatch_start(c->at);
    case ORB_TIME_COMMAND_STOPWATCH_PAUSE: orb_time_stopwatch_pause(c->at); return true;
    case ORB_TIME_COMMAND_STOPWATCH_RESET: orb_time_stopwatch_reset(); return true;
    case ORB_TIME_COMMAND_STOPWATCH_LAP: return orb_time_stopwatch_lap(c->at);
    case ORB_TIME_COMMAND_ALARM_ENABLE: return orb_time_alarm_enable(c->a != 0, (time_t)c->at);
    case ORB_TIME_COMMAND_ALARM_SNOOZE: return orb_time_alarm_snooze(c->a);
    case ORB_TIME_COMMAND_DISMISS: orb_time_dismiss(c->a != 0, c->b != 0); return true;
    case ORB_TIME_COMMAND_REMINDER_SNOOZE: return orb_time_reminder_snooze(c->a, c->b);
    case ORB_TIME_COMMAND_REMINDER_DONE: return orb_time_reminder_done(c->a);
    case ORB_TIME_COMMAND_CLOCK_FORMAT: orb_time_set_clock_format(c->a != 0); return true;
    case ORB_TIME_COMMAND_PERSIST_RETRY: orb_time_persist_retry(); return true;
    }
    return false;
}

bool orb_time_command(const orb_time_commands_t *port, const orb_time_command_t *command)
{
    return port && port->send ? port->send(port->context, command) : orb_time_command_execute(command);
}
