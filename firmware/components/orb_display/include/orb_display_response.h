#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A single bounded response probe. Tokens travel with the queued band, so an
 * older transfer can never satisfy a newly injected navigation request. */
typedef struct {
    uint32_t token;
    int64_t input_us, first_band_us, first_frame_us, settled_frame_us;
    int16_t x1, y1, x2, y2;
    bool settled, causal;
} orb_display_response_t;

static inline void orb_response_complete(orb_display_response_t *response,
                                         uint32_t token, bool settled,
                                         int x1, int y1, int x2, int y2,
                                         int64_t completed, bool band_ok, bool frame_ok)
{
    if (!token || token != response->token || completed < response->input_us) return;
    bool intersects = x1 <= response->x2 && x2 >= response->x1 &&
                      y1 <= response->y2 && y2 >= response->y1;
    if (band_ok && intersects && !response->first_band_us) response->first_band_us = completed;
    if (frame_ok && !response->first_frame_us) response->first_frame_us = completed;
    if (frame_ok && settled && !response->settled_frame_us) response->settled_frame_us = completed;
}
