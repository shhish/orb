#include "orb_nav_compositor.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIDE 16
#define PIXELS (SIDE * SIDE)

static void capture(orb_nav_compositor_t *c, unsigned slot, uint16_t base)
{
    assert(orb_nav_compositor_capture_begin(c, slot, 17));
    uint16_t *pixels = orb_nav_compositor_pixels(c, slot);
    assert(pixels);
    for (unsigned y = 0; y < SIDE; y++)
        for (unsigned x = 0; x < SIDE; x++) pixels[y * SIDE + x] = base + y * SIDE + x;
    assert(orb_nav_compositor_capture_complete(c, slot));
    assert(orb_nav_compositor_valid(c, slot, 17));
    assert(!orb_nav_compositor_valid(c, slot, 18));
}

static void raster(orb_nav_compositor_t *c, uint16_t *out)
{
    assert(orb_nav_compositor_render_band(c, 0, 0, SIDE, SIDE, out, SIDE * 2, false));
}

static void endpoints_and_slide(void)
{
    orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
    capture(c, 0, 0x1000); capture(c, 1, 0x2000);
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_SLIDE, 0, 1,
                                    ORB_NAV_DIRECTION_LEFT, NULL, false, false));
    uint16_t out[PIXELS]; raster(c, out);
    assert(out[8 * SIDE + 5] == 0x1000 + 8 * SIDE + 5);
    orb_nav_compositor_drag(c, .5f); raster(c, out);
    /* 8px translation and 1px parallax gap, independently calculated. */
    assert(out[8 * SIDE] == 0x1000 + 8 * SIDE + 8);
    assert(out[8 * SIDE + 8] == 0);
    assert(out[8 * SIDE + 9] == 0x2000 + 8 * SIDE);
    assert(!orb_nav_compositor_capture_begin(c, 0, 19));
    assert(!orb_nav_compositor_render_band(c, 0, 0, SIDE, 1,
        orb_nav_compositor_pixels(c, 0) + SIDE, SIDE * 2, false));
    orb_nav_compositor_release(c, true, 1);
    for (unsigned i = 0; i < 100 && orb_nav_compositor_active(c); i++) orb_nav_compositor_tick(c, 16);
    assert(orb_nav_compositor_committed(c)); raster(c, out);
    assert(out[5] == 0x2005);
    orb_nav_compositor_stats_t stats; orb_nav_compositor_stats(c, &stats);
    assert(stats.snapshot_bytes == PIXELS * 6 && stats.captures == 2 && stats.frames == 3);
    orb_nav_compositor_cancel(c);
    assert(orb_nav_compositor_swap(c, 0, 1));
    assert(orb_nav_compositor_pixels(c, 0)[5] == 0x2005);
    assert(orb_nav_compositor_pixels(c, 1)[5] == 0x1005);
    orb_nav_compositor_destroy(c);
}

typedef struct { unsigned attempts, frees, allowed; size_t live, peak; } pool_t;

static void *pool_allocate(size_t bytes, void *context)
{
    pool_t *pool = context;
    if (++pool->attempts > pool->allowed) return NULL;
    size_t *allocation = malloc(bytes + sizeof(*allocation));
    assert(allocation); *allocation = bytes;
    pool->live += bytes;
    if (pool->live > pool->peak) pool->peak = pool->live;
    return allocation + 1;
}

static void pool_release(void *pixels, void *context)
{
    pool_t *pool = context;
    size_t *allocation = (size_t *)pixels - 1;
    pool->live -= *allocation; pool->frees++;
    free(allocation);
}

static void allocation_lifecycle(void)
{
    pool_t pool = {.allowed = 1};
    orb_nav_compositor_t *c = orb_nav_compositor_create_with_allocator(SIDE, SIDE,
        pool_allocate, pool_release, &pool);
    capture(c, 0, 0x1000);
    assert(!orb_nav_compositor_capture_begin(c, 1, 17));
    assert(orb_nav_compositor_valid(c, 0, 17));
    pool.allowed = 3; capture(c, 1, 0x2000);
    /* A sheet needs the spare background: OOM must keep both originals valid. */
    assert(!orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_SHEET, 0, 1,
                                     ORB_NAV_DIRECTION_UP, NULL, false, false));
    assert(!orb_nav_compositor_active(c));
    assert(orb_nav_compositor_valid(c, 0, 17) && orb_nav_compositor_valid(c, 1, 17));
    unsigned attempts = pool.attempts;
    for (unsigned cycle = 0; cycle < 20; cycle++) {
        assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_SLIDE, 0, 1,
                                        ORB_NAV_DIRECTION_LEFT, NULL, false, false));
        uint16_t out[PIXELS];
        for (unsigned frame = 0; frame < 30; frame++) {
            orb_nav_compositor_drag(c, frame / 29.f); raster(c, out);
        }
        orb_nav_compositor_cancel(c);
    }
    assert(pool.attempts == attempts && pool.live == PIXELS * 4 && pool.peak == PIXELS * 4);
    orb_nav_compositor_destroy(c);
    assert(!pool.live && pool.frees == 2);
}

static void prepared_invalidation_retires_transition(void)
{
    const orb_nav_transition_kind_t kinds[] = {
        ORB_NAV_TRANSITION_IRIS, ORB_NAV_TRANSITION_SHEET, ORB_NAV_TRANSITION_BACK,
    };
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
        capture(c, 0, 0x1000); capture(c, 1, 0x2000);
        assert(orb_nav_compositor_begin(c, kinds[i], 0, 1,
            ORB_NAV_DIRECTION_LEFT, NULL, false, false));
        orb_nav_compositor_drag(c, .5f);
        orb_nav_compositor_invalidate(c, 2);
        assert(!orb_nav_compositor_active(c));
        assert(!orb_nav_compositor_tick(c, 16));
        /* A replacement capture must not overwrite pixels still being read
         * by an iris/layer transition which consumed that prepared plane. */
        capture(c, 2, 0x3000);
        assert(!orb_nav_compositor_active(c));
        assert(orb_nav_compositor_begin(c, kinds[i], 0, 1,
            ORB_NAV_DIRECTION_LEFT, NULL, false, false));
        orb_nav_compositor_destroy(c);
    }
    /* Fades and slides consume only their two source planes. A spare-plane
     * refresh must not cancel those unrelated active presentations. */
    for (unsigned reduced = 0; reduced < 2; reduced++) {
        orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
        capture(c, 0, 0x1000); capture(c, 1, 0x2000);
        assert(orb_nav_compositor_begin(c,
            reduced ? ORB_NAV_TRANSITION_SHEET : ORB_NAV_TRANSITION_SLIDE, 0, 1,
            ORB_NAV_DIRECTION_LEFT, NULL, false, reduced));
        orb_nav_compositor_invalidate(c, 2);
        assert(orb_nav_compositor_active(c));
        orb_nav_compositor_destroy(c);
    }
}

static void glyph_ride(void)
{
    enum { size = 64 };
    orb_nav_compositor_t *c = orb_nav_compositor_create(size, size);
    for (unsigned slot = 0; slot < 2; slot++) {
        assert(orb_nav_compositor_capture_begin(c, slot, 1));
        uint16_t *pixels = orb_nav_compositor_pixels(c, slot);
        for (unsigned i = 0; i < size * size; i++) pixels[i] = slot ? 0x001F : 0xF800;
        if (!slot) for (unsigned y = 20; y < 28; y++)
            for (unsigned x = 21; x < 27; x++) pixels[y * size + x] = 0xFFFF;
        assert(orb_nav_compositor_capture_complete(c, slot));
    }
    orb_nav_token_rect_t token = {8, 8, 32, 32}, header = {41, 4, 6, 8};
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 0, 1,
                                    ORB_NAV_DIRECTION_LEFT, &token, false, false));
    assert(orb_nav_compositor_token_glyph(c, &header, 0x07E0));
    orb_nav_compositor_drag(c, .1f);
    uint16_t early[size * size];
    assert(orb_nav_compositor_render_band(c, 0, 0, size, size, early, size * 2, false));
    assert(early[19 * size + 29] == 0x07E0);
    orb_nav_compositor_drag(c, .5f);
    uint16_t full[size * size], partial[size * size];
    assert(orb_nav_compositor_render_band(c, 0, 0, size, size, full, size * 2, false));
    /* Header arrival precedes the readable destination, so the token cannot
     * fly across its rows. Retain the contrast-safe header ink there. */
    assert(full[7 * size + 43] == 0x07E0);
    for (unsigned y = 0; y < size; y += 8)
        assert(orb_nav_compositor_render_band(c, 0, y, size, 8, partial + y * size, size * 2, false));
    assert(!memcmp(full, partial, sizeof(full)));
    orb_nav_compositor_stats_t stats; orb_nav_compositor_stats(c, &stats);
    assert(stats.sprite_bytes == 22 * 22);
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 1, 0,
        ORB_NAV_DIRECTION_RIGHT, &token, true, false));
    assert(orb_nav_compositor_token_glyph(c, &header, 0x07E0));
    orb_nav_compositor_drag(c, .9f);
    assert(orb_nav_compositor_render_band(c, 0, 0, size, size, partial, size * 2, false));
    assert(partial[19 * size + 29] == 0x07E0);
    /* .1 and 1-.9 differ by float rounding at the AA aperture boundary.
     * Permit one RGB565 level there, while the glyph interior stays exact. */
    for (unsigned i = 0; i < size * size; i++) {
        assert(abs((early[i] >> 11) - (partial[i] >> 11)) <= 1);
        assert(abs(((early[i] >> 5) & 63) - ((partial[i] >> 5) & 63)) <= 1);
        assert(abs((early[i] & 31) - (partial[i] & 31)) <= 1);
    }
    orb_nav_compositor_destroy(c);
}

static void scanout_windows(void)
{
    orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
    capture(c, 0, 0x1000); capture(c, 1, 0x2000);
    orb_nav_token_rect_t token = {3, 5, 2, 2};
    for (unsigned kind = ORB_NAV_TRANSITION_SLIDE; kind <= ORB_NAV_TRANSITION_BOUNDARY; kind++) {
        for (unsigned direction = ORB_NAV_DIRECTION_LEFT; direction <= ORB_NAV_DIRECTION_DOWN; direction++) {
            assert(orb_nav_compositor_begin(c, kind, 0, kind == ORB_NAV_TRANSITION_BOUNDARY ? 0 : 1, direction, &token,
                direction & 1, false));
            for (int frame = -2; frame <= 12; frame++) {
                orb_nav_compositor_drag(c, frame / 10.f);
                uint16_t full[PIXELS]; raster(c, full);
                for (unsigned y = 0; y < SIDE; y += 4) {
                    unsigned x = y / 4, width = SIDE - x * 2;
                    unsigned stride = width * 2 + 4;
                    uint8_t clipped[(SIDE * 2 + 4) * 4];
                    memset(clipped, 0xA5, sizeof(clipped));
                    bool swapped = (y / 4) & 1;
                    assert(orb_nav_compositor_render_band(c, x, y, width, 4, clipped, stride, swapped));
                    for (unsigned row = 0; row < 4; row++) {
                        for (unsigned col = 0; col < width; col++) {
                            const uint8_t *pixel = clipped + row * stride + col * 2;
                            uint16_t actual;
                            if (swapped) actual = (uint16_t)((pixel[0] << 8) | pixel[1]);
                            else memcpy(&actual, pixel, 2);
                            assert(actual == full[(y + row) * SIDE + x + col]);
                        }
                        for (unsigned pad = width * 2; pad < stride; pad++)
                            assert(clipped[row * stride + pad] == 0xA5);
                    }
                }
            }
        }
    }
    orb_nav_compositor_destroy(c);
}

static void boundary_bounce(void)
{
    orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
    capture(c, 0, 0x1000);
    assert(!orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_SLIDE, 0, 0,
                                     ORB_NAV_DIRECTION_RIGHT, NULL, false, false));
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_BOUNDARY, 0, 0,
                                    ORB_NAV_DIRECTION_RIGHT, NULL, false, false));
    orb_nav_compositor_drag(c, .25f);
    uint16_t out[PIXELS]; raster(c, out);
    assert(out[8 * SIDE] == 0 && out[8 * SIDE + 3] == 0);
    assert(out[8 * SIDE + 4] == 0x1000 + 8 * SIDE);
    assert(out[8 * SIDE + 15] == 0x1000 + 8 * SIDE + 11);
    orb_nav_compositor_stats_t stats; orb_nav_compositor_stats(c, &stats);
    assert(stats.snapshot_bytes == PIXELS * 6); /* All planes were reserved at create. */
    /* Even an erroneous commit request cannot cross an absent map edge. */
    orb_nav_compositor_release(c, true, 1);
    for (unsigned frame = 0; frame < 100 && orb_nav_compositor_active(c); frame++)
        orb_nav_compositor_tick(c, 16);
    assert(!orb_nav_compositor_active(c) && !orb_nav_compositor_committed(c));
    assert(orb_nav_compositor_progress(c) == 0);
    raster(c, out);
    assert(out[8 * SIDE + 4] == 0x1000 + 8 * SIDE + 4);
    orb_nav_compositor_destroy(c);
}

static void iris_and_partial(void)
{
    orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
    capture(c, 0, 0x1000); capture(c, 1, 0x2000);
    orb_nav_token_rect_t token = {7, 7, 2, 2};
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 0, 1,
                                    ORB_NAV_DIRECTION_LEFT, &token, false, false));
    orb_nav_compositor_drag(c, .35f);
    uint16_t full[PIXELS], bands[PIXELS]; raster(c, full);
    assert(full[8 * SIDE + 8] == 0x2000 + 8 * SIDE + 8);
    assert(full[0] == 0x1000);
    memset(bands, 0, sizeof(bands));
    for (unsigned y = 0; y < SIDE; y += 4)
        assert(orb_nav_compositor_render_band(c, 0, y, SIDE, 4, bands + y * SIDE, SIDE * 2, false));
    assert(!memcmp(full, bands, sizeof(full)));
    uint8_t swapped[PIXELS * 2];
    assert(orb_nav_compositor_render_band(c, 0, 0, SIDE, SIDE, swapped, SIDE * 2, true));
    for (unsigned i = 0; i < PIXELS; i++) assert(swapped[i * 2] == (full[i] >> 8) && swapped[i * 2 + 1] == (full[i] & 255));
    /* Reverse iris exposes the destination outside, source inside. */
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 1, 0,
                                    ORB_NAV_DIRECTION_RIGHT, &token, true, false));
    orb_nav_compositor_drag(c, .65f); raster(c, bands);
    assert(!memcmp(full, bands, sizeof(full)));
    orb_nav_compositor_destroy(c);
}

static void iris_isolation(void)
{
    enum { side = 64 };
    orb_nav_compositor_t *c = orb_nav_compositor_create(side, side);
    for (unsigned slot = 0; slot < 2; slot++) {
        assert(orb_nav_compositor_capture_begin(c, slot, 1));
        uint16_t *pixels = orb_nav_compositor_pixels(c, slot);
        for (unsigned i = 0; i < side * side; i++) pixels[i] = slot ? 0x001F : 0xF800;
        assert(orb_nav_compositor_capture_complete(c, slot));
    }
    orb_nav_token_rect_t token = {3, 7, 10, 10};
    uint16_t out[side * side], reverse[side * side];
    for (unsigned frame = 1; frame < 10; frame++) {
        float progress = frame / 10.f;
        assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 0, 1,
            ORB_NAV_DIRECTION_LEFT, &token, false, false));
        assert(!orb_nav_compositor_capture_begin(c, 2, 9));
        orb_nav_compositor_drag(c, progress);
        assert(orb_nav_compositor_render_band(c, 0, 0, side, side, out, side * 2, false));
        float radius = 5 + (hypotf(55, 51) + 1 - 5) * progress;
        for (unsigned y = 0; y < side; y++) for (unsigned x = 0; x < side; x++) {
            float distance = hypotf((float)x - 8, (float)y - 12);
            if (distance > radius + 1) assert(out[y * side + x] == (25 << 11));
            if (distance < radius - 2) assert(out[y * side + x] == 0x001F);
        }
        /* Opening and closing share exactly the same aperture, rim and dim.
         * This catches destination ink leaking into the outside source plane. */
        assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 1, 0,
            ORB_NAV_DIRECTION_RIGHT, &token, true, false));
        orb_nav_compositor_drag(c, 1 - progress);
        assert(orb_nav_compositor_render_band(c, 0, 0, side, side, reverse, side * 2, false));
        assert(!memcmp(out, reverse, sizeof(out)));
    }
    orb_nav_compositor_destroy(c);
}

static void sheet_and_reduced_motion(void)
{
    orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
    capture(c, 0, 0); capture(c, 1, 0xFFFF - PIXELS);
    uint16_t *a = orb_nav_compositor_pixels(c, 0);
    for (unsigned i = 0; i < PIXELS; i++) a[i] = 0xFFFF;
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_SHEET, 0, 1,
                                    ORB_NAV_DIRECTION_UP, NULL, false, false));
    orb_nav_compositor_drag(c, .5f);
    uint16_t out[PIXELS]; raster(c, out);
    /* 40% white quantizes to RGB565 12/25/12 (polish #4: a receded place). */
    assert(out[4 * SIDE + 4] == ((12 << 11) | (25 << 5) | 12));
    orb_nav_compositor_stats_t stats; orb_nav_compositor_stats(c, &stats);
    assert(stats.snapshot_bytes == PIXELS * 6);
    assert(!orb_nav_compositor_valid(c, 2, 0));
    orb_nav_compositor_cancel(c);
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_SHEET, 0, 1,
                                    ORB_NAV_DIRECTION_UP, NULL, false, true));
    orb_nav_compositor_release(c, true, 4);
    assert(orb_nav_compositor_tick(c, 30));
    assert(fabsf(orb_nav_compositor_progress(c) - .2f) < .0001f);
    raster(c, out);
    /* RM has no translation, parallax, scale or depth. The outgoing layer
     * fades first (60% left at p=.2), then the incoming one (20% at p=.6). */
    for (unsigned i = 0; i < PIXELS; i++)
        assert(out[i] == ((19 << 11) | (38 << 5) | 19));
    assert(orb_nav_compositor_tick(c, 60));
    raster(c, out);
    uint16_t b = orb_nav_compositor_pixels(c, 1)[4 * SIDE + 4];
    assert((out[4 * SIDE + 4] >> 11) == (((b >> 11) * 51 + 128) >> 8));
    /* HIG §6: the whole change takes 150ms. */
    assert(orb_nav_compositor_tick(c, 60));
    assert(!orb_nav_compositor_active(c) && orb_nav_compositor_committed(c));
    orb_nav_compositor_destroy(c);
}

static void cancellation_and_capture(void)
{
    orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
    assert(!orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_SLIDE, 0, 1,
                                     ORB_NAV_DIRECTION_LEFT, NULL, false, false));
    uint16_t row[SIDE] = {0};
    assert(orb_nav_compositor_capture_begin(c, 0, 8));
    assert(!orb_nav_compositor_capture_band(c, 0, 0, 1, SIDE, 1, row, SIDE * 2, false));
    assert(!orb_nav_compositor_capture_end(c, 0));
    capture(c, 0, 0x1000); capture(c, 1, 0x2000);
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_SLIDE, 0, 1,
                                    ORB_NAV_DIRECTION_RIGHT, NULL, false, false));
    orb_nav_compositor_drag(c, -.4f);
    assert(orb_nav_compositor_progress(c) < 0 && orb_nav_compositor_progress(c) > -.4f);
    orb_nav_compositor_drag(c, .8f);
    orb_nav_compositor_release(c, true, 3);
    orb_nav_compositor_tick(c, 16);
    float moving = orb_nav_compositor_progress(c);
    orb_nav_compositor_retarget(c, false);
    orb_nav_compositor_tick(c, 1);
    assert(fabsf(orb_nav_compositor_progress(c) - moving) < .05f);
    for (unsigned i = 0; i < 150 && orb_nav_compositor_active(c); i++) orb_nav_compositor_tick(c, 16);
    assert(!orb_nav_compositor_active(c) && !orb_nav_compositor_committed(c));
    assert(orb_nav_compositor_progress(c) == 0);
    orb_nav_compositor_invalidate_all(c);
    assert(!orb_nav_compositor_valid(c, 0, 17));
    orb_nav_compositor_destroy(c);
    assert(!orb_nav_compositor_create(0, SIDE));
    assert(!orb_nav_compositor_create(1025, SIDE));
}

static void prepared_return_lifetime(void)
{
    orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
    capture(c, 0, 0x1000); capture(c, 1, 0x2000);
    orb_nav_token_rect_t token = {3, 5, 6, 6};
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 0, 1,
        ORB_NAV_DIRECTION_LEFT, &token, false, false));
    uint16_t opening[PIXELS], returning[PIXELS];
    orb_nav_compositor_drag(c, .5f); raster(c, opening);
    orb_nav_compositor_cancel(c);
    assert(orb_nav_compositor_swap(c, 0, 1));
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 0, 1,
        ORB_NAV_DIRECTION_RIGHT, &token, true, false));
    orb_nav_compositor_drag(c, .5f); raster(c, returning);
    assert(!memcmp(opening, returning, sizeof(opening)));
    orb_nav_compositor_stats_t stats; orb_nav_compositor_stats(c, &stats);
    assert(stats.background_prepares == 1 && stats.background_hits == 1);
    orb_nav_compositor_cancel(c);
    /* Rewriting a source with the same external revision still retires its
     * derived plane. Pointer and revision equality alone cannot prove reuse. */
    capture(c, 1, 0x3000);
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 0, 1,
        ORB_NAV_DIRECTION_RIGHT, &token, true, false));
    orb_nav_compositor_drag(c, .5f); raster(c, returning);
    assert(memcmp(opening, returning, sizeof(opening)));
    orb_nav_compositor_stats(c, &stats);
    assert(stats.background_prepares == 2 && stats.background_hits == 1);
    orb_nav_compositor_cancel(c);
    capture(c, 2, 0x4000);
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 0, 1,
        ORB_NAV_DIRECTION_RIGHT, &token, true, false));
    orb_nav_compositor_stats(c, &stats);
    assert(stats.background_prepares == 3);
    orb_nav_compositor_destroy(c);
}

static void round_reverse_invisible_prefix(void)
{
    enum { side = 64 };
    static const orb_nav_token_rect_t tokens[] = {
        {24, 24, 16, 16}, {0, 12, 12, 12}, {48, 30, 12, 12}, {-6, -6, 16, 16}, {0, 0, 1, 1},
    };
    orb_nav_compositor_t *c = orb_nav_compositor_create(side, side);
    for (unsigned slot = 0; slot < 2; slot++) {
        assert(orb_nav_compositor_capture_begin(c, slot, 1));
        uint16_t *pixels = orb_nav_compositor_pixels(c, slot);
        for (unsigned i = 0; i < side * side; i++)
            pixels[i] = slot ? (uint16_t)(0x8000 + i) : (uint16_t)(0x1000 + i);
        assert(orb_nav_compositor_capture_complete(c, slot));
    }
    uint16_t source[side * side], skipped[side * side];
    for (unsigned token = 0; token < sizeof(tokens) / sizeof(tokens[0]); token++) {
        assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 0, 1,
            ORB_NAV_DIRECTION_RIGHT, &tokens[token], true, false));
        float prefix = orb_nav_compositor_reverse_iris_round_start(c);
        assert(isfinite(prefix) && prefix > 0 && prefix < 1);
        assert(orb_nav_compositor_render_band(c, 0, 0, side, side, source, side * 2, false));
        for (unsigned sample = 1; sample <= 8; sample++) {
            orb_nav_compositor_drag(c, prefix * sample / 8);
            assert(orb_nav_compositor_render_band(c, 0, 0, side, side, skipped, side * 2, false));
            for (unsigned y = 0; y < side; y++) for (unsigned x = 0; x < side; x++) {
                /* Independent panel coverage oracle, including edge pixels
                 * with any antialiased coverage. No compositor geometry here. */
                float dx = x + .5f - side * .5f, dy = y + .5f - side * .5f;
                if (dx * dx + dy * dy < (side * .5f + .5f) * (side * .5f + .5f))
                    assert(source[y * side + x] == skipped[y * side + x]);
            }
        }
        orb_nav_compositor_drag(c, prefix + (1 - prefix) * .3f);
        orb_nav_compositor_release(c, false, 0);
        for (unsigned frame = 0; frame < 150 && orb_nav_compositor_active(c); frame++)
            orb_nav_compositor_tick(c, 16);
        assert(!orb_nav_compositor_active(c) && !orb_nav_compositor_committed(c));
        assert(orb_nav_compositor_progress(c) == prefix);
        assert(orb_nav_compositor_render_band(c, 0, 0, side, side, skipped, side * 2, false));
        for (unsigned y = 0; y < side; y++) for (unsigned x = 0; x < side; x++) {
            float dx = x + .5f - side * .5f, dy = y + .5f - side * .5f;
            if (dx * dx + dy * dy < (side * .5f + .5f) * (side * .5f + .5f))
                assert(source[y * side + x] == skipped[y * side + x]);
        }
    }
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 0, 1,
        ORB_NAV_DIRECTION_LEFT, &tokens[0], false, false));
    assert(orb_nav_compositor_reverse_iris_round_start(c) == 0);
    assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, 0, 1,
        ORB_NAV_DIRECTION_RIGHT, &tokens[0], true, true));
    assert(orb_nav_compositor_reverse_iris_round_start(c) == 0);
    orb_nav_compositor_destroy(c);
}

static void reduced_motion_never_double_exposes(void)
{
    /* Polish #33: no RM frame shows both places. Red outgoing, blue incoming:
     * every pixel of every sampled frame carries at most one of them. A
     * release partway through a tracked fade finishes the remaining share. */
    orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
    for (unsigned slot = 0; slot < 2; slot++) {
        assert(orb_nav_compositor_capture_begin(c, slot, 3));
        uint16_t *pixels = orb_nav_compositor_pixels(c, slot);
        for (unsigned i = 0; i < PIXELS; i++) pixels[i] = slot ? 0x001F : 0xF800;
        assert(orb_nav_compositor_capture_complete(c, slot));
    }
    const orb_nav_transition_kind_t kinds[] = {
        ORB_NAV_TRANSITION_SLIDE, ORB_NAV_TRANSITION_BACK, ORB_NAV_TRANSITION_SHEET, ORB_NAV_TRANSITION_IRIS,
    };
    orb_nav_token_rect_t token = {4, 4, 6, 6};
    uint16_t out[PIXELS];
    for (unsigned kind = 0; kind < sizeof(kinds) / sizeof(kinds[0]); kind++) {
        assert(orb_nav_compositor_begin(c, kinds[kind], 0, 1, ORB_NAV_DIRECTION_LEFT, &token, false, true));
        for (unsigned step = 0; step <= 20; step++) {
            orb_nav_compositor_drag(c, step / 20.f); raster(c, out);
            for (unsigned i = 0; i < PIXELS; i++) assert(!(out[i] & 0xF800) || !(out[i] & 0x001F));
        }
        orb_nav_compositor_drag(c, .5f);
        orb_nav_compositor_release(c, true, 0);
        unsigned elapsed = 0;
        while (orb_nav_compositor_active(c)) {
            assert(orb_nav_compositor_tick(c, 5) || !orb_nav_compositor_active(c));
            elapsed += 5; raster(c, out);
            for (unsigned i = 0; i < PIXELS; i++) assert(!(out[i] & 0xF800) || !(out[i] & 0x001F));
        }
        assert(elapsed == 75 && orb_nav_compositor_committed(c));
    }
    orb_nav_compositor_destroy(c);
}

static void layer_is_whole_slab(void)
{
    /* Polish #3/#4: a departing page (Back) or an arriving sheet moves as a
     * complete slab, including its captured corner pixels. Only the panel
     * bezel clips the final output; there is no second moving disc mask. */
    enum { side = 64 };
    const struct { orb_nav_transition_kind_t kind; orb_nav_direction_t direction; } cases[] = {
        {ORB_NAV_TRANSITION_BACK, ORB_NAV_DIRECTION_RIGHT}, {ORB_NAV_TRANSITION_SHEET, ORB_NAV_DIRECTION_UP},
        {ORB_NAV_TRANSITION_SHEET, ORB_NAV_DIRECTION_DOWN}, {ORB_NAV_TRANSITION_SHEET, ORB_NAV_DIRECTION_LEFT},
    };
    orb_nav_compositor_t *c = orb_nav_compositor_create(side, side);
    for (unsigned slot = 0; slot < 2; slot++) {
        assert(orb_nav_compositor_capture_begin(c, slot, 5));
        uint16_t *pixels = orb_nav_compositor_pixels(c, slot);
        for (unsigned i = 0; i < side * side; i++) pixels[i] = (uint16_t)((slot ? 0x8000 : 0x1000) + i);
        assert(orb_nav_compositor_capture_complete(c, slot));
    }
    uint16_t out[side * side];
    unsigned beneath = 0, layer = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool sheet = cases[i].kind == ORB_NAV_TRANSITION_SHEET;
        assert(orb_nav_compositor_begin(c, cases[i].kind, 0, 1, cases[i].direction, NULL, false, false));
        for (unsigned frame = 1; frame < 10; frame++) {
            float p = frame / 10.f;
            orb_nav_compositor_drag(c, p);
            assert(orb_nav_compositor_render_band(c, 0, 0, side, side, out, side * 2, false));
            /* Independent translation: Back moves `from` by p; a sheet brings
             * `to` in from the far side with the compositor's parallax peek. */
            int sign = cases[i].direction == ORB_NAV_DIRECTION_LEFT || cases[i].direction == ORB_NAV_DIRECTION_UP ? -1 : 1;
            int displacement = (int)lroundf(p * side * sign);
            int peek = (int)lroundf(.06f * side * sinf(3.14159265f * p));
            int offset = sheet ? displacement - sign * (side + peek) : displacement;
            bool horizontal = cases[i].direction == ORB_NAV_DIRECTION_LEFT || cases[i].direction == ORB_NAV_DIRECTION_RIGHT;
            int ox = horizontal ? offset : 0, oy = horizontal ? 0 : offset;
            const uint16_t *moving = orb_nav_compositor_pixels(c, sheet ? 1 : 0);
            const uint16_t *prepared = orb_nav_compositor_pixels(c, 2);
            for (int y = 0; y < side; y++) for (int x = 0; x < side; x++) {
                int sx = x - ox, sy = y - oy;
                if (sx < 0 || sx >= side || sy < 0 || sy >= side) {
                    assert(out[y * side + x] == prepared[y * side + x]); beneath++;
                } else {
                    assert(out[y * side + x] == moving[sy * side + sx]); layer++;
                }
            }
        }
        orb_nav_compositor_cancel(c);
    }
    assert(beneath > 1000 && layer > 1000);
    orb_nav_compositor_destroy(c);
}

int main(void)
{
    endpoints_and_slide(); iris_and_partial(); sheet_and_reduced_motion(); cancellation_and_capture();
    allocation_lifecycle(); glyph_ride(); scanout_windows(); boundary_bounce(); iris_isolation();
    prepared_return_lifetime();
    prepared_invalidation_retires_transition();
    round_reverse_invisible_prefix();
    reduced_motion_never_double_exposes();
    layer_is_whole_slab();
    puts("nav compositor: transition, capture, lifetime and isolation contracts passed");
    return 0;
}
