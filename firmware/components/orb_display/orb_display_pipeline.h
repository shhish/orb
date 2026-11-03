#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Caller serializes these transitions. A disabled pipeline still owns every
 * queued/in-flight slot until its sender has drained or explicitly discarded
 * the job. Disabling must never turn a DMA source into a free buffer. */
#define ORB_PIPELINE_SLOTS 2
typedef enum { ORB_SLOT_FREE, ORB_SLOT_COPYING, ORB_SLOT_QUEUED, ORB_SLOT_IN_DMA } orb_slot_state_t;
typedef struct {
    orb_slot_state_t slot[ORB_PIPELINE_SLOTS];
    bool enabled;
    bool redraw;
} orb_pipeline_t;

static inline bool orb_pipeline_claim(orb_pipeline_t *p, unsigned slot)
{
    if (!p->enabled || slot >= ORB_PIPELINE_SLOTS || p->slot[slot] != ORB_SLOT_FREE) return false;
    p->slot[slot] = ORB_SLOT_COPYING;
    return true;
}

static inline bool orb_pipeline_queue(orb_pipeline_t *p, unsigned slot)
{
    if (!p->enabled || slot >= ORB_PIPELINE_SLOTS || p->slot[slot] != ORB_SLOT_COPYING) return false;
    p->slot[slot] = ORB_SLOT_QUEUED;
    return true;
}

static inline bool orb_pipeline_cancel_copy(orb_pipeline_t *p, unsigned slot)
{
    if (slot >= ORB_PIPELINE_SLOTS || p->slot[slot] != ORB_SLOT_COPYING) return false;
    p->slot[slot] = ORB_SLOT_FREE;
    return true;
}

static inline bool orb_pipeline_begin(orb_pipeline_t *p, unsigned slot)
{
    if (slot >= ORB_PIPELINE_SLOTS || p->slot[slot] != ORB_SLOT_QUEUED) return false;
    p->slot[slot] = ORB_SLOT_IN_DMA;
    return true;
}

static inline bool orb_pipeline_retire(orb_pipeline_t *p, unsigned slot)
{
    if (slot >= ORB_PIPELINE_SLOTS || p->slot[slot] != ORB_SLOT_IN_DMA) return false;
    p->slot[slot] = ORB_SLOT_FREE;
    return true;
}

static inline void orb_pipeline_disable(orb_pipeline_t *p)
{
    p->enabled = false;
    p->redraw = true;
}

typedef struct { int64_t started; bool failed; } orb_frame_result_t;

/* The last band is successful only if every preceding band in its frame was.
 * Keeping this state in the FIFO sender also handles interleaved brightness. */
static inline bool orb_frame_complete(orb_frame_result_t *f, int64_t started, bool band_ok, bool last)
{
    if (f->started != started) *f = (orb_frame_result_t){ .started = started };
    f->failed |= !band_ok;
    return last && !f->failed;
}
