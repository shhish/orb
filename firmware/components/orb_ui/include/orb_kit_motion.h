#pragma once

#include "lvgl.h"
#include "orb_delight_motion.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One paused-at-rest UI clock serves all effects. A key identifies a property.
 * Callbacks apply final values on finish and return whether motion continues.
 * Retargeting and ticks allocate nothing; deleting obj cancels safely. */
typedef bool (*orb_kit_motion_cb_t)(void *context, uint32_t elapsed_ms,
                                    uint32_t dt_ms, bool finish);
bool orb_kit_motion_start(lv_obj_t *obj, const void *key,
                          orb_kit_motion_cb_t callback, void *context);
/* Named waits use 100ms; simultaneous faster effects do not increase their
 * callback rate. The common timer adopts the earliest outstanding deadline. */
bool orb_kit_motion_start_interval(lv_obj_t *obj, const void *key,
    orb_kit_motion_cb_t callback, void *context, uint32_t period_ms);
/* Named waits have a 10Hz ceiling and do not delay a navigation's settled
 * report. Their absolute lifetime remains the waiting helper's responsibility. */
bool orb_kit_motion_start_wait(lv_obj_t *obj, const void *key,
    orb_kit_motion_cb_t callback, void *context);
bool orb_kit_motion_start_wait_until(lv_obj_t *obj, const void *key,
    orb_kit_motion_cb_t callback, void *context, uint32_t deadline_ms);
void orb_kit_motion_cancel(lv_obj_t *obj, const void *key, bool finish);
void orb_kit_motion_finish(lv_obj_t *parent);
/* A new contact ends existing presentation. Its own press/scroll feedback
 * remains responsive; presenters call this at their input ownership boundary. */
void orb_kit_motion_interrupt(lv_obj_t *parent);
/* Admission helpers retain this value so a later snapshot cannot re-arm an
 * arrival after the contact that already ended its prepared presentation. */
uint32_t orb_kit_motion_interrupt_generation(void);
bool orb_kit_motion_start_feedback(lv_obj_t *obj, const void *key,
    orb_kit_motion_cb_t callback, void *context);
bool orb_kit_motion_allowed(lv_obj_t *obj);
void orb_kit_motion_set_active(lv_obj_t *parent, bool active);
bool orb_kit_motion_timer_running(void);
unsigned orb_kit_motion_count(void);
/* Finite presentation under this tree, excluding ongoing named waits. */
bool orb_kit_motion_settling(lv_obj_t *parent);

/* Start values are prepared before destination snapshots. The callback is
 * consumed once: true on fresh committed arrival; false on Back, cover/restore
 * or after 1200ms without a signal. Effects finish within 450ms of completion. */
typedef void (*orb_kit_arrival_cb_t)(void *context, bool animate);
bool orb_kit_arrival_prepare(lv_obj_t *obj, orb_kit_arrival_cb_t callback, void *context);
void orb_kit_arrival_complete(lv_obj_t *parent, bool fresh);

/* Reserve clock ownership while constructing hidden retained controls. */
bool orb_kit_motion_prepare(lv_obj_t *obj);

/* Stationary material/outline fades remain available under Reduce Motion.
 * Visibility and quiet-surface gates still finish them immediately. */
bool orb_kit_motion_start_fade(lv_obj_t *obj, const void *key,
    orb_kit_motion_cb_t callback, void *context);

#ifdef __cplusplus
}
#endif
