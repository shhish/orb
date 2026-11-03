#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Arrival arms the minimum presentation before a navigation snapshot. The
 * general arrival fence starts the finite sweep after the transition, or
 * settles it immediately when it is no longer a fresh, visible arrival. */
void orb_kit_ring_set_arrival(lv_obj_t *obj, int32_t value);
void orb_kit_rim_slider_set_arrival(lv_obj_t *obj, int32_t value);
void orb_kit_level_set_arrival(lv_obj_t *obj, int32_t value);

/* Attached list indicators start quiet. The host consumes their pending
 * arrival once; Back/restore uses false. Raw layout/offset restoration never
 * admits feedback. Native pointer scrolling and explicit rim input do. */
void orb_kit_scroll_arc_arrival_prepare(lv_obj_t *list);
void orb_kit_scroll_arc_input(lv_obj_t *list);

/* Optional format projection (temperature, units, etc.) for the same presented
 * sample as the arc. It must only format presentation, never issue commands.
 * Called while the UI lock is held; output storage is supplied by the kit. */
typedef void (*orb_kit_ring_format_cb_t)(float value, char *out, size_t capacity, void *context);
void orb_kit_ring_set_formatter(lv_obj_t *obj, orb_kit_ring_format_cb_t callback, void *context);
/* Couple an existing readout to the arc without changing its geometry/style.
 * NULL detaches it. The readout's semantic text is always the true target. */
void orb_kit_ring_set_value_label(lv_obj_t *obj, lv_obj_t *label, const char *reserve_template);
lv_obj_t *orb_kit_chip_value_label(lv_obj_t *chip);
typedef void (*orb_kit_ring_present_cb_t)(float presented, float target, void *context);
void orb_kit_ring_set_presenter(lv_obj_t *obj, orb_kit_ring_present_cb_t callback, void *context);
float orb_kit_ring_get_presented_value(lv_obj_t *obj);
/* Diagnostics distinguish an animated presentation from its true target.
 * Native arc units preserve the existing trace schema (plain ring: tenths). */
bool orb_kit_ring_semantic_value(lv_obj_t *obj, int32_t *out);
bool orb_kit_ring_semantic_text(lv_obj_t *label, const char **out);

#ifdef __cplusplus
}
#endif
