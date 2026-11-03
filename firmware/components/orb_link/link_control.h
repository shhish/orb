#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Caller serializes access. Control state never depends on payload queue
 * capacity. A terminal observation is latched until the next connection. */
typedef struct {
    uint32_t conn;
    uint32_t accepted_frames;
    bool opened;
    bool ended;
    bool lost_frame;
} link_control_t;

typedef enum { LINK_CONTROL_UP, LINK_CONTROL_DOWN, LINK_CONTROL_LOST_FRAME } link_control_event_t;

static inline bool link_control_observe(link_control_t *state, uint32_t conn, link_control_event_t event)
{
    if (state->conn != conn) return false;
    if (event == LINK_CONTROL_UP) {
        if (!state->ended) state->opened = true;
    } else {
        state->ended = true;
        state->lost_frame |= event == LINK_CONTROL_LOST_FRAME;
    }
    return true;
}

/* Assign a sequence before publishing the owned text to the bounded queue.
 * Failure to publish afterwards must latch LOST_FRAME, never undo this count. */
static inline bool link_control_accept_frame(link_control_t *state, uint32_t conn, uint32_t *sequence)
{
    if (state->conn != conn || state->ended) return false;
    *sequence = ++state->accepted_frames;
    return true;
}

/* A normal peer close may follow pair.done in the same receive burst. Drain
 * its accepted prefix before closing; a lost prefix must instead resync now.
 * Equality works across sequence wrap because frames are consumed in FIFO. */
static inline bool link_control_should_end(const link_control_t *state, uint32_t handled_frames)
{
    return state->ended && (state->lost_frame || handled_frames == state->accepted_frames);
}
