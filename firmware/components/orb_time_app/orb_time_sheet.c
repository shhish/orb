#include "orb_time_view_internal.h"
#include "orb_sound.h"

#include <stdio.h>
#include <string.h>

void tv_reminder_when(const orb_reminder_t *reminder, time_t now, char *out, size_t capacity, bool due)
{
    struct tm local, today;
    out[0] = 0;
    if (!localtime_r(&reminder->due_time, &local)) return;
    char clock[32], date[32];
    orb_time_format_clock(clock, sizeof(clock), local.tm_hour, local.tm_min);
    strftime(date, sizeof(date), "%a %d %b", &local);
    /* Relative days read faster and keep the line short at AX3. */
    time_t tomorrow = now + 86400;
    if (now && localtime_r(&now, &today) && today.tm_yday == local.tm_yday && today.tm_year == local.tm_year)
        snprintf(date, sizeof(date), "Today");
    else if (now && localtime_r(&tomorrow, &today) && today.tm_yday == local.tm_yday && today.tm_year == local.tm_year)
        snprintf(date, sizeof(date), "Tomorrow");
    /* HIG §§5/8: a due entry says so in words, not only by its glyph. */
    if (due) snprintf(out, capacity, "Due · %s", clock);
    else snprintf(out, capacity, "%s · %s", date, clock);
}

/* Refresh only when the actual rows change. Timer and persistence activity
 * must not destroy a reminder contact or reading position (HIG §§1/7). Civil
 * display text is included so a timezone/hour-cycle change refreshes it too. */
uint32_t tv_reminder_signature(const orb_time_view_t *v)
{
    uint32_t hash = 2166136261u ^ v->model.save_error;
    for (unsigned i = 0; i < v->model.reminder_count; ++i) {
        orb_reminder_t reminder;
        if (!orb_time_reminder_get(i, &reminder)) break;
        char when[96], row[192];
        tv_reminder_when(&reminder, v->model.clock_now, when, sizeof(when), reminder.due);
        snprintf(row, sizeof(row), "%u|%s|%s", (unsigned)reminder.id, reminder.title, when);
        for (const unsigned char *p = (const unsigned char *)row; *p; ++p)
            hash = (hash ^ *p) * 16777619u;
    }
    return hash;
}

static void reminder_action(lv_event_t *event)
{
    orb_time_view_t *v = lv_event_get_user_data(event);
    if (v->paging || v->drag.active || v->preview) return;
    uint32_t id = (uint32_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    orb_time_snapshot(orb_time_now_us(), &v->model);
    for (unsigned i = 0; i < v->model.reminder_count; ++i) {
        orb_reminder_t reminder;
        if (!orb_time_reminder_get(i, &reminder) || reminder.id != id) continue;
        tv_remember(v); v->reminder_id = id; v->reminder_scroll = 0;
        v->reminder_detail = true; orb_sound_play(ORB_SOUND_OPEN); tv_build(v); return;
    }
}

/* Every Time row opts into the kit's two-line titles, so Dynamic Type grows a
 * row rather than truncating it (review findings 5/6). */
static lv_obj_t *row(orb_time_view_t *v, lv_obj_t *list, const orb_kit_row_config_t *config,
                     int command, orb_text_variant_t title, orb_text_variant_t subtitle)
{
    lv_obj_t *obj = orb_kit_row_create(list, config);
    if (title) orb_kit_row_set_text_role(obj, ORB_KIT_ROW_TEXT_TITLE, title);
    if (subtitle) orb_kit_row_set_text_role(obj, ORB_KIT_ROW_TEXT_SUBTITLE, subtitle);
    orb_kit_row_wrap_title(obj, title);
    if (command >= 0) tv_bind(v, obj, command);
    return obj;
}

/* HIG §4 (Dynamic Type) and review finding 5: at accessibility sizes a
 * value moves under its label, as iOS stacks a cell, so "Repeat" is never
 * cut to make room for "One time". */
static lv_obj_t *option(orb_time_view_t *v, const char *name, const char *glyph, int command,
                        const char *reserve)
{
    bool stacked = v->size == ORB_CONTENT_SIZE_AX3;
    lv_obj_t *obj = row(v, v->list, &(orb_kit_row_config_t){
        .title = name, .glyph = glyph, .accent = TV_ACCENT,
        .subtitle = stacked && reserve ? reserve : NULL, .value = !stacked && reserve ? "" : NULL,
    }, command, 0, 0);
    if (reserve) orb_kit_row_set_live_text(obj, stacked ? ORB_KIT_ROW_TEXT_SUBTITLE : ORB_KIT_ROW_TEXT_VALUE,
        "", reserve);
    return obj;
}

static void option_value(const orb_time_view_t *v, lv_obj_t *obj, const char *value)
{
    orb_kit_row_set_live_text(obj, v->size == ORB_CONTENT_SIZE_AX3 ? ORB_KIT_ROW_TEXT_SUBTITLE :
        ORB_KIT_ROW_TEXT_VALUE, value, NULL);
}

void tv_sheet_update(orb_time_view_t *v)
{
    if (v->sheet != 2) return;
    char repeat[64], line[32];
    orb_time_format_repeat(repeat, sizeof(repeat), v->model.alarm_repeat, v->model.alarm_days);
    /* The next row exposes every selected weekday. This summary stays
     * bounded while the crown steps through repeat modes. */
    option_value(v, v->repeat_label, v->model.alarm_repeat == ORB_TIME_REPEAT_CUSTOM ? "Custom" : repeat);
    snprintf(line, sizeof(line), "%u min", (unsigned)v->model.alarm_snooze_seconds / 60);
    option_value(v, v->snooze_label, line);
    option_value(v, v->format_label, v->model.hour24 ? "24-hour" : "12-hour");
    orb_kit_row_set_focus(v->repeat_label, v->field == 3);
    orb_kit_row_set_focus(v->snooze_label, v->field == 4);
}

/* HIG §7: identify the action with its real glyph. Presets are stopwatch
 * durations, while Last used has the separate history arrow. */
static void build_choices(orb_time_view_t *v)
{
    unsigned offset = v->sheet == 1 && !v->model.timer_last_seconds ? 1 : 0;
    unsigned count = v->sheet == 1 ? 6 - offset : 7;
    for (unsigned i = 0; i < count; ++i) {
        char label[96];
        if (v->sheet == 1) {
            static const unsigned minutes[] = {0, 1, 3, 5, 10, 25};
            if (i + offset) snprintf(label, sizeof(label), "%u min", minutes[i + offset]);
            else {
                char last[40];
                orb_time_format_duration(last, sizeof(last), (int64_t)v->model.timer_last_seconds * 1000000, false);
                snprintf(label, sizeof(label), "Last used · %s", last);
            }
        } else {
            struct tm day = {.tm_wday = (int)i};
            strftime(label, sizeof(label), "%A", &day);
        }
        /* Review finding 19: a day is a checklist item. Off is an empty
         * circle on grey, on is a check on teal; the focus ring stays focus. */
        bool on = v->sheet == 3 && (v->model.alarm_days & (1u << i));
        lv_obj_t *obj = row(v, v->list, &(orb_kit_row_config_t){
            .title = label, .accent = v->sheet == 3 && !on ? ORB_KIT_GREY : TV_ACCENT,
            .glyph = v->sheet == 1 ? (i + offset ? ORB_ICON_STOPWATCH : ORB_ICON_RECENTS) :
                on ? ORB_ICON_CHECK : ORB_ICON_CIRCLE,
        }, (v->sheet == 1 ? PRESET_BASE + (int)offset : DAY_BASE) + (int)i, v->sheet == 1 ? ORB_TEXT_TABULAR : 0, 0);
        if (v->sheet == 3) lv_obj_set_state(obj, LV_STATE_CHECKED, on);
    }
}

void tv_build_sheet(orb_time_view_t *v)
{
    v->title = tv_rim_title(v, v->sheet == 1 ? "Quick timers" : v->sheet == 2 ? "Alarm options" : "Custom days");
    lv_obj_t *close = orb_kit_disc_create(v->content, ORB_KIT_DISC_KIND_MINI,
        ORB_KIT_NEUTRAL_GLASS, TV_ACCENT, ORB_ICON_X);
    lv_obj_set_pos(close, 73, 73); tv_bind(v, close, SHEET_CLOSE); tv_semantic(close, "Close");
    v->list = tv_sheet_list(v);
    if (v->sheet == 2) {
        lv_obj_set_height(v->list, 184);
        /* The empty initial rails do not describe their full editing range.
         * Reserve before the first update, so 9→10 minutes stays immovable. */
        v->repeat_label = option(v, "Repeat", ORB_ICON_REPEAT, REPEAT_EDIT, "Custom days");
        option(v, "Custom days", ORB_ICON_CALENDAR, CUSTOM_EDIT, NULL);
        v->snooze_label = option(v, "Snooze", ORB_ICON_ZZZ, SNOOZE_EDIT, "88 min");
        v->format_label = option(v, "Time", ORB_ICON_CLOCK, CLOCK_FORMAT, "24-hour");
        orb_kit_row_set_text_role(v->snooze_label, v->size == ORB_CONTENT_SIZE_AX3 ?
            ORB_KIT_ROW_TEXT_SUBTITLE : ORB_KIT_ROW_TEXT_VALUE, ORB_TEXT_TABULAR);
        orb_kit_row_set_text_role(v->format_label, v->size == ORB_CONTENT_SIZE_AX3 ?
            ORB_KIT_ROW_TEXT_SUBTITLE : ORB_KIT_ROW_TEXT_VALUE, ORB_TEXT_TABULAR);
    } else build_choices(v);
    if (v->contrast) tv_contrast_apply(v->list);
    orb_kit_list_apply(v->list);
    lv_obj_scroll_to_y(v->list, v->sheet_scroll[v->sheet], LV_ANIM_OFF);
}

void tv_build_reminders(orb_time_view_t *v)
{
    v->listed_signature = tv_reminder_signature(v);
    v->title = tv_rim_title(v, "Reminders");
    if (!v->model.reminder_count) {
        lv_obj_t *bell = tv_text(v->content, ORB_ICON_BELL, 233, 183, 100, ORB_TEXT_TITLE2, 0);
        orb_icon_apply(bell, ORB_TEXT_TITLE2, 0, 0); tv_colour(v, bell, TV_SECONDARY);
        tv_text(v->content, "No reminders", 233, 251, 330, ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED);
        tv_colour(v, tv_text(v->content, "Hold BOOT to add", 233, 310, 330, ORB_TEXT_FOOTNOTE2, 0), TV_SECONDARY);
    } else {
        v->list = tv_sheet_list(v);
        if (v->model.save_error) lv_obj_set_height(v->list, 184);
        for (unsigned i = 0; i < v->model.reminder_count; ++i) {
            orb_reminder_t reminder; if (!orb_time_reminder_get(i, &reminder)) break;
            char line[96]; tv_reminder_when(&reminder, v->model.clock_now, line, sizeof(line), reminder.due);
            /* Review finding 17: a due entry is filled and says "Due". */
            lv_obj_t *obj = row(v, v->list, &(orb_kit_row_config_t){
                .title = reminder.title, .subtitle = line, .accent = TV_ACCENT,
                .glyph = reminder.due ? ORB_ICON_BELL_FILL : ORB_ICON_BELL,
            }, -1, 0, ORB_TEXT_TABULAR);
            lv_obj_set_user_data(obj, (void *)(uintptr_t)reminder.id);
            lv_obj_set_event_bubble(obj, true);
            lv_obj_add_event_cb(obj, reminder_action, LV_EVENT_CLICKED, v);
        }
        if (v->contrast) tv_contrast_apply(v->list);
        orb_kit_list_apply(v->list);
    }
    if (v->model.save_error) {
        v->status = tv_status(v, "Couldn’t save");
        v->hint = tv_capsule(v, v->content, "Retry", false, SAVE_RETRY);
        orb_kit_place_bottom_arc(v->hint);
    }
    if (v->list) lv_obj_scroll_to_y(v->list, v->saved_scroll[v->page], LV_ANIM_OFF);
    tv_build_navigation(v);
}
