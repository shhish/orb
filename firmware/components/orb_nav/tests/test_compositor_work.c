/* A deterministic work budget for the device's swapped snapshot bands.
 * Compile the compositor with __XTENSA__, -fno-builtin-memcpy (as IDF does),
 * and memcpy=orb_nav_test_memcpy. _FORTIFY_SOURCE=0 prevents Darwin's header
 * from silently rewriting the interception back to a compiler builtin.
 * Pixel oracles cannot catch per-pixel calls. */
#ifdef memcpy
#undef memcpy
#endif
#include "orb_nav_compositor.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SIDE = 466, BAND_ROWS = 84, MAX_TINY_CALLS_PER_ROW = 4 };
static size_t s_tiny_calls, s_span_calls, s_span_pixels;

void *orb_nav_test_memcpy(void *destination, const void *source, size_t bytes)
{
    if (bytes <= 4) s_tiny_calls++;
    return memcpy(destination, source, bytes);
}

/* Only the S3 assembly transfer is substituted. The compositor's actual
 * device dispatch and byte conversion execute with IDF's compiler policy. */
void orb_simd_copy_rgb565(void *destination, const void *source, size_t pixels)
{
    s_span_calls++;
    s_span_pixels += pixels;
    memcpy(destination, source, pixels * 2);
}

int main(void)
{
    orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
    assert(c);
    for (unsigned slot = 0; slot < 2; slot++) {
        assert(orb_nav_compositor_capture_begin(c, slot, 1));
        uint16_t *pixels = orb_nav_compositor_pixels(c, slot);
        for (unsigned i = 0; i < SIDE * SIDE; i++) pixels[i] = (uint16_t)(i * 137 + slot * 1031);
        assert(orb_nav_compositor_capture_complete(c, slot));
    }
    uint8_t *storage = malloc(SIDE * BAND_ROWS * 2 + 32);
    assert(storage);
    uint8_t *aligned = (uint8_t *)(((uintptr_t)storage + 15) & ~(uintptr_t)15);
    const orb_nav_token_rect_t token = {197, 197, 72, 72};
    static const int bands[] = {0, 84, 168, 252, 336, 382};
    static const float positions[] = {0, .17f, .5f, .83f, 1};
    unsigned cases = 0;
    size_t maximum = 0;
    for (unsigned kind = ORB_NAV_TRANSITION_SLIDE; kind <= ORB_NAV_TRANSITION_BACK; kind++) {
        for (unsigned direction = ORB_NAV_DIRECTION_LEFT; direction <= ORB_NAV_DIRECTION_DOWN; direction++) {
            for (unsigned reverse = 0; reverse < 2; reverse++) {
                assert(orb_nav_compositor_begin(c, kind, 0, 1, direction, &token, reverse, false));
                for (unsigned position = 0; position < sizeof(positions) / sizeof(positions[0]); position++) {
                    orb_nav_compositor_drag(c, positions[position]);
                    for (unsigned band = 0; band < sizeof(bands) / sizeof(bands[0]); band++) {
                        /* Halfword and odd capture destinations supplement the
                         * real aligned 466x84 DMA band without weakening its budget. */
                        for (unsigned offset = 0; offset < 16; offset++) {
                            s_tiny_calls = s_span_calls = s_span_pixels = 0;
                            assert(orb_nav_compositor_render_band(c, 0, bands[band], SIDE, BAND_ROWS,
                                aligned + offset, SIDE * 2, true));
                            if (s_tiny_calls > maximum) maximum = s_tiny_calls;
                            if (s_tiny_calls > MAX_TINY_CALLS_PER_ROW * BAND_ROWS) {
                                fprintf(stderr, "kind=%u direction=%u reverse=%u progress=%.2f y=%d offset=%u: "
                                    "%zu tiny memcpy calls for %u pixels (limit %u)\n", kind, direction,
                                    reverse, positions[position], bands[band], offset, s_tiny_calls,
                                    SIDE * BAND_ROWS, MAX_TINY_CALLS_PER_ROW * BAND_ROWS);
                                return 1;
                            }
                            /* A span transfer is required; a scalar rewrite of
                             * every copied pixel would evade the call budget. */
                            assert(s_span_calls && s_span_calls <= BAND_ROWS * 3);
                            assert(s_span_pixels >= SIDE * BAND_ROWS / 2);
                            assert(s_span_pixels <= SIDE * BAND_ROWS * 2);
                            if (kind == ORB_NAV_TRANSITION_BACK || kind == ORB_NAV_TRANSITION_SHEET)
                                assert(s_span_pixels == SIDE * BAND_ROWS);
                            cases++;
                        }
                    }
                }
            }
        }
    }
    printf("nav work budget: %u device-band cases; at most %zu tiny memcpy calls per 39144 pixels (limit %u)\n",
        cases, maximum, MAX_TINY_CALLS_PER_ROW * BAND_ROWS);
    free(storage);
    orb_nav_compositor_destroy(c);
    return 0;
}
