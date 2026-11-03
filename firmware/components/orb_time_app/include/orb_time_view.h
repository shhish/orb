#pragma once

#include "lvgl.h"
#include "orb_time_command.h"

typedef struct orb_time_view orb_time_view_t;
typedef enum {
    ORB_TIME_PAGE_TIMER, ORB_TIME_PAGE_STOPWATCH, ORB_TIME_PAGE_ALARM,
    ORB_TIME_PAGE_REMINDER,
} orb_time_page_t;

/* UI-thread owned. System left-rim Back leaves Time; PWR locks. No service state belongs
 * to the view: destroying it never stops a timer, stopwatch or armed alarm. Service
 * changes leave through the copied command port; NULL calls orb_time directly. */
orb_time_view_t *orb_time_view_create(lv_obj_t *parent, const orb_time_commands_t *commands);
void orb_time_view_destroy(orb_time_view_t *view);
/* State only, excluding LVGL children/shared fonts/materials. */
size_t orb_time_view_state_size(void);
/* Consume Back inside an editor/detail before the shell leaves Time. */
bool orb_time_view_back(orb_time_view_t *view);
lv_obj_t *orb_time_view_root(const orb_time_view_t *view);
/* Caller polls at most 4 Hz. Only changed labels/rim bands are damaged. */
void orb_time_view_poll(orb_time_view_t *view);
void orb_time_view_set_contrast(orb_time_view_t *view, bool increased);
void orb_time_view_select(orb_time_view_t *view, orb_time_page_t page);
orb_time_page_t orb_time_view_page(const orb_time_view_t *view);

typedef struct {
    float value, minimum, maximum, detent, units_per_turn;
} orb_time_rotary_t;
/* The shell recognizer owns rim arbitration; the standalone preview retains
 * its local input. Rotary changes values/scroll only, never commits an action. */
void orb_time_view_set_system_input(orb_time_view_t *view, bool enabled);
bool orb_time_view_rotary(orb_time_view_t *view, orb_time_rotary_t *owner);
void orb_time_view_rotate(orb_time_view_t *view, float value);
void orb_time_view_rotary_finish(orb_time_view_t *view);
lv_obj_t *orb_time_view_scroll(const orb_time_view_t *view);

/* Navigation memory is view state, never timer/alarm service state. The shell
 * can retain this small value while Time is destroyed for an iris or lock. */
typedef struct {
    orb_time_page_t page;
    int scroll[4], sheet_scroll[4], detail_scroll;
    unsigned fields[4], sheet, field;
    uint32_t reminder_id;
    bool reminder_detail;
} orb_time_view_context_t;
void orb_time_view_save(orb_time_view_t *view, orb_time_view_context_t *context);
void orb_time_view_restore(orb_time_view_t *view, const orb_time_view_context_t *context);
