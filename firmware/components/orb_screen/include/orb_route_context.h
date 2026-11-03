#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Fixed, caller-owned value contexts for a shell's compatible route slots.
 * Keeping the 16-byte record inline avoids allocation or a store search while
 * observing scroll. All-zero storage is an unused route; no pointers or queued
 * operations can enter its versioned three-integer payload. */
typedef struct { uint32_t words[4]; } orb_route_context_t;
typedef struct {
    int32_t scroll, focus;
    uint32_t page;
    bool valid;
} orb_route_location_t;

orb_route_location_t orb_route_context_read(const orb_route_context_t *context);
void orb_route_context_save(orb_route_context_t *context, int32_t scroll, uint32_t page, int32_t focus);
/* Scroll observation does not mark an unused route valid. */
void orb_route_context_scroll(orb_route_context_t *context, int32_t scroll);
/* The caller chooses whether reading identity shares the ordinary focus slot.
 * This operation keeps the page and resets scroll only for another identity. */
int32_t orb_route_context_reading(orb_route_context_t *context, uint32_t identity);
