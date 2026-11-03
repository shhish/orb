#include "orb_stack.h"

#include "orb_icons.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

bool orb_stack_time_active(const orb_time_snapshot_t *time)
{
    return time->timer_state != ORB_TIME_IDLE || time->alarm_ringing || time->reminder_due ||
        time->stopwatch_state == ORB_TIME_RUNNING ||
        (time->stopwatch_state == ORB_TIME_PAUSED && time->stopwatch_elapsed_us > 0);
}

bool orb_stack_alarm_today(const orb_time_snapshot_t *time)
{
    if (!time->clock_valid || !time->alarm_enabled || time->alarm_next <= time->clock_now) return false;
    struct tm now, next;
    return localtime_r(&time->clock_now, &now) && localtime_r(&time->alarm_next, &next) &&
        now.tm_year == next.tm_year && now.tm_yday == next.tm_yday;
}

static bool home_lit(const orb_home_snapshot_t *home)
{
    if (!home || !home->online || !home->synced || !home->available) return false;
    for (unsigned i = 0; i < home->accessory_count; i++) {
        const orb_home_accessory_t *a = &home->accessories[i];
        if (a->kind == ORB_HOME_LIGHT && a->reachable &&
            (a->state.present & ORB_HOME_ON) && a->state.on) return true;
    }
    return false;
}

static int relevant_usage(const orb_model_snapshot_t *model, const orb_time_snapshot_t *time, unsigned provider)
{
    if (!model->online || !model->synced || model->usage.unknown[provider] ||
        !model->prefs.sources[provider]) return -1;
    int best = -1;
    for (unsigned i = 0; i < model->usage.count; i++) {
        const orb_usage_window_t *window = &model->usage.windows[i];
        if ((unsigned)window->provider != provider || window->stale || window->source == ORB_USAGE_UNKNOWN ||
            !isfinite(window->used_percent) ||
            window->used_percent < 0 || window->used_percent > 100) continue;
        /* Reset epochs are Unix seconds (protocol §Usage). Once a known reset
         * has passed this observation cannot truthfully describe pressure. */
        if (time->clock_valid && window->resets_at && window->resets_at <= (uint64_t)time->clock_now) continue;
        bool near_reset = time->clock_valid && window->resets_at > (uint64_t)time->clock_now &&
            window->resets_at - (uint64_t)time->clock_now <= 30u * 60u;
        if (window->used_percent < 70.0 && !near_reset) continue;
        if (best < 0 || window->used_percent > model->usage.windows[best].used_percent) best = (int)i;
    }
    return best;
}

unsigned orb_stack_build(const orb_model_snapshot_t *model, const orb_time_snapshot_t *time,
                         const orb_home_snapshot_t *home, int clock_hour, bool date_known,
                         orb_stack_entry_t *entries, unsigned capacity)
{
    unsigned count = 0;
#define ADD(type, value) do { \
    if (entries && count < capacity) entries[count] = (orb_stack_entry_t){type, value}; \
    count++; \
} while (0)
    /* Stable within wire priority; answering never leaves a stale action in
     * the glance order. The workspace retains an explicit delivery receipt. */
    for (int priority = ORB_PRIORITY_HIGH; priority >= ORB_PRIORITY_LOW; priority--)
        for (unsigned i = 0; i < model->card_count; i++)
            if (!model->cards[i].answered && (int)model->cards[i].priority == priority) ADD(ORB_STACK_CARD, i);
    bool active = orb_stack_time_active(time), alarm_today = orb_stack_alarm_today(time);
    bool urgent_time = time->timer_state == ORB_TIME_RUNNING || time->timer_state == ORB_TIME_FINISHED ||
        time->stopwatch_state == ORB_TIME_RUNNING || time->alarm_ringing || time->reminder_due;
    if (urgent_time) ADD(ORB_STACK_TIME_ACTIVE, 0);
    else if (alarm_today) ADD(ORB_STACK_TIME_ALARM, 0);
    bool lit = home_lit(home), night = clock_hour >= 0 && (clock_hour < 5 || clock_hour >= 21);
    if (lit && night) ADD(ORB_STACK_HOME, 0);
    if (model->online && model->synced && model->nowplaying_supported && model->nowplaying_valid)
        ADD(ORB_STACK_NOWPLAYING, 0);
    if (lit && !night) ADD(ORB_STACK_HOME, 0);
    /* A paused utility remains useful without outranking a live activity or
     * lights left on. This is the paused timer below Home in the Stack frame. */
    if (active && !urgent_time) ADD(ORB_STACK_TIME_ACTIVE, 0);
    for (unsigned provider = ORB_AGENT_CLAUDE; provider <= ORB_AGENT_CODEX; provider++) {
        int index = relevant_usage(model, time, provider);
        if (index >= 0) ADD(ORB_STACK_USAGE, (unsigned)index);
    }
    if (date_known || time->clock_valid)
        ADD(ORB_STACK_TIME_FLOOR, 0);
#undef ADD
    return count;
}

/* Stack metadata is derived once from the same model as its focused card. */
void orb_stack_time_caption(const orb_time_snapshot_t *time, char *text, size_t size, bool compact)
{
    if (time->reminder_due) snprintf(text, size, "%s", compact ? ORB_ICON_BELL " Reminder" : "Reminder");
    else if (time->alarm_ringing && time->timer_state == ORB_TIME_IDLE) snprintf(text, size, "%s", compact ? ORB_ICON_BELL " Alarm" : "Alarm");
    else if (time->timer_state == ORB_TIME_FINISHED)
        snprintf(text, size, "%s", compact ? ORB_ICON_TIMER " Done" : "0:00");
    else if (time->timer_state == ORB_TIME_IDLE && time->stopwatch_state != ORB_TIME_IDLE) {
        char elapsed[40];
        orb_time_format_duration(elapsed, sizeof(elapsed), time->stopwatch_elapsed_us, false);
        snprintf(text, size, "%s%s", compact ? ORB_ICON_TIMER " " : "", elapsed);
    }
    else {
        /* Same figures as the Time app: no zero-padded leading group ("3:30"). */
        char remaining[40];
        int64_t rounded = (time->timer_remaining_us + 999999) / 1000000 * 1000000;
        orb_time_format_duration(remaining, sizeof(remaining), rounded, false);
        snprintf(text, size, "%s%s", compact ? ORB_ICON_TIMER " " : "", remaining);
    }
}
static void stack_copy(char *out, size_t capacity, const char *source)
{
    if (!capacity) return;
    size_t length = strlen(source), count = length < capacity ? length : capacity - 1;
    /* A preview can shorten metadata, but never in the middle of a glyph. */
    if (count < length) while (count && ((unsigned char)source[count] & 0xC0) == 0x80) count--;
    memcpy(out, source, count); out[count] = '\0';
}

void orb_stack_preview_copy(const orb_model_snapshot_t *model, const orb_time_snapshot_t *time,
                            const char *room_name, orb_stack_entry_t entry, char *text, size_t capacity)
{
    switch (entry.kind) {
    case ORB_STACK_CARD: {
        const orb_card_t *card = &model->cards[entry.index];
        stack_copy(text, capacity, card->title[0] ? card->title : "Needs you");
        break;
    }
    case ORB_STACK_TIME_ACTIVE: {
        char value[48]; orb_stack_time_caption(time, value, sizeof(value), false);
        const char *name = time->reminder_due ? "Reminder" : time->alarm_ringing ? "Alarm" :
            time->timer_state != ORB_TIME_IDLE ? "Timer" : "Stopwatch";
        snprintf(text, capacity, "%s · %s", name, value);
        break;
    }
    case ORB_STACK_TIME_ALARM: {
        /* Reads like the Alarm screen: "7:00 AM". */
        char alarm[32];
        orb_time_format_clock(alarm, sizeof(alarm), time->alarm_hour, time->alarm_minute);
        snprintf(text, capacity, "Alarm · %s", alarm); break;
    }
    case ORB_STACK_HOME: {
        snprintf(text, capacity, "%s%s", room_name ? "Home · " : "Home", room_name ? room_name : ""); break;
    }
    case ORB_STACK_NOWPLAYING:
        stack_copy(text, capacity, model->nowplaying.title[0] ? model->nowplaying.title : "Now Playing"); break;
    case ORB_STACK_USAGE: {
        const orb_usage_window_t *window = &model->usage.windows[entry.index];
        snprintf(text, capacity, "%s · %.0f%%", window->provider == ORB_AGENT_CLAUDE ? "Claude" : "Codex", window->used_percent);
        break;
    }
    case ORB_STACK_TIME_FLOOR: snprintf(text, capacity, "Time"); break;
    default: text[0] = '\0'; break;
    }
}
