#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "orb_display_scroll_trace.h"

#define ORB_SCROLL_TRACE_RECORD_BYTES 13u
#define ORB_SCROLL_TRACE_CHUNK_BYTES 192u
#define ORB_SCROLL_TRACE_EMIT_TIMEOUT_US 10000000LL

static inline void orb_scroll_trace_u32le(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (i * 8));
}

/* No native structures cross the wire. uint32 relative microseconds cover the
 * bounded 60 s capture while retaining literal signed coordinates/arguments. */
static inline bool orb_scroll_trace_pack(const orb_display_scroll_trace_event_t *event,
                                         int64_t started_us,
                                         uint8_t out[ORB_SCROLL_TRACE_RECORD_BYTES])
{
    if (event->at_us < started_us) return false;
    uint64_t relative_us = (uint64_t)event->at_us - (uint64_t)started_us;
    if (relative_us > UINT32_MAX) return false;
    orb_scroll_trace_u32le(out, (uint32_t)relative_us);
    orb_scroll_trace_u32le(out + 4, (uint32_t)event->a);
    orb_scroll_trace_u32le(out + 8, (uint32_t)event->b);
    out[12] = event->kind;
    return true;
}
