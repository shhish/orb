#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct { bool alive; } lv_obj_t;
typedef struct lv_timer_t lv_timer_t;
typedef struct lv_display_t lv_display_t;
typedef struct lv_event_t lv_event_t;
typedef struct lv_event_dsc_t lv_event_dsc_t;
typedef void (*lv_event_cb_t)(lv_event_t *event);
#define LV_EVENT_REFR_START 1
#define LV_EVENT_REFR_READY 2
typedef void (*lv_timer_cb_t)(lv_timer_t *timer);
void lv_lock(void);
void lv_unlock(void);
lv_obj_t *lv_layer_top(void);
lv_obj_t *lv_screen_active(void);
lv_obj_t *lv_obj_create(lv_obj_t *parent);
void lv_screen_load(lv_obj_t *screen);
void lv_obj_delete(lv_obj_t *object);
lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period, void *context);
void lv_timer_delete(lv_timer_t *timer);
void lv_timer_ready(lv_timer_t *timer);
void *lv_timer_get_user_data(lv_timer_t *timer);
lv_display_t *lv_display_get_default(void);
lv_event_dsc_t *lv_display_add_event_cb(lv_display_t *display, lv_event_cb_t callback,
                                       int filter, void *context);
uint32_t lv_display_remove_event_cb_with_user_data(lv_display_t *display,
                                                 lv_event_cb_t callback, void *context);
void *lv_event_get_user_data(lv_event_t *event);
int lv_event_get_code(lv_event_t *event);

uint32_t lv_tick_get(void);
void lv_timer_set_period(lv_timer_t *timer, uint32_t period);
void lv_timer_reset(lv_timer_t *timer);
void lv_timer_pause(lv_timer_t *timer);
void lv_timer_resume(lv_timer_t *timer);
