/**
 * @file lv_obj_raster_private.h
 *
 * ORB PATCH: raster cache, the hooks the core calls (see lv_obj_raster.h).
 */

#ifndef LV_OBJ_RASTER_PRIVATE_H
#define LV_OBJ_RASTER_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "../../include/lvgl/core/lv_obj_raster.h"

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Mark the raster caches that a change of `obj` makes outdated: those of its
 * ancestors and, if `self`, its own. A widget's own cache survives a move.
 */
void lv_obj_raster_mark_dirty(const lv_obj_t * obj, bool self);

/** Publish old/new composited bounds without invalidating captured pixels. */
void lv_obj_raster_invalidate_draw(const lv_obj_t * obj);

/**
 * Draw `obj` from its raster cache.
 * @return  false if it has no up-to-date cache and must be drawn as usual
 */
bool lv_obj_raster_draw(lv_layer_t * layer, lv_obj_t * obj);

/** Whether a cache keeps the object's original opaque coverage. A faded or
 * scaled cache must preserve the backdrop even when its source style is opaque. */
bool lv_obj_raster_is_opaque(const lv_obj_t * obj);

/**
 * Draw the outdated caches that this frame will show. Called once per frame,
 * after the invalidated areas are joined and before rendering starts.
 */
void lv_obj_raster_refresh(lv_display_t * disp);

/** Free the cache of a widget being deleted. */
void lv_obj_raster_delete(lv_obj_t * obj);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_OBJ_RASTER_PRIVATE_H*/
