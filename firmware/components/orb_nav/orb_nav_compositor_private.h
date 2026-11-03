#pragma once

#include "orb_nav_compositor.h"

/* Slot lifetime and transition rendering share one owner and allocation
 * contract. Keeping this state private avoids exposing mutable planes. */
typedef struct {
    uint16_t *pixels;
    uint64_t revision;
    unsigned next_y;
    bool valid, capturing;
} snapshot_t;

struct orb_nav_compositor {
    snapshot_t slots[ORB_NAV_SNAPSHOT_SLOTS];
    uint16_t width, height;
    unsigned from, to, prepared;
    orb_nav_transition_kind_t kind;
    orb_nav_direction_t direction;
    orb_nav_token_rect_t token;
    float position, velocity, target, accumulator, fade_start;
    float release_start;
    uint32_t fade_elapsed;
    bool active, dragging, reverse, reduced_motion;
    bool prepared_valid;
    orb_nav_transition_kind_t prepared_kind;
    const uint16_t *prepared_source;
    uint64_t prepared_revision;
    orb_nav_snapshot_alloc_fn allocate;
    orb_nav_snapshot_free_fn release;
    void *allocator_context;
    uint8_t *glyph;
    size_t glyph_capacity;
    orb_nav_token_rect_t glyph_source, glyph_target;
    uint16_t glyph_ink;
    bool glyph_visible;
    orb_nav_compositor_stats_t stats;
};

static inline bool slot_exists(const orb_nav_compositor_t *c, unsigned slot)
{
    return c && slot < ORB_NAV_SNAPSHOT_SLOTS;
}

/* Background preparation admits its spare plane through the same pool. */
bool orb_nav_compositor_admit_slot(orb_nav_compositor_t *c, unsigned slot);
