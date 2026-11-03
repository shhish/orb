#include "orb_time_view_internal.h"

#include <stdio.h>
#include <string.h>

/* A ringing alarm, a finished timer and a due reminder are one modal
 * pattern (review finding 26): a full teal rim, an inset rim title, the
 * readout or reminder body, a neutral text capsule, then the single tinted
 * capsule at six o'clock (HIG §§2/7). Alerts have no page dots and no page
 * swipe; they end only through their own explicit actions. */
bool tv_alert_state(const orb_time_view_t *v)
{
    if (v->sheet) return false;
    if (v->page == ORB_TIME_PAGE_ALARM) return v->model.alarm_ringing;
    if (v->page == ORB_TIME_PAGE_TIMER) return v->model.timer_state == ORB_TIME_FINISHED;
    return v->page == ORB_TIME_PAGE_REMINDER && v->due_view;
}

/* Only the selected reminder is copied. An eight-item list never grows the
 * view snapshot or creates a second retained data store (HIG §§1/7). */
bool tv_reminder_selected(orb_time_view_t *v, orb_reminder_t *out)
{
    if (v->due_view && v->model.reminder_due) {
        if (v->reminder_id != v->model.active_reminder.id) v->reminder_scroll = 0;
        *out = v->model.active_reminder; v->reminder_id = out->id; return true;
    }
    for (unsigned i = 0; i < v->model.reminder_count; ++i) {
        if (orb_time_reminder_get(i, out) && out->id == v->reminder_id) return true;
    }
    return false;
}

static void ring(orb_time_view_t *v)
{
    v->ring = orb_kit_ring_create(v->content, TIMER_RING_RADIUS, TIMER_RING_WIDTH, TV_ACCENT);
    orb_kit_ring_set_value(v->ring, 100, false);
}

static void actions(orb_time_view_t *v, const char *secondary, int secondary_command,
                    const char *primary, int primary_command)
{
    lv_obj_t *second = tv_capsule(v, v->content, secondary, false, secondary_command);
    lv_obj_align(second, LV_ALIGN_TOP_MID, 0, TV_ALERT_ACTION_TOP);
    lv_obj_t *first = tv_capsule(v, v->content, primary, true, primary_command);
    orb_kit_place_bottom_arc(first);
    if (v->page == ORB_TIME_PAGE_REMINDER) { v->rem_snooze = second; v->rem_done = first; }
    else { v->secondary = second; v->primary = first; }
}

void tv_build_alert(orb_time_view_t *v)
{
    bool alarm = v->page == ORB_TIME_PAGE_ALARM;
    ring(v);
    v->title = tv_rim_title(v, alarm ? "Alarm" : "Timer");
    tv_digits_show(v, v->content, TV_HERO_Y);
    v->detail = tv_text(v->content, "", 233, TV_ALARM_CAPTION_Y, 300, ORB_TEXT_FOOTNOTE2, 0);
    tv_colour(v, v->detail, TV_SECONDARY);
    /* Repeat restarts the same countdown, as Snooze defers the same alarm. */
    actions(v, alarm ? "Snooze" : "Repeat", alarm ? ALARM_SNOOZE : TIMER_REPEAT, "Stop", PRIMARY);
    tv_alert_update(v);
    lv_obj_move_to_index(v->ring, -1);
    lv_obj_move_to_index(v->title, -1);
}

/* Caption, then title; centred while short, scrolling from the top once
 * Dynamic Type or a long title outgrows the body (review finding 4). */
static void reminder_layout(orb_time_view_t *v)
{
    lv_obj_t *items[] = {v->rem_time, v->rem_title};
    int heights[2], total = 0, gaps[] = {10, 0};
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_update_layout(items[i]);
        heights[i] = lv_obj_get_height(items[i]); total += heights[i] + gaps[i];
    }
    int room = TV_BODY_BOTTOM - TV_BODY_TOP, y = total < room ? (room - total) / 2 : 0;
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_set_pos(items[i], 233 - lv_obj_get_width(items[i]) / 2, y);
        y += heights[i] + gaps[i];
    }
    /* The right-rim arc shows when the rim or a finger can reveal more. */
    orb_kit_scroll_arc_refresh(v->list);
}

void tv_build_reminder(orb_time_view_t *v, const orb_reminder_t *reminder)
{
    if (v->alert) ring(v);
    v->title = tv_rim_title(v, "Reminders");
    lv_obj_t *body = tv_plain(v->content);
    lv_obj_set_size(body, 466, TV_BODY_BOTTOM - TV_BODY_TOP); lv_obj_set_pos(body, 0, TV_BODY_TOP);
    lv_obj_set_scrollable(body, true); lv_obj_set_clickable(body, true);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    v->list = body;
    /* Body lines fit the chord at the body's top edge inside the alert ring.
     * The bell leads the caption inline, leaving three title lines. */
    v->rem_time = tv_text(body, "", 233, 0, 280, ORB_TEXT_FOOTNOTE2, ORB_TEXT_TABULAR);
    tv_colour(v, v->rem_time, TV_SECONDARY);
    v->rem_title = tv_text(body, reminder->title[0] ? reminder->title : "Reminder", 233, 0, 310,
        ORB_TEXT_HEADLINE, ORB_TEXT_EMPHASIZED);
    orb_scroll_edge_follow(body, 12, 16); orb_kit_scroll_arc_attach(body);
    actions(v, "In 1 hour", REMINDER_SNOOZE, "Done", REMINDER_DONE);
    tv_alert_update(v);
    reminder_layout(v);
    lv_obj_scroll_to_y(body, v->reminder_scroll, LV_ANIM_OFF);
    if (v->alert) { lv_obj_move_to_index(v->ring, -1); lv_obj_move_to_index(v->title, -1); }
}

/* HIG §§5/8 and review finding 17: the failure is named in words beside a
 * warning glyph and stays until the next explicit action rebuilds Time. */
void tv_reminder_snooze_failed(orb_time_view_t *v)
{
    v->error_shown = true;
    tv_changed(v->rem_time, ORB_ICON_WARNING " Couldn’t snooze\nClock offline");
    tv_colour(v, v->rem_time, ORB_LABEL);
    reminder_layout(v);
}

/* A caption that outgrows its line breaks at its separator, never inside
 * the clock text ("15:" / "05"). */
static void caption_breaks(lv_obj_t *label, char *text)
{
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, 0),
        lv_obj_get_style_text_letter_space(label, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    char *separator = strstr(text, " · ");
    if (size.x <= lv_obj_get_width(label) || !separator) return;
    *separator = '\n';
    memmove(separator + 1, separator + strlen(" · "), strlen(separator + strlen(" · ")) + 1);
}

static void reminder_update(orb_time_view_t *v)
{
    orb_reminder_t reminder;
    if (!tv_reminder_selected(v, &reminder)) {
        v->reminder_detail = v->due_view = false; tv_build(v); return;
    }
    const char *title = reminder.title[0] ? reminder.title : "Reminder";
    char when[96];
    tv_reminder_when(&reminder, v->model.clock_now, when + strlen(ORB_ICON_BELL " "),
        sizeof(when) - strlen(ORB_ICON_BELL " "), v->due_view);
    /* Both bell codepoints are three UTF-8 bytes, so either prefix fits. */
    memcpy(when, v->due_view ? ORB_ICON_BELL_FILL " " : ORB_ICON_BELL " ", strlen(ORB_ICON_BELL " "));
    caption_breaks(v->rem_time, when);
    bool relayout = strcmp(lv_label_get_text(v->rem_title), title) != 0 ||
        (!v->error_shown && strcmp(lv_label_get_text(v->rem_time), when) != 0);
    tv_changed(v->rem_title, title);
    if (!v->error_shown) tv_changed(v->rem_time, when);
    if (relayout) reminder_layout(v);
}

void tv_alert_update(orb_time_view_t *v)
{
    if (v->page == ORB_TIME_PAGE_REMINDER) { reminder_update(v); return; }
    tv_digits_show(v, v->content, TV_HERO_Y);
    char caption[64] = "Timer finished";
    if (v->page == ORB_TIME_PAGE_ALARM) {
        orb_time_format_repeat(caption, sizeof(caption), v->model.alarm_repeat, v->model.alarm_days);
        /* Match the idle Alarm's glanceable schedule summary. Enumerating six
         * or seven days wraps into Snooze at AX3; the editor exposes the
         * complete selected-day set without crowding a ringing action. */
        if (v->model.alarm_repeat == ORB_TIME_REPEAT_CUSTOM) snprintf(caption, sizeof(caption), "Custom days");
    }
    tv_changed(v->detail, caption);
}
