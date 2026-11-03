#pragma once

/* Private to orb_time_app: the presenter's state and the helpers shared by
 * its page (orb_time_view.c), readout (orb_time_digits.c), sheet and list
 * (orb_time_sheet.c) and alert/detail (orb_time_alert.c) builders. UI thread
 * only; nothing here owns service state. */
#include "orb_time_view.h"
#include "orb_time.h"
#include "orb_rim.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_effects.h"

/* HIG §§4/5: semantic secondary ink, including accessibility. */
#define TV_SECONDARY ORB_KIT_SECONDARY
#define TV_ACCENT ORB_KIT_TIME

enum { PRIMARY, SECONDARY_ACTION, FIELD_HOUR, FIELD_MINUTE, FIELD_SECOND,
       PAGE_TIMER, PAGE_STOPWATCH, PAGE_ALARM, FINISH, LAPS_MORE,
       ALARM_TOGGLE, ALARM_SNOOZE, REMINDER_SNOOZE, REMINDER_DONE, PAGE_NAV, PRESETS, SHEET_CLOSE,
       REPEAT_EDIT, CUSTOM_EDIT, SNOOZE_EDIT, CLOCK_FORMAT, SAVE_RETRY, TIMER_REPEAT,
       PRESET_BASE = 40, DAY_BASE = 50 };

/* One family geometry (HIG §§3/7, Design 1.0 Timer/Alarm/Stopwatch frames):
 * every Time readout centres on the same hero line, captions and capsules use
 * shared slots, and every alert keeps its secondary capsule above the tinted
 * six o'clock action. */
enum {
    PAGE_DOT_TOP = 420,
    LAP_VIEW_TOP = 140,
    LAP_VIEW_BOTTOM = PAGE_DOT_TOP - ORB_RIM_MARGIN_TEXT,
    TIMER_RING_RADIUS = 213,
    TIMER_RING_WIDTH = 12,
    TV_HERO_Y = 188,
    TV_STOPWATCH_HERO_Y = 180, /* ≥20 px of ink clearance above the Laps capsule. */
    TV_CAPTION_Y = 258,       /* Timer/Stopwatch caption above the glyph pair. */
    TV_ALARM_CAPTION_Y = 250, /* Alarm and alert caption above a capsule. */
    TV_SLOT_Y = 268,          /* A capsule that replaces the caption (Laps, Retry). */
    TV_ALERT_ACTION_TOP = 276,
    TV_ALARM_ACTION_TOP = 284,
    TV_STATUS_Y = 104,        /* Failure line directly under the rim title. */
    TV_LAP_ROWS = 276,        /* First lap row, in the lap viewport. */
    TV_LAP_ANCHOR = 252,      /* Laps scroll position: rows under the header. */
    TV_LAP_PIN = 140,         /* Counter gone: the compact header takes over. */
    TV_LAP_CONTROLS = 176,    /* Glyph pair fully faded before the viewport clips it. */
    TV_BODY_TOP = 96,
    TV_BODY_BOTTOM = 264,
};

struct orb_time_view {
    lv_obj_t *root, *content, *canvas, *ring, *navigation, *title, *digits, *groups[3], *focus[3], *detail;
    lv_obj_t *primary, *secondary, *knob, *list, *summary, *laps_link, *elapsed;
    lv_obj_t *suffix, *ampm, *switch_btn, *hint, *sw_sem, *status;
    lv_obj_t *rem_time, *rem_title, *rem_snooze, *rem_done;
    lv_obj_t *repeat_label, *snooze_label, *format_label;
    unsigned sheet; /* 0 app, 1 quick timers, 2 alarm options, 3 custom days. */
    uint32_t reminder_id, listed_signature;
    int reminder_scroll;
    bool reminder_detail, due_view;
    orb_time_page_t page;
    orb_time_snapshot_t model;
    orb_rim_drag_t drag;
    struct orb_time_view *peer;
    lv_timer_t *settle;
    orb_time_commands_t commands;
    float position, offset, velocity, target, drag_base, settle_from;
    int saved_scroll[4], sheet_scroll[4];
    unsigned saved_field[4];
    int detent, direction, start_x, start_y, ring_percent;
    uint32_t last_tick, started;
    unsigned field, lap_total, digit_format;
    lv_dir_t pointer_axis;
    int64_t drag_unit_base;
    orb_content_size_t size;
    bool bold, opaque, reduced, contrast, pressed, paging, preview, ring_hidden, primary_tinted;
    bool system_input, system_rotary, ringing_view, rotary_scroll, drag_scroll;
    bool alert, editing, error_shown, arrival, timer_sampled;
    orb_time_state_t timer_state_shown;
    int64_t timer_duration_shown, timer_remaining_shown;
    uint32_t waiting_started;
    unsigned rotary_field;
    char primary_name[16], secondary_name[16];
};

/* orb_time_view.c: shared construction helpers and lifecycle. */
lv_obj_t *tv_plain(lv_obj_t *parent);
void tv_changed(lv_obj_t *label, const char *value);
void tv_colour(const orb_time_view_t *v, lv_obj_t *o, lv_color_t ink);
lv_obj_t *tv_text(lv_obj_t *parent, const char *value, int cx, int cy, int width,
                  orb_text_style_t style, orb_text_variant_t variant);
lv_obj_t *tv_hit(lv_obj_t *parent, int x, int y, int w, int h, int command, orb_time_view_t *v);
void tv_bind(orb_time_view_t *v, lv_obj_t *obj, int command);
void tv_semantic(lv_obj_t *obj, const char *name);
lv_obj_t *tv_rim_title(orb_time_view_t *v, const char *name);
lv_obj_t *tv_capsule(orb_time_view_t *v, lv_obj_t *parent, const char *name, bool primary, int command);
lv_obj_t *tv_sheet_list(orb_time_view_t *v);
lv_obj_t *tv_status(orb_time_view_t *v, const char *text);
void tv_build_navigation(orb_time_view_t *v);
void tv_contrast_apply(lv_obj_t *obj);
bool tv_editable(const orb_time_view_t *v);
void tv_build(orb_time_view_t *v);
void tv_update(orb_time_view_t *v);
void tv_remember(orb_time_view_t *v);
void tv_controls(orb_time_view_t *v, const char *primary, const char *secondary,
                  const char *symbol, int command);

/* orb_time_digits.c: the centred tabular hero readout. */
bool tv_digits_show(orb_time_view_t *v, lv_obj_t *parent, int cy);
void tv_digits_project(float presented, float target, void *context);
void tv_stopwatch_text(char *out, size_t capacity, int64_t us);

/* orb_time_sheet.c: editor sheets and the reminder list. */
void tv_build_sheet(orb_time_view_t *v);
void tv_sheet_update(orb_time_view_t *v);
void tv_build_reminders(orb_time_view_t *v);
uint32_t tv_reminder_signature(const orb_time_view_t *v);
void tv_reminder_when(const orb_reminder_t *reminder, time_t now, char *out, size_t capacity, bool due);

/* orb_time_alert.c: one alert template (ringing alarm, finished timer, due
 * reminder) and the reminder detail that shares its body. */
bool tv_alert_state(const orb_time_view_t *v);
bool tv_reminder_selected(orb_time_view_t *v, orb_reminder_t *out);
void tv_build_alert(orb_time_view_t *v);
void tv_build_reminder(orb_time_view_t *v, const orb_reminder_t *reminder);
void tv_alert_update(orb_time_view_t *v);
void tv_reminder_snooze_failed(orb_time_view_t *v);
