#include "orb_time_view_internal.h"
#include "orb_sound.h"
#include "orb_memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HIG §§3/7: the rim has no toolbar gaps. Zero is 12 o'clock, increasing
 * clockwise. Relative input never changes a value merely by touching it. */
static const orb_rim_config_t s_rim = {.start_angle = 270, .sweep_angle = 360};
static void action(lv_event_t *event);
static orb_time_view_t *create(lv_obj_t *parent, bool preview, const orb_time_commands_t *commands);
static void scroll_chrome(orb_time_view_t *v);
static void peer_delete(orb_time_view_t *v);

typedef struct {
    const char *primary, *secondary, *caption, *secondary_glyph;
    int secondary_command;
    bool tinted;
    char text[128];
} page_state_t;

static bool option_visible(const orb_time_view_t *v)
{
    lv_obj_t *row = v->field == 4 ? v->snooze_label : v->repeat_label;
    if (!row || !v->list) return false;
    lv_area_t bounds, viewport;
    lv_obj_get_coords(row, &bounds); lv_obj_get_coords(v->list, &viewport);
    return bounds.y1 >= viewport.y1 && bounds.y2 <= viewport.y2;
}

lv_obj_t *tv_plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    lv_obj_set_event_bubble(o, true);
    return o;
}

void tv_changed(lv_obj_t *label, const char *value)
{
    if (!label) return;
    orb_kit_value_info_t info;
    if (orb_kit_value_info(label, &info)) orb_kit_value_set(label, value, NULL);
    else if (strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
}

void tv_colour(const orb_time_view_t *v, lv_obj_t *o, lv_color_t ink)
{
    if (!o) return;
    if (v->contrast && lv_color_eq(ink, TV_SECONDARY)) ink = ORB_LABEL;
    if (!lv_color_eq(lv_obj_get_style_text_color(o, 0), ink)) lv_obj_set_style_text_color(o, ink, 0);
    lv_opa_t opacity = lv_color_eq(ink, TV_SECONDARY) ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER;
    if (lv_obj_get_style_text_opa(o, 0) != opacity) lv_obj_set_style_text_opa(o, opacity, 0);
}

/* Every caption and hint goes through orb_text_apply, so Dynamic Type and
 * Bold Text reach it (review finding 38). */
lv_obj_t *tv_text(lv_obj_t *parent, const char *value, int cx, int cy, int width,
                  orb_text_style_t style, orb_text_variant_t variant)
{
    lv_obj_t *label = lv_label_create(parent);
    orb_text_apply(label, style, variant, 0);
    lv_obj_set_style_text_color(label, ORB_LABEL, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, value);
    lv_obj_set_width(label, width);
    lv_obj_update_layout(label);
    lv_obj_set_pos(label, cx - width / 2, cy - lv_obj_get_height(label) / 2);
    lv_obj_set_clickable(label, false);
    return label;
}

bool tv_editable(const orb_time_view_t *v)
{
    return v->sheet == 2 || (v->page == ORB_TIME_PAGE_TIMER && v->model.timer_state == ORB_TIME_IDLE) ||
        (v->page == ORB_TIME_PAGE_ALARM && v->model.clock_valid && !v->model.alarm_ringing);
}

static unsigned steps(const orb_time_view_t *v)
{
    if (v->sheet == 2) return v->field == 4 ? 59 : 4;
    return v->field ? (v->page == ORB_TIME_PAGE_TIMER ? 60 : 59) :
        v->page == ORB_TIME_PAGE_TIMER ? 24 : 23;
}

/* The knob is the edit position; the ring itself reports elapsing time. */
static void position(orb_time_view_t *v, float value, bool audible)
{
    value = fmaxf(0, fminf(1, value));
    bool changed = (int)lroundf(v->position * 1000) != (int)lroundf(value * 1000);
    v->position = value;
    orb_rim_detent(value, steps(v), &v->detent, audible);
    if (changed && v->knob) orb_kit_rim_slider_set_value(v->knob, (int)lroundf(value * 1000), false);
}

/* Only a changed percentage damages the rim band; resting pages stay idle. */
static void ring_value(orb_time_view_t *v, float fraction)
{
    int percent = (int)lroundf(LV_CLAMP(0.f, fraction, 1.f) * 100);
    if (!v->ring || percent == v->ring_percent) return;
    v->ring_percent = percent;
    /* A delayed first poll after Start/Resume includes elapsed seconds. Those
     * are ticks, not a new action to animate from the old full ring. */
    bool immediate_state = v->model.timer_state != v->timer_state_shown &&
        v->model.timer_remaining_us > v->timer_remaining_shown - INT64_C(1000000);
    bool follow = v->page == ORB_TIME_PAGE_TIMER && v->timer_sampled &&
        (immediate_state || v->model.timer_duration_us != v->timer_duration_shown ||
         v->model.timer_remaining_us > v->timer_remaining_shown);
    orb_kit_ring_set_value(v->ring, percent, follow);
}

static void set_value(orb_time_view_t *v, float value)
{
    if (v->sheet == 2) {
        if (v->field == 4) orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_SNOOZE_SET, .a = 60u * (1u + (unsigned)lroundf(LV_CLAMP(0.f, value, 1.f) * 59))});
        else orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_REPEAT, .a = (orb_time_repeat_t)lroundf(LV_CLAMP(0.f, value, 1.f) * 4),
            .b = v->model.alarm_days ? v->model.alarm_days : ORB_TIME_DAYS_WEEKDAYS});
    } else if (v->page == ORB_TIME_PAGE_TIMER) {
        int64_t unit = v->field == 0 ? 3600 : v->field == 1 ? 60 : 1;
        /* HIG §§1/7: keep the common M:SS picker uncluttered. A minute turn
         * carries naturally into hours, with digit taps as its alternative.
         * Use the unwrapped angle so the seam never caps the picker at 59. */
        int64_t seconds = v->drag_unit_base + (int64_t)lroundf(value * steps(v)) * unit;
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_TIMER_SET, .a = (uint32_t)LV_CLAMP(1, seconds, ORB_TIME_TIMER_MAX_SECONDS)});
    } else if (v->page == ORB_TIME_PAGE_ALARM) {
        value = fmaxf(0, fminf(1, value));
        unsigned hour = v->model.alarm_hour, minute = v->model.alarm_minute;
        if (v->field) minute = (unsigned)lroundf(value * 59);
        else hour = (unsigned)lroundf(value * 23);
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_SET, .a = hour, .b = minute});
    }
    orb_time_snapshot(orb_time_now_us(), &v->model);
    int previous_detent = v->detent;
    tv_update(v);
    v->detent = previous_detent;
    position(v, v->position, true);
}

lv_obj_t *tv_hit(lv_obj_t *parent, int x, int y, int w, int h, int command, orb_time_view_t *v)
{
    lv_obj_t *o = tv_plain(parent);
    lv_obj_set_size(o, w, h); lv_obj_set_pos(o, x - w / 2, y - h / 2);
    lv_obj_set_clickable(o, true);
    lv_obj_set_user_data(o, (void *)(intptr_t)command);
    orb_kit_press_apply(o);
    lv_obj_add_event_cb(o, action, LV_EVENT_CLICKED, v);
    return o;
}

static void step_field(orb_time_view_t *v)
{
    if (v->page == ORB_TIME_PAGE_TIMER) {
        int unit = v->field == 0 ? 3600 : v->field == 1 ? 60 : 1;
        int64_t seconds = v->model.timer_duration_us / 1000000 + unit;
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_TIMER_SET, .a = (uint32_t)LV_MIN(seconds, ORB_TIME_TIMER_MAX_SECONDS)});
    } else if (v->page == ORB_TIME_PAGE_ALARM) {
        unsigned hour = v->model.alarm_hour, minute = v->model.alarm_minute;
        if (v->field) minute = (minute + 1) % 60;
        else hour = (hour + 1) % 24;
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_SET, .a = hour, .b = minute});
    }
}

/* Sheet navigation and edits. Returns true when the action is complete. */
static bool sheet_action(orb_time_view_t *v, int a, int64_t now)
{
    if (a == SHEET_CLOSE) {
        v->sheet = v->sheet == 3 ? 2 : 0; v->field = v->sheet ? 3 : v->saved_field[v->page];
        orb_sound_play(ORB_SOUND_CLOSE); tv_build(v); return true;
    }
    if (a == PRESETS) { v->sheet = 1; orb_sound_play(ORB_SOUND_OPEN); tv_build(v); return true; }
    if (a >= PRESET_BASE && a < PRESET_BASE + 6) {
        static const uint32_t presets[] = {0, 60, 180, 300, 600, 1500};
        uint32_t seconds = a == PRESET_BASE ? v->model.timer_last_seconds : presets[a - PRESET_BASE];
        if (orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_TIMER_SET, .a = seconds})) { v->sheet = 0; tv_build(v); }
        return true;
    }
    if (a == REPEAT_EDIT || a == SNOOZE_EDIT) { v->field = a == REPEAT_EDIT ? 3 : 4; tv_update(v); return true; }
    if (a == CUSTOM_EDIT) {
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_REPEAT, .a = ORB_TIME_REPEAT_CUSTOM, .b = v->model.alarm_days ? v->model.alarm_days : 0x3e});
        orb_time_snapshot(now, &v->model); v->sheet = 3; tv_build(v); return true;
    }
    if (a >= DAY_BASE && a < DAY_BASE + 7) {
        uint8_t days = v->model.alarm_days ^ (1u << (a - DAY_BASE));
        if (days) orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_REPEAT, .a = ORB_TIME_REPEAT_CUSTOM, .b = days});
        int scroll = v->list ? lv_obj_get_scroll_y(v->list) : 0;
        orb_time_snapshot(now, &v->model); tv_build(v);
        if (v->list) lv_obj_scroll_to_y(v->list, scroll, LV_ANIM_OFF);
        return true;
    }
    if (a == CLOCK_FORMAT) {
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_CLOCK_FORMAT, .a = !v->model.hour24});
        orb_time_snapshot(now, &v->model); tv_update(v); return true;
    }
    return false;
}

/* Alert and reminder actions. The shell destroys an alert layer on its next
 * poll, so these never build an ordinary page just before that teardown. */
static bool alert_action(orb_time_view_t *v, int a, int64_t now)
{
    if (a == ALARM_SNOOZE) {
        (void)orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_SNOOZE, .a = v->model.alarm_snooze_seconds});
        return true;
    }
    if (v->ringing_view && a == PRIMARY) {
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_DISMISS, .b = true});
        return true;
    }
    if (a == TIMER_REPEAT) {
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_DISMISS, .a = true});
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_TIMER_START, .at = now});
        return false;
    }
    if (a != REMINDER_SNOOZE && a != REMINDER_DONE) return false;
    if (a == REMINDER_SNOOZE && !orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_REMINDER_SNOOZE, .a = v->reminder_id, .b = 3600})) {
        tv_reminder_snooze_failed(v);
        return true;
    }
    if (a == REMINDER_DONE && orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_REMINDER_DONE, .a = v->reminder_id}))
        orb_sound_play(ORB_SOUND_DONE);
    v->reminder_detail = false; v->due_view = false;
    orb_time_snapshot(now, &v->model);
    if (!v->model.reminder_due && !v->model.alarm_ringing && v->model.timer_state == ORB_TIME_FINISHED) {
        v->page = ORB_TIME_PAGE_TIMER; v->field = v->saved_field[v->page];
    }
    tv_build(v);
    return true;
}

static bool navigation_action(orb_time_view_t *v, int a, lv_event_t *event)
{
    if (a >= PAGE_TIMER && a <= PAGE_ALARM) { orb_time_view_select(v, (orb_time_page_t)(a - PAGE_TIMER)); return true; }
    if (a == PAGE_NAV) {
        /* Four kit dots share one segmented target: 52px per destination. */
        lv_area_t area; lv_obj_get_coords(lv_event_get_target_obj(event), &area);
        lv_indev_t *input = lv_indev_active();
        lv_point_t p; if (!input) return true;
        lv_indev_get_point(input, &p);
        int column = (p.x - area.x1) * 4 / LV_MAX(1, lv_area_get_width(&area));
        orb_time_view_select(v, (orb_time_page_t)LV_CLAMP(0, column, 3));
        return true;
    }
    if (a == LAPS_MORE) {
        lv_obj_scroll_to_y(v->list, TV_LAP_ANCHOR, v->reduced ? LV_ANIM_OFF : LV_ANIM_ON);
        return true;
    }
    if (a == FINISH && v->page == ORB_TIME_PAGE_ALARM && !v->model.alarm_ringing) {
        v->sheet = 2; v->field = 3; orb_sound_play(ORB_SOUND_OPEN); tv_build(v); return true;
    }
    if (a == FINISH) {
        orb_time_view_select(v, v->model.timer_state == ORB_TIME_FINISHED &&
            v->page != ORB_TIME_PAGE_TIMER ? ORB_TIME_PAGE_TIMER : ORB_TIME_PAGE_ALARM);
        return true;
    }
    return false;
}

static void page_action(orb_time_view_t *v, int a, int64_t now)
{
    if (a == ALARM_TOGGLE) {
        if (v->model.clock_valid) {
            bool enabled = !v->model.alarm_enabled;
            orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_ENABLE, .a = enabled, .at = v->model.clock_now});
            orb_sound_play(enabled ? ORB_SOUND_HOME_ON : ORB_SOUND_HOME_OFF);
        }
    } else if (a >= FIELD_HOUR && a <= FIELD_SECOND && tv_editable(v)) {
        /* The first tap targets a group (the frame appears); tapping the
         * targeted group again adds one, as before. */
        unsigned field = (unsigned)(a - FIELD_HOUR);
        if (v->editing && v->field == field) step_field(v);
        else v->field = field;
        v->editing = true;
    } else if (a == PRIMARY) {
        if (v->page == ORB_TIME_PAGE_TIMER) {
            if (v->model.timer_state == ORB_TIME_FINISHED) orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_DISMISS, .a = true});
            else if (v->model.timer_state == ORB_TIME_RUNNING) orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_TIMER_PAUSE, .at = now});
            else orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_TIMER_START, .at = now});
        } else if (v->page == ORB_TIME_PAGE_STOPWATCH) {
            if (v->model.stopwatch_state == ORB_TIME_RUNNING) orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_STOPWATCH_PAUSE, .at = now});
            else orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_STOPWATCH_START, .at = now});
        } else if (v->model.alarm_ringing) orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_DISMISS, .b = true});
        else if (v->model.clock_valid) orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_ENABLE, .a = !v->model.alarm_enabled, .at = v->model.clock_now});
    } else if (a == SECONDARY_ACTION) {
        if (v->page == ORB_TIME_PAGE_TIMER) orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_TIMER_RESET});
        else if (v->model.stopwatch_state == ORB_TIME_RUNNING) orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_STOPWATCH_LAP, .at = now});
        else orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_STOPWATCH_RESET});
    }
}

static void action(lv_event_t *event)
{
    orb_time_view_t *v = lv_event_get_user_data(event);
    if (v->paging || v->drag.active || v->preview) return;
    tv_remember(v);
    int a = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    int64_t now = orb_time_now_us();
    orb_time_snapshot(now, &v->model);
    /* Editing ends with any action other than targeting a digit group. */
    if (a < FIELD_HOUR || a > FIELD_SECOND) v->editing = false;
    if (a == PRIMARY || a == SECONDARY_ACTION || a == ALARM_SNOOZE || a == REMINDER_SNOOZE || a == TIMER_REPEAT)
        orb_sound_play(ORB_SOUND_TAP);
    if (a == SAVE_RETRY) { orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_PERSIST_RETRY}); return; }
    if (sheet_action(v, a, now) || alert_action(v, a, now) || navigation_action(v, a, event)) return;
    page_action(v, a, now);
    orb_time_snapshot(now, &v->model);
    tv_update(v);
}

void tv_bind(orb_time_view_t *v, lv_obj_t *obj, int command)
{
    lv_obj_set_user_data(obj, (void *)(intptr_t)command);
    lv_obj_set_event_bubble(obj, true);
    lv_obj_add_event_cb(obj, action, LV_EVENT_CLICKED, v);
}

/* Semantic names are not painted; simulator input resolves the same target
 * as a finger. The kit owns all geometry/material/press behavior. */
void tv_semantic(lv_obj_t *obj, const char *name)
{
    lv_obj_t *label = tv_text(obj, name, 0, 0, 1, ORB_TEXT_FOOTNOTE2, 0);
    lv_obj_set_style_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(label, 0, 0);
    lv_obj_set_size(label, 1, 1);
}

lv_obj_t *tv_rim_title(orb_time_view_t *v, const char *name)
{
    /* HIG §§3/7: titles stay inside the gauge instead of crossing the top
     * stroke. The shared inset survives font and content changes. */
    lv_obj_t *title = v->ring ? orb_kit_rim_title_inset_create(v->content, name, TV_ACCENT) :
        orb_kit_rim_title_create(v->content, name, TV_ACCENT);
    tv_semantic(title, name);
    return title;
}

lv_obj_t *tv_capsule(orb_time_view_t *v, lv_obj_t *parent, const char *name, bool primary, int command)
{
    lv_obj_t *obj = primary ? orb_kit_button_primary_create(parent, name, TV_ACCENT) :
        orb_kit_button_secondary_create(parent, name);
    tv_bind(v, obj, command);
    if (v->contrast) tv_contrast_apply(obj);
    return obj;
}

/* HIG §§5/8, review findings 14/15: one failure line under the rim title,
 * a warning glyph plus words, on every Time page that persists. */
lv_obj_t *tv_status(orb_time_view_t *v, const char *text)
{
    lv_obj_t *label = tv_text(v->content, "", 233, TV_STATUS_Y, 260, ORB_TEXT_FOOTNOTE2, 0);
    char line[64];
    snprintf(line, sizeof(line), ORB_ICON_WARNING " %s", text);
    lv_label_set_text(label, line);
    lv_obj_update_layout(label);
    lv_obj_set_y(label, TV_STATUS_Y - lv_obj_get_height(label) / 2);
    return label;
}

static void control_amount(lv_obj_t *wrapper, float amount)
{
    if (!wrapper) return;
    bool hidden = amount <= 0;
    if (lv_obj_is_hidden(wrapper) != hidden) lv_obj_set_hidden(wrapper, hidden);
    if (hidden || fabsf(orb_glass_get_amount(wrapper) - amount) <= .001f) return;
    /* A live capsule label restyles on every set, so only a change of the
     * material amount touches its ink; resting polls stay idle. */
    orb_glass_set_amount(wrapper, amount);
    orb_kit_disc_set_ink_opa(wrapper, (lv_opa_t)lroundf(255 * amount));
}

/* Review finding 23: the counter hands over to a compact header of the same
 * precision once it leaves; glass fades out before the viewport would clip
 * it, and the Laps anchor lands the first row directly under the header. */
static void scroll_chrome(orb_time_view_t *v)
{
    if (!v->list || !v->elapsed) return;
    int scroll = lv_obj_get_scroll_y(v->list);
    /* The counter is ordinary content, excluded from the row compositor.
     * Floating keeps its native children intact; its translation matches the
     * same viewport scroll and the viewport still clips it. */
    if (v->summary && lv_obj_get_y(v->summary) != -scroll) lv_obj_set_y(v->summary, -scroll);
    float amount = LV_CLAMP(0.f, (TV_LAP_CONTROLS - scroll) / 64.f, 1.f);
    control_amount(v->primary, amount);
    control_amount(v->secondary, amount);
    if (v->laps_link && v->model.lap_count)
        control_amount(v->laps_link, LV_CLAMP(0.f, (TV_LAP_PIN - 28 - scroll) / 64.f, 1.f));
    bool pinned = scroll >= TV_LAP_PIN;
    if (lv_obj_is_hidden(v->elapsed) == pinned) lv_obj_set_hidden(v->elapsed, !pinned);
    bool hidden = scroll > 32;
    if (hidden != v->ring_hidden) {
        v->ring_hidden = hidden;
        if (v->ring) lv_obj_set_hidden(v->ring, hidden);
    }
    orb_scroll_edge_follow(v->list, 32, hidden ? 24 : 0);
}

static void lap_scroll(lv_event_t *event)
{
    orb_time_view_t *v = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) != v->list) return;
    scroll_chrome(v);
}

/* The newest split labels the Laps capsule; only a new lap touches it. */
static void laps_label(orb_time_view_t *v)
{
    if (!v->laps_link) return;
    lv_obj_set_hidden(v->laps_link, v->model.lap_count == 0);
    if (!v->model.lap_count) return;
    char split[32], label[48], reserve[48];
    tv_stopwatch_text(split, sizeof(split), v->model.laps_us[v->model.lap_count - 1]);
    snprintf(label, sizeof(label), "Lap %u · %s", v->model.lap_total, split);
    /* Reserve this label's own digit shape: the capsule keeps its width
     * between laps and widens only when a split gains a digit. */
    snprintf(reserve, sizeof(reserve), "%s", label);
    for (char *c = reserve; *c; ++c) if (*c >= '0' && *c <= '9') *c = '8';
    orb_kit_button_set_value(v->laps_link, label, reserve);
}

static void laps(orb_time_view_t *v)
{
    if (v->lap_total == v->model.lap_total) return;
    v->lap_total = v->model.lap_total;
    laps_label(v);
    for (unsigned i = lv_obj_get_child_count(v->list); i > 0; --i) {
        lv_obj_t *child = lv_obj_get_child(v->list, (int)i - 1);
        if (child != v->summary) lv_obj_delete(child);
    }
    unsigned count = v->model.lap_count, fastest = 0, slowest = 0;
    for (unsigned i = 1; i < count; ++i) {
        if (v->model.laps_us[i] < v->model.laps_us[fastest]) fastest = i;
        if (v->model.laps_us[i] > v->model.laps_us[slowest]) slowest = i;
    }
    for (unsigned i = 0; i < count; ++i) {
        unsigned index = count - i - 1;
        char name[24], split[40];
        snprintf(name, sizeof(name), "Lap %u", v->model.lap_total - i);
        tv_stopwatch_text(split, sizeof(split), v->model.laps_us[index]);
        const char *glyph = ORB_ICON_FLAG;
        if (count > 1 && fastest != slowest && index == fastest) glyph = ORB_ICON_ARROW_UP;
        else if (count > 1 && fastest != slowest && index == slowest) glyph = ORB_ICON_ARROW_DOWN;
        lv_obj_t *row = orb_kit_row_create(v->list, &(orb_kit_row_config_t){
            .title = name, .subtitle = split, .glyph = glyph,
            .accent = count > 1 && fastest != slowest && index == slowest ? ORB_RED : TV_ACCENT,
        });
        /* Only real lap rows enter the kit's viewport cache. A full-height
         * nested container would incorrectly retain every offscreen lap. */
        lv_obj_set_y(row, TV_LAP_ROWS + (int)i * 84);
        orb_kit_row_set_text_role(row, ORB_KIT_ROW_TEXT_TITLE, ORB_TEXT_TABULAR);
        orb_kit_row_set_text_role(row, ORB_KIT_ROW_TEXT_SUBTITLE, ORB_TEXT_TABULAR);
        lv_obj_set_event_bubble(row, true);
    }
    /* A short list can still scroll its first row up under the header. */
    int rows_bottom = TV_LAP_ROWS + (int)count * 84 - 12;
    lv_obj_set_style_pad_bottom(v->list, count ? LV_MAX(0, TV_LAP_ANCHOR + (LAP_VIEW_BOTTOM - LAP_VIEW_TOP) - rows_bottom) : 0, 0);
    if (v->contrast) tv_contrast_apply(v->list);
    orb_kit_list_apply(v->list);
}

void tv_remember(orb_time_view_t *v)
{
    if (v->preview) return;
    if (v->sheet) {
        if (v->list) v->sheet_scroll[v->sheet] = lv_obj_get_scroll_y(v->list);
    } else if (v->reminder_detail || v->due_view) {
        if (v->list) v->reminder_scroll = lv_obj_get_scroll_y(v->list);
    } else {
        if (v->list) v->saved_scroll[v->page] = lv_obj_get_scroll_y(v->list);
        v->saved_field[v->page] = v->field;
    }
}

static void timer_state(orb_time_view_t *v, page_state_t *s)
{
    orb_time_state_t state = v->model.timer_state;
    s->primary = state == ORB_TIME_RUNNING ? "Pause" : state == ORB_TIME_PAUSED ? "Resume" : "Start";
    bool active = state == ORB_TIME_RUNNING || state == ORB_TIME_PAUSED;
    /* Review finding 20: Quick timers opens a list of presets. */
    s->secondary = active ? "Cancel" : "Quick timers";
    s->secondary_glyph = active ? ORB_ICON_X : ORB_ICON_LIST_BULLET;
    s->secondary_command = active ? SECONDARY_ACTION : PRESETS;
    if (state == ORB_TIME_RUNNING) {
        char total[32];
        orb_time_format_duration(total, sizeof(total), v->model.timer_duration_us, false);
        snprintf(s->text, sizeof(s->text), "of %s", total);
        s->caption = s->text;
    } else if (state == ORB_TIME_PAUSED) s->caption = "Paused";
    unsigned seconds = (unsigned)(v->model.timer_duration_us / 1000000);
    unsigned selected = v->field == 0 ? seconds / 3600 : v->field == 1 ? seconds / 60 % 60 : seconds % 60;
    float remaining = v->model.timer_duration_us ? (float)v->model.timer_remaining_us / v->model.timer_duration_us : 0;
    position(v, state == ORB_TIME_IDLE ? (float)selected / steps(v) : remaining, false);
    /* Review finding 27: the ring is the time left. A set timer is full; the
     * hour picker no longer paints a stray 1/24 arc at twelve o'clock. */
    ring_value(v, state == ORB_TIME_IDLE ? 1.f : remaining);
    v->timer_sampled = true;
    v->timer_state_shown = state;
    v->timer_duration_shown = v->model.timer_duration_us;
    v->timer_remaining_shown = v->model.timer_remaining_us;
}

static void stopwatch_state(orb_time_view_t *v, page_state_t *s)
{
    orb_time_state_t state = v->model.stopwatch_state;
    s->primary = state == ORB_TIME_RUNNING ? "Pause" : state == ORB_TIME_PAUSED ? "Resume" : "Start";
    if (state == ORB_TIME_RUNNING) { s->secondary = "Lap"; s->secondary_glyph = ORB_ICON_FLAG; }
    else if (v->model.stopwatch_elapsed_us > 0) { s->secondary = "Reset"; s->secondary_glyph = ORB_ICON_ARROW_COUNTER_CLOCKWISE; }
    s->secondary_command = SECONDARY_ACTION;
    if (state == ORB_TIME_PAUSED && !v->model.lap_count) s->caption = "Paused";
    /* One rim for the family (review findings 2/26): the Timer's track ring
     * sweeps each elapsed minute, replacing the tick marker that crossed the
     * title. A paused stopwatch's ring stops with the service snapshot. */
    ring_value(v, (float)(v->model.stopwatch_elapsed_us / 1000 % 60000) / 60000.f);
    laps(v);
}

static void alarm_state(orb_time_view_t *v, page_state_t *s)
{
    char repeat[64];
    orb_time_format_repeat(repeat, sizeof(repeat), v->model.alarm_repeat, v->model.alarm_days);
    if (v->model.alarm_repeat == ORB_TIME_REPEAT_CUSTOM) snprintf(repeat, sizeof(repeat), "Custom days");
    s->caption = s->text;
    if (!v->model.clock_valid) s->caption = "Waiting for time";
    else if (v->size == ORB_CONTENT_SIZE_AX3) snprintf(s->text, sizeof(s->text), "%s · %s", repeat, v->model.alarm_enabled ? "On" : "Off");
    else if (v->model.alarm_enabled) {
        int64_t minutes = ((int64_t)v->model.alarm_next - v->model.clock_now + 59) / 60;
        if (minutes >= 60) snprintf(s->text, sizeof(s->text), "%s · in %lld h %lld min", repeat,
            (long long)(minutes / 60), (long long)(minutes % 60));
        else if (minutes > 0) snprintf(s->text, sizeof(s->text), "%s · in %lld min", repeat, (long long)minutes);
        else snprintf(s->text, sizeof(s->text), "%s · Enabled", repeat);
    } else snprintf(s->text, sizeof(s->text), "%s · Off", repeat);
    position(v, v->field ? (float)v->model.alarm_minute / 59 : (float)v->model.alarm_hour / 23, false);
    /* A poll is not a new toggle. Restarting the kit spring every 250 ms
     * keeps a settled, unchanged Alarm invalidating forever. */
    if (v->switch_btn && orb_kit_switch_get_checked(v->switch_btn) != v->model.alarm_enabled)
        orb_kit_switch_set_checked(v->switch_btn, v->model.alarm_enabled, !v->reduced);
    if (v->sw_sem) tv_changed(v->sw_sem, v->model.alarm_enabled ? "On" : "Set");
}

/* Review findings 14/15: a failed save shows its line and Retry in place;
 * Alarm keeps Options beside Retry, Timer gives Retry the caption's slot. */
static void error_state(orb_time_view_t *v)
{
    bool failed = v->model.save_error && v->hint;
    if (!v->hint || lv_obj_is_hidden(v->hint) != failed) return;
    lv_obj_set_hidden(v->hint, !failed);
    if (v->status) lv_obj_set_hidden(v->status, !failed);
    if (v->page == ORB_TIME_PAGE_ALARM && v->secondary) {
        lv_obj_update_layout(v->content);
        int options = lv_obj_get_width(v->secondary), retry = lv_obj_get_width(v->hint);
        /* Two text capsules in one row (HIG §3: at most two), centred as a
         * pair; offsets are from the top-centre alignment point. */
        int left = -(options + (failed ? 20 + retry : 0)) / 2;
        lv_obj_align(v->secondary, LV_ALIGN_TOP_MID, left + options / 2, TV_ALARM_ACTION_TOP);
        lv_obj_align(v->hint, LV_ALIGN_TOP_MID, left + options + 20 + retry / 2, TV_ALARM_ACTION_TOP);
    }
}

static void page_update(orb_time_view_t *v)
{
    page_state_t s = {.caption = "", .tinted = true};
    if (v->page == ORB_TIME_PAGE_TIMER) timer_state(v, &s);
    else if (v->page == ORB_TIME_PAGE_STOPWATCH) stopwatch_state(v, &s);
    else alarm_state(v, &s);
    if (v->model.timer_state == ORB_TIME_FINISHED && v->page != ORB_TIME_PAGE_TIMER) s.caption = "Timer finished";
    else if (v->model.alarm_ringing && v->page != ORB_TIME_PAGE_ALARM) s.caption = "Alarm ringing";
    tv_changed(v->detail, s.caption);
    if (v->page == ORB_TIME_PAGE_ALARM && !v->model.clock_valid && !v->waiting_started)
        v->waiting_started = lv_tick_get();
    orb_kit_breathe_since(v->detail, v->page == ORB_TIME_PAGE_ALARM && !v->model.clock_valid, v->waiting_started);
    if (v->model.clock_valid) v->waiting_started = 0;
    lv_obj_update_layout(v->detail);
    lv_obj_set_y(v->detail, (v->summary ? TV_CAPTION_Y - LAP_VIEW_TOP : v->page == ORB_TIME_PAGE_ALARM ?
        TV_ALARM_CAPTION_Y : TV_CAPTION_Y) - lv_obj_get_height(v->detail) / 2);
    tv_digits_show(v, v->summary ? v->summary : v->content, v->summary ? TV_STOPWATCH_HERO_Y - LAP_VIEW_TOP : TV_HERO_Y);
    if (v->laps_link) lv_obj_move_to_index(v->laps_link, -1);
    error_state(v);
    /* The Timer's Retry takes the caption's slot while the failure lasts. */
    if (v->page == ORB_TIME_PAGE_TIMER && lv_obj_is_hidden(v->detail) != (v->hint && !lv_obj_is_hidden(v->hint)))
        lv_obj_set_hidden(v->detail, v->hint && !lv_obj_is_hidden(v->hint));
    tv_controls(v, s.primary, s.secondary, s.secondary_glyph, s.secondary_command);
    if (v->elapsed) {
        char elapsed[40];
        tv_stopwatch_text(elapsed, sizeof(elapsed), v->model.stopwatch_elapsed_us);
        tv_changed(v->elapsed, elapsed);
        scroll_chrome(v);
    }
}

void tv_update(orb_time_view_t *v)
{
    if (v->sheet) {
        tv_sheet_update(v);
        if (v->sheet == 2) position(v, v->field == 4 ? (v->model.alarm_snooze_seconds / 60.f - 1) / 59.f :
            v->model.alarm_repeat / 4.f, false);
        return;
    }
    if (v->alert != tv_alert_state(v)) { tv_build(v); return; }
    if (v->alert) { tv_alert_update(v); return; }
    if (v->page == ORB_TIME_PAGE_REMINDER) {
        if (!v->reminder_detail) {
            if (v->listed_signature != tv_reminder_signature(v)) {
                int scroll = v->list ? lv_obj_get_scroll_y(v->list) : 0;
                tv_build(v);
                if (v->list) lv_obj_scroll_to_y(v->list, scroll, LV_ANIM_OFF);
            }
            return;
        }
        tv_alert_update(v);
        return;
    }
    page_update(v);
}

void tv_build_navigation(orb_time_view_t *v)
{
    v->navigation = orb_kit_page_dots_create(v->content, 4);
    orb_kit_page_dots_set_active(v->navigation, (unsigned)v->page, TV_ACCENT);
    /* The kit paints position; a single nonpainted segmented target supplies
     * the touch alternative without hand-built dot geometry. */
    tv_hit(v->navigation, 233, 419, 208, 46, PAGE_NAV, v);
}

lv_obj_t *tv_sheet_list(orb_time_view_t *v)
{
    lv_obj_t *list = tv_plain(v->content);
    lv_obj_set_size(list, 466, 252); lv_obj_set_pos(list, 0, 140);
    lv_obj_set_scrollable(list, true); lv_obj_set_clickable(list, true);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    orb_scroll_edge_follow(list, 24, 24);
    orb_kit_scroll_arc_attach(list);
    return list;
}

static void build_stopwatch(orb_time_view_t *v)
{
    v->elapsed = tv_text(v->content, "00:00.00", 233, TV_STATUS_Y, 300, ORB_TEXT_HEADLINE,
        ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR);
    orb_kit_value_set(v->elapsed, "00:00.00", "88:88:88.88");
    lv_obj_set_hidden(v->elapsed, true);
    v->list = tv_sheet_list(v);
    lv_obj_set_height(v->list, LAP_VIEW_BOTTOM - LAP_VIEW_TOP);
    lv_obj_add_event_cb(v->list, lap_scroll, LV_EVENT_SCROLL, v);
    v->summary = tv_plain(v->list); lv_obj_set_size(v->summary, 466, LAP_VIEW_BOTTOM - LAP_VIEW_TOP);
    lv_obj_set_floating(v->summary, true); lv_obj_set_pos(v->summary, 0, 0);
    /* Review finding 21: the newest split is a centred capsule in the
     * caption's slot, 26 px clear of the controls, that opens the laps. */
    v->laps_link = tv_capsule(v, v->summary, "Laps", false, LAPS_MORE);
    tv_semantic(v->laps_link, "Laps");
    lv_obj_update_layout(v->laps_link);
    lv_obj_align(v->laps_link, LV_ALIGN_TOP_MID, 0, TV_SLOT_Y - LAP_VIEW_TOP - lv_obj_get_height(v->laps_link) / 2);
    lv_obj_set_hidden(v->laps_link, true);
}

static void build_alarm(orb_time_view_t *v)
{
    v->secondary = tv_capsule(v, v->content, "Options", false, FINISH);
    lv_obj_align(v->secondary, LV_ALIGN_TOP_MID, 0, TV_ALARM_ACTION_TOP);
    v->switch_btn = orb_kit_switch_create(v->content, v->model.alarm_enabled, TV_ACCENT);
    lv_obj_align(v->switch_btn, LV_ALIGN_TOP_MID, 0, 356);
    tv_bind(v, v->switch_btn, ALARM_TOGGLE);
    v->sw_sem = tv_text(v->switch_btn, "Set", 40, 23, 1, ORB_TEXT_FOOTNOTE2, 0);
    lv_obj_set_style_opa(v->sw_sem, 0, 0); lv_obj_set_size(v->sw_sem, 1, 1);
}

static void build_page(orb_time_view_t *v)
{
    static const char *const names[] = {"Timer", "Stopwatch", "Alarm"};
    if (v->page != ORB_TIME_PAGE_ALARM) {
        v->ring = orb_kit_ring_create(v->content, TIMER_RING_RADIUS, TIMER_RING_WIDTH, TV_ACCENT);
        v->ring_percent = -1;
        v->timer_sampled = false;
    }
    if (v->page == ORB_TIME_PAGE_TIMER) {
        /* Input and display are separate kit objects; the shell remains the
         * sole crown recognizer, so an invisible slider never owns a touch. */
        v->knob = orb_kit_rim_slider_create(v->content, TIMER_RING_RADIUS, TIMER_RING_WIDTH,
            TV_ACCENT, 0, 1000, 0, NULL, NULL);
        orb_kit_rim_slider_set_angles(v->knob, 270, 360);
        lv_obj_set_clickable(v->knob, false); lv_obj_set_hidden(v->knob, true);
    }
    v->title = tv_rim_title(v, names[v->page]);
    if (v->page == ORB_TIME_PAGE_STOPWATCH) build_stopwatch(v);
    lv_obj_t *parent = v->summary ? v->summary : v->content;
    tv_digits_show(v, parent, v->summary ? TV_STOPWATCH_HERO_Y - LAP_VIEW_TOP : TV_HERO_Y);
    if (v->laps_link) lv_obj_move_to_index(v->laps_link, -1);
    v->detail = tv_text(parent, "", 233, TV_CAPTION_Y, 330, ORB_TEXT_FOOTNOTE2, 0);
    if (v->page == ORB_TIME_PAGE_ALARM)
        orb_kit_value_set(v->detail, "", v->size == ORB_CONTENT_SIZE_AX3 ? "Custom days · Off" :
            "Custom days · in 188 h 88 min");
    tv_colour(v, v->detail, TV_SECONDARY);
    if (v->page == ORB_TIME_PAGE_ALARM) build_alarm(v);
    if (v->page != ORB_TIME_PAGE_STOPWATCH) {
        v->status = tv_status(v, "Couldn’t save");
        v->hint = tv_capsule(v, v->content, "Retry", false, SAVE_RETRY);
        lv_obj_update_layout(v->hint);
        lv_obj_align(v->hint, LV_ALIGN_TOP_MID, 0, v->page == ORB_TIME_PAGE_ALARM ? TV_ALARM_ACTION_TOP :
            TV_SLOT_Y - lv_obj_get_height(v->hint) / 2);
        lv_obj_set_hidden(v->status, true); lv_obj_set_hidden(v->hint, true);
    }
    tv_build_navigation(v);
    v->position = 0; v->detent = -1;
    tv_update(v);
    if (v->page == ORB_TIME_PAGE_TIMER && v->ring) {
        orb_kit_ring_set_presenter(v->ring, tv_digits_project, v);
        if (v->arrival) orb_kit_ring_set_arrival(v->ring, v->ring_percent);
    }
    if (v->list) lv_obj_scroll_to_y(v->list, v->saved_scroll[v->page], LV_ANIM_OFF);
    if (v->ring) lv_obj_move_to_index(v->ring, -1);
    lv_obj_move_to_index(v->title, -1);
    lv_obj_move_to_index(v->navigation, -1);
}

static void build_content(orb_time_view_t *v)
{
    /* A rebuild preempts any page gesture. An old preview/settle must never
     * outlive an alert or accessibility rebuild and cover the new content. */
    if (v->settle) lv_timer_pause(v->settle);
    peer_delete(v); v->paging = v->pressed = v->system_rotary = false;
    v->offset = v->velocity = v->drag_base = 0;
    if (v->content) lv_obj_delete(v->content);
    v->content = tv_plain(v->root); lv_obj_set_size(v->content, 466, 466);
    v->primary = v->secondary = v->list = v->summary = v->laps_link = v->elapsed = v->digits = NULL;
    v->suffix = v->ampm = v->switch_btn = v->hint = v->sw_sem = v->status = v->detail = NULL;
    v->rem_time = v->rem_title = v->rem_snooze = v->rem_done = NULL;
    v->repeat_label = v->snooze_label = v->format_label = NULL;
    v->navigation = v->knob = v->ring = NULL;
    v->primary_name[0] = v->secondary_name[0] = 0;
    v->ringing_view = v->page == ORB_TIME_PAGE_ALARM && v->model.alarm_ringing;
    v->alert = tv_alert_state(v);
    v->lap_total = UINT32_MAX; v->ring_hidden = false; v->drag.active = false;
    v->editing = v->error_shown = false;
    v->size = orb_fonts_content_size(); v->bold = orb_fonts_bold_text();
    v->opaque = orb_ui_reduce_transparency(); v->reduced = orb_ui_reduce_motion();
    v->canvas = tv_plain(v->content); lv_obj_set_size(v->canvas, 466, 466);

    if (v->sheet) { tv_build_sheet(v); tv_update(v); return; }
    if (v->page == ORB_TIME_PAGE_REMINDER && !v->reminder_detail && !v->due_view) { tv_build_reminders(v); return; }
    if (v->page == ORB_TIME_PAGE_REMINDER) {
        orb_reminder_t selected;
        if (!tv_reminder_selected(v, &selected)) {
            v->reminder_detail = v->due_view = v->alert = false; tv_build_reminders(v); return;
        }
        tv_build_reminder(v, &selected);
        return;
    }
    if (v->alert) { tv_build_alert(v); return; }
    build_page(v);
}

void tv_contrast_apply(lv_obj_t *obj)
{
    /* HIG §6: stronger secondary ink and the shared white 50% control rim.
     * Preserve flat app identity fills and the kit's original geometry. */
    if (lv_obj_get_style_text_opa(obj, 0) != LV_OPA_TRANSP &&
        lv_color_eq(lv_obj_get_style_text_color(obj, 0), TV_SECONDARY)) {
        lv_obj_set_style_text_color(obj, ORB_LABEL, 0);
        lv_obj_set_style_text_opa(obj, LV_OPA_COVER, 0);
    }
    if (orb_glass_get_amount(obj) > 0) {
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, ORB_LABEL, 0);
        lv_obj_set_style_border_opa(obj, LV_OPA_50, 0);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i)
        tv_contrast_apply(lv_obj_get_child(obj, i));
}

void tv_build(orb_time_view_t *v)
{
    build_content(v);
    if (v->contrast) tv_contrast_apply(v->content);
}

static void peer_delete(orb_time_view_t *v)
{
    if (v->peer) orb_time_view_destroy(v->peer);
    v->peer = NULL;
}

static void page_offset(orb_time_view_t *v, float offset)
{
    v->offset = offset;
    if (v->reduced) {
        lv_obj_set_style_opa(v->content, (int)(255 * (1 - LV_MIN(1.f, fabsf(offset) / 494))), 0);
        if (v->peer) lv_obj_set_style_opa(v->peer->root, (int)(255 * LV_MIN(1.f, fabsf(offset) / 494)), 0);
    } else {
        lv_obj_set_x(v->content, (int)lroundf(offset));
        if (v->peer) lv_obj_set_x(v->peer->root, (int)lroundf(offset) + v->direction * 494);
    }
}

static void settle_tick(lv_timer_t *timer)
{
    orb_time_view_t *v = lv_timer_get_user_data(timer);
    uint32_t now = lv_tick_get();
    float remaining = LV_MIN(.064f, (now - v->last_tick) / 1000.f);
    if (!v->paging) { lv_timer_pause(timer); return; }
    if (v->reduced) {
        float progress = LV_MIN(1.f, (now - v->started) / 150.f);
        v->offset = v->settle_from + (v->target - v->settle_from) * progress;
        remaining = 0;
    }
    while (remaining > 0) {
        float dt = fminf(remaining, 1.f / 240);
        float omega = 2 * (float)M_PI / .35f;
        v->velocity += (omega * omega * (v->target - v->offset) - 2 * omega * v->velocity) * dt;
        v->offset += v->velocity * dt; remaining -= dt;
    }
    v->last_tick = now;
    page_offset(v, v->offset);
    if ((v->reduced && now - v->started >= 150) ||
        (fabsf(v->target - v->offset) < .5f && fabsf(v->velocity) < 5)) {
        bool commit = v->target != 0;
        orb_time_page_t page = v->peer ? v->peer->page : v->page;
        lv_timer_pause(timer);
        peer_delete(v); v->paging = false;
        v->offset = v->velocity = 0;
        if (commit) { v->page = page; v->field = v->saved_field[page]; tv_build(v); }
        else { lv_obj_set_x(v->content, 0); lv_obj_set_style_opa(v->content, 255, 0); }
    }
}

static void rim_begin(orb_time_view_t *v)
{
    v->drag_scroll = v->list && (v->sheet != 2 || !option_visible(v));
    float value = v->drag_scroll ? (float)lv_obj_get_scroll_y(v->list) /
        LV_MAX(1, lv_obj_get_scroll_y(v->list) + lv_obj_get_scroll_bottom(v->list)) : v->position;
    if (!orb_rim_drag_begin_relative(&s_rim, &v->drag, v->start_x, v->start_y, value)) return;
    if (v->page == ORB_TIME_PAGE_TIMER) {
        int64_t seconds = v->model.timer_duration_us / 1000000;
        int64_t unit = v->field == 0 ? 3600 : v->field == 1 ? 60 : 1;
        v->drag_unit_base = seconds - (v->field == 0 ? seconds / unit : seconds / unit % 60) * unit;
    }
    if (v->drag_scroll) lv_obj_set_scroll_dir(v->list, LV_DIR_NONE);
    else if (v->knob && !v->sheet) lv_obj_set_hidden(v->knob, false);
    lv_obj_invalidate(v->canvas);
}

static void pointer_move(orb_time_view_t *v, lv_point_t p)
{
    int dx = p.x - v->start_x, dy = p.y - v->start_y;
    float radius = hypotf(v->start_x - 233, v->start_y - 233);
    /* A list owns its contact even at a limit. Later sideways drift cannot
     * change that decision (interaction model v3, HIG §7). */
    if (!v->paging && v->list && lv_indev_get_scroll_obj(lv_indev_active()) == v->list)
        v->pointer_axis = LV_DIR_VER;
    if (radius < 193 && v->pointer_axis == LV_DIR_NONE) {
        if (abs(dx) > 12 && abs(dx) > abs(dy) * 1.3f) v->pointer_axis = LV_DIR_HOR;
        else if (abs(dy) > 12) v->pointer_axis = LV_DIR_VER;
    }
    bool rotary = !v->system_input && (tv_editable(v) || v->list != NULL);
    if (!v->paging && !v->drag.active && rotary && radius >= 193 && hypotf(dx, dy) >= 12) {
        float radial = ((v->start_x - 233) * dx + (v->start_y - 233) * dy) / radius;
        float tangent = ((v->start_x - 233) * dy - (v->start_y - 233) * dx) / radius;
        if (fabsf(tangent) > fabsf(radial) * 1.15f) rim_begin(v);
    }
    if (v->drag.active) {
        float value = orb_rim_drag_update(&s_rim, &v->drag, p.x, p.y, v->reduced);
        if (v->drag_scroll) {
            int maximum = lv_obj_get_scroll_y(v->list) + lv_obj_get_scroll_bottom(v->list);
            lv_obj_scroll_to_y(v->list, (int)(LV_CLAMP(0.f, value, 1.f) * maximum), LV_ANIM_OFF);
            orb_rim_detent(value, v->model.lap_count, &v->detent, true);
        } else set_value(v, v->page == ORB_TIME_PAGE_TIMER ? orb_rim_value(&s_rim, v->drag.angle) : value);
    } else if (!v->sheet && !v->reminder_detail && !v->due_view && radius < 193 && v->pointer_axis == LV_DIR_HOR) {
        if (!v->paging) {
            tv_remember(v);
            v->direction = dx < 0 ? 1 : -1;
            int destination = (int)v->page + v->direction;
            v->paging = true;
            if (destination >= 0 && destination <= ORB_TIME_PAGE_REMINDER) {
                v->peer = create(v->root, true, NULL);
                if (v->peer) {
                    v->peer->page = (orb_time_page_t)destination;
                    v->peer->field = destination == ORB_TIME_PAGE_TIMER ? 1 : 0;
                    v->peer->contrast = v->contrast; tv_build(v->peer);
                }
            }
        }
        float offset = v->peer ? LV_CLAMP(-494, dx + v->drag_base, 494) : (dx + v->drag_base) * .15f;
        if (v->peer && offset * v->direction > 0) offset *= .15f;
        page_offset(v, offset);
    }
}

static void pointer(lv_event_t *event)
{
    orb_time_view_t *v = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *input = lv_indev_active();
    /* Alerts are modal: no page swipe until their own action ends them. */
    if (!input || v->preview || v->alert) return;
    lv_point_t p; lv_indev_get_point(input, &p);
    lv_area_t area; lv_obj_get_coords(v->root, &area);
    p.x -= area.x1; p.y -= area.y1;
    if (code == LV_EVENT_PRESSED) {
        if (v->paging) lv_timer_pause(v->settle);
        v->drag_base = v->paging ? v->offset : 0;
        v->pointer_axis = v->paging ? LV_DIR_HOR : LV_DIR_NONE;
        v->pressed = true; v->start_x = p.x; v->start_y = p.y;
    } else if (code == LV_EVENT_PRESSING && v->pressed) {
        pointer_move(v, p);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        v->pressed = false;
        if (v->drag.active) {
            v->drag.active = false;
            if (v->drag_scroll) lv_obj_set_scroll_dir(v->list, LV_DIR_VER);
            else if (v->knob && !v->sheet) lv_obj_set_hidden(v->knob, true);
            lv_obj_invalidate(v->canvas);
            if (!v->drag_scroll) tv_update(v);
        }
        if (v->paging && lv_timer_get_paused(v->settle)) {
            v->target = code == LV_EVENT_RELEASED && v->peer && v->offset * -v->direction > 80 ? -v->direction * 494 : 0;
            v->settle_from = v->offset;
            v->last_tick = v->started = lv_tick_get();
            lv_timer_resume(v->settle);
        }
    }
}

static void deleted(lv_event_t *event)
{
    orb_time_view_t *v = lv_event_get_user_data(event);
    if (v->settle) lv_timer_delete(v->settle);
    orb_memory_free(v);
}

static orb_time_view_t *create(lv_obj_t *parent, bool preview, const orb_time_commands_t *commands)
{
    orb_time_view_t *v = orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(*v));
    if (!v) return NULL;
    v->preview = preview; v->field = 1; v->saved_field[ORB_TIME_PAGE_TIMER] = 1; v->commands = commands ? *commands : (orb_time_commands_t){0};
    v->root = tv_plain(parent); lv_obj_set_size(v->root, 466, 466);
    lv_obj_set_style_bg_color(v->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(v->root, LV_OPA_COVER, 0);
    lv_obj_set_clickable(v->root, true);
    lv_obj_add_event_cb(v->root, deleted, LV_EVENT_DELETE, v);
    if (!preview) {
        v->settle = lv_timer_create(settle_tick, 16, v); lv_timer_pause(v->settle);
        lv_obj_add_event_cb(v->root, pointer, LV_EVENT_PRESSED, v);
        lv_obj_add_event_cb(v->root, pointer, LV_EVENT_PRESSING, v);
        lv_obj_add_event_cb(v->root, pointer, LV_EVENT_RELEASED, v);
        lv_obj_add_event_cb(v->root, pointer, LV_EVENT_PRESS_LOST, v);
    }
    orb_time_snapshot(orb_time_now_us(), &v->model);
    if (v->model.alarm_ringing) v->page = ORB_TIME_PAGE_ALARM;
    else if (v->model.reminder_due) { v->page = ORB_TIME_PAGE_REMINDER; v->due_view = true; }
    v->arrival = !preview;
    tv_build(v);
    v->arrival = false;
    return v;
}

orb_time_view_t *orb_time_view_create(lv_obj_t *parent, const orb_time_commands_t *commands) { return create(parent, false, commands); }
void orb_time_view_destroy(orb_time_view_t *v) { if (v) lv_obj_delete(v->root); }
lv_obj_t *orb_time_view_root(const orb_time_view_t *v) { return v ? v->root : NULL; }
void orb_time_view_select(orb_time_view_t *v, orb_time_page_t page)
{
    if (!v || page > ORB_TIME_PAGE_REMINDER || v->page == page) return;
    if (v->model.alarm_ringing) page = ORB_TIME_PAGE_ALARM;
    tv_remember(v);
    if (v->settle) lv_timer_pause(v->settle);
    peer_delete(v); v->paging = false;
    v->page = page; v->field = v->saved_field[page];
    v->sheet = 0; v->reminder_detail = false; v->due_view = page == ORB_TIME_PAGE_REMINDER && v->model.reminder_due;
    orb_time_snapshot(orb_time_now_us(), &v->model);
    tv_build(v);
}
bool orb_time_view_back(orb_time_view_t *v)
{
    if (!v) return false;
    tv_remember(v);
    if (v->sheet) {
        orb_sound_play(ORB_SOUND_CLOSE);
        v->sheet = v->sheet == 3 ? 2 : 0;
        v->field = v->sheet ? 3 : v->saved_field[v->page]; tv_build(v); return true;
    }
    if (v->reminder_detail && !v->due_view) {
        v->reminder_detail = false; orb_sound_play(ORB_SOUND_CLOSE); tv_build(v); return true;
    }
    return false;
}

void orb_time_view_save(orb_time_view_t *v, orb_time_view_context_t *context)
{
    if (!v || !context) return;
    tv_remember(v);
    *context = (orb_time_view_context_t){
        .page = v->page, .sheet = v->sheet, .field = v->field,
        .reminder_id = v->reminder_id, .reminder_detail = v->reminder_detail,
        .detail_scroll = v->reminder_detail && v->list ? lv_obj_get_scroll_y(v->list) : 0,
    };
    memcpy(context->scroll, v->saved_scroll, sizeof(context->scroll));
    memcpy(context->sheet_scroll, v->sheet_scroll, sizeof(context->sheet_scroll));
    memcpy(context->fields, v->saved_field, sizeof(context->fields));
}

void orb_time_view_restore(orb_time_view_t *v, const orb_time_view_context_t *context)
{
    if (!v || !context || context->page > ORB_TIME_PAGE_REMINDER || context->sheet > 3) return;
    orb_time_snapshot(orb_time_now_us(), &v->model);
    /* Alerts keep their service-owned priority and ringing latch. Restoring
     * navigation is never a Stop, Snooze or Save. */
    if (v->model.alarm_ringing || v->model.reminder_due) return;
    memcpy(v->saved_scroll, context->scroll, sizeof(v->saved_scroll));
    memcpy(v->sheet_scroll, context->sheet_scroll, sizeof(v->sheet_scroll));
    memcpy(v->saved_field, context->fields, sizeof(v->saved_field));
    v->page = context->page; v->sheet = context->sheet; v->field = context->field;
    v->reminder_id = context->reminder_id; v->reminder_detail = context->reminder_detail;
    v->reminder_scroll = context->detail_scroll;
    v->due_view = false;
    tv_build(v);
    if (v->reminder_detail && v->list) lv_obj_scroll_to_y(v->list, context->detail_scroll, LV_ANIM_OFF);
}

size_t orb_time_view_state_size(void) { return sizeof(orb_time_view_t); }
orb_time_page_t orb_time_view_page(const orb_time_view_t *v) { return v ? v->page : ORB_TIME_PAGE_TIMER; }

void orb_time_view_set_system_input(orb_time_view_t *v, bool enabled)
{
    if (v) v->system_input = enabled;
}

lv_obj_t *orb_time_view_scroll(const orb_time_view_t *v) { return v ? v->list : NULL; }

bool orb_time_view_rotary(orb_time_view_t *v, orb_time_rotary_t *owner)
{
    if (!v || !owner) return false;
    orb_time_snapshot(orb_time_now_us(), &v->model);
    v->rotary_field = v->field;
    v->rotary_scroll = v->list && (v->sheet != 2 || !option_visible(v));
    if (v->rotary_scroll && (v->model.lap_count || v->page == ORB_TIME_PAGE_REMINDER || v->sheet)) {
        lv_obj_update_layout(v->root);
        float value = lv_obj_get_scroll_y(v->list);
        float maximum = LV_MAX(0, value + lv_obj_get_scroll_bottom(v->list));
        if (maximum <= 0) return false;
        *owner = (orb_time_rotary_t){.value = value, .maximum = maximum,
            .detent = 72, .units_per_turn = 900};
        return true;
    }
    if (!tv_editable(v)) return false;
    if (v->sheet == 2) {
        *owner = (orb_time_rotary_t){.value = v->field == 4 ? v->model.alarm_snooze_seconds / 60 : v->model.alarm_repeat,
            .minimum = v->field == 4 ? 1 : 0, .maximum = v->field == 4 ? 60 : 4,
            .detent = 1, .units_per_turn = v->field == 4 ? 30 : 5};
    } else if (v->page == ORB_TIME_PAGE_TIMER) {
        int unit = v->field == 0 ? 3600 : v->field == 1 ? 60 : 1;
        *owner = (orb_time_rotary_t){.value = (float)(v->model.timer_duration_us / 1000000 / unit),
            .minimum = 1.f / unit, .maximum = (float)ORB_TIME_TIMER_MAX_SECONDS / unit,
            .detent = 1, .units_per_turn = v->field ? 60 : 24};
    } else if (v->page == ORB_TIME_PAGE_ALARM) {
        *owner = (orb_time_rotary_t){.value = v->field ? v->model.alarm_minute : v->model.alarm_hour,
            .maximum = v->field ? 59 : 23, .detent = 1,
            .units_per_turn = v->field ? 60 : 24};
    }
    return true;
}

void orb_time_view_rotate(orb_time_view_t *v, float value)
{
    if (!v) return;
    if (v->list && (v->sheet != 2 || v->rotary_scroll)) {
        lv_obj_scroll_to_y(v->list, (int)lroundf(value), LV_ANIM_OFF);
        return;
    }
    if (!tv_editable(v)) return;
    if (!v->system_rotary) {
        v->system_rotary = true;
        if (v->knob && !v->sheet) lv_obj_set_hidden(v->knob, false);
        lv_obj_invalidate(v->canvas);
    }
    v->field = v->rotary_field;
    if (v->sheet == 2) {
        if (v->field == 4) orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_SNOOZE_SET, .a = (uint32_t)LV_CLAMP(1, lroundf(value), 60) * 60});
        else orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_REPEAT, .a = (orb_time_repeat_t)LV_CLAMP(0, lroundf(value), 4), .b = v->model.alarm_days ? v->model.alarm_days : ORB_TIME_DAYS_WEEKDAYS});
    } else if (v->page == ORB_TIME_PAGE_TIMER) {
        int64_t unit = v->field == 0 ? 3600 : v->field == 1 ? 60 : 1;
        int64_t seconds = v->model.timer_duration_us / 1000000;
        seconds = (int64_t)lroundf(value) * unit + seconds % unit;
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_TIMER_SET, .a = (uint32_t)LV_CLAMP(1, seconds, ORB_TIME_TIMER_MAX_SECONDS)});
    } else if (v->page == ORB_TIME_PAGE_ALARM) {
        unsigned hour = v->model.alarm_hour, minute = v->model.alarm_minute;
        if (v->field) minute = (unsigned)LV_CLAMP(0, lroundf(value), 59);
        else hour = (unsigned)LV_CLAMP(0, lroundf(value), 23);
        orb_time_command(&v->commands, &(orb_time_command_t){.kind = ORB_TIME_COMMAND_ALARM_SET, .a = hour, .b = minute});
    }
    orb_time_snapshot(orb_time_now_us(), &v->model);
    tv_update(v);
    position(v, v->position, false);
}

void orb_time_view_rotary_finish(orb_time_view_t *v)
{
    if (!v || !v->system_rotary) return;
    v->system_rotary = false;
    if (v->knob && !v->sheet) lv_obj_set_hidden(v->knob, true);
    lv_obj_invalidate(v->canvas);
    tv_update(v);
}

void orb_time_view_poll(orb_time_view_t *v)
{
    if (!v) return;
    orb_time_snapshot(orb_time_now_us(), &v->model);
    if (v->model.alarm_ringing) {
        tv_remember(v);
        v->sheet = 0;
        if (v->page != ORB_TIME_PAGE_ALARM) {
            orb_time_view_select(v, ORB_TIME_PAGE_ALARM);
            return;
        }
        v->field = v->saved_field[ORB_TIME_PAGE_ALARM];
    }
    if (!v->model.alarm_ringing && !v->model.reminder_due && v->model.timer_state == ORB_TIME_FINISHED &&
        (v->ringing_view || v->due_view)) {
        v->sheet = 0; v->due_view = v->reminder_detail = false;
        v->page = ORB_TIME_PAGE_TIMER; tv_build(v); return;
    }
    if (!v->model.alarm_ringing && v->model.reminder_due && !v->due_view) {
        tv_remember(v);
        v->sheet = 0; v->page = ORB_TIME_PAGE_REMINDER; v->due_view = true;
        v->reminder_detail = false; tv_build(v); return;
    }
    if (v->due_view && !v->model.reminder_due) {
        v->due_view = false; v->reminder_detail = false; tv_build(v); return;
    }
    if (v->ringing_view != (v->page == ORB_TIME_PAGE_ALARM && v->model.alarm_ringing) ||
        v->size != orb_fonts_content_size() || v->bold != orb_fonts_bold_text() ||
        v->opaque != orb_ui_reduce_transparency() || v->reduced != orb_ui_reduce_motion()) { tv_remember(v); tv_build(v); }
    else tv_update(v);
    if (v->peer) orb_time_view_poll(v->peer);
}

void orb_time_view_set_contrast(orb_time_view_t *v, bool increased)
{
    if (!v || v->contrast == increased) return;
    tv_remember(v);
    v->contrast = increased;
    tv_build(v);
}
