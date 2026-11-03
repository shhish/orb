/*
 * The scan model (orb_geom_te_meets) follows Muse Gadgets' muse_lcd_bands.c,
 * Copyright (c) Meta Platforms, Inc. and affiliates, Apache License 2.0.
 */
#include "orb_display_geom.h"

#include <math.h>

void orb_geom_half_chords(int16_t *half, int d)
{
    float r = d / 2.0f;
    for (int y = 0; y < d; y++) {
        float dy = y + 0.5f - r;
        half[y] = (int16_t)(dy * dy < r * r ? sqrtf(r * r - dy * dy) + 1 : 0);
    }
}

void orb_geom_circle_cols(const int16_t *half, int d, int32_t y1, int32_t y2, int32_t *x1, int32_t *x2)
{
    int32_t y = y1 > d / 2 ? y1 : y2 < d / 2 ? y2 : d / 2;
    *x1 = (d / 2 - half[y]) & ~1;
    *x2 = (d / 2 + half[y]) | 1;
}

static void clip_cols(const int16_t *half, int d, int32_t y1, int32_t y2, int32_t ax1, int32_t ax2, int32_t *x1,
                      int32_t *x2)
{
    orb_geom_circle_cols(half, d, y1, y2, x1, x2);
    *x1 = *x1 > ax1 ? *x1 : ax1;
    *x2 = *x2 < ax2 ? *x2 : ax2;
}

void orb_geom_band(const int16_t *half, int d, int32_t y, int32_t y_last, int32_t ax1, int32_t ax2, uint32_t buf_px,
                   orb_geom_guard_t guard, int32_t *y2, int32_t *x1, int32_t *x2)
{
    /* Narrower rows near the top and bottom fit more of them into the buffer:
     * shrink until the band fits; its width only falls as it gets shorter. */
    int32_t rows = y_last - y + 1;
    for (;;) {
        clip_cols(half, d, y, y + rows - 1, ax1, ax2, x1, x2);
        int32_t w = *x2 - *x1 + 1;
        if (w <= 0 || (uint32_t)(w * rows) <= buf_px) {
            break;
        }
        rows = (int32_t)(buf_px / (uint32_t)w);
    }
    if (guard) {
        rows = guard(y, y_last, rows);
    }
    if (y + rows - 1 < y_last) {
        rows &= ~1;   /* the CO5300 takes only even-aligned windows */
    }
    rows = rows < 2 ? 2 : rows;
    *y2 = y + rows - 1 < y_last ? y + rows - 1 : y_last;
    clip_cols(half, d, y, *y2, ax1, ax2, x1, x2);
}

bool orb_geom_te_meets(int64_t t0, int64_t w, int32_t y1, int32_t y2, int64_t fall, int64_t p, int64_t scan,
                       int32_t h)
{
    for (int k = -1; k <= 3; k++) {
        int64_t f = fall + k * p;
        int64_t ta = f + scan * y1 / h, tb = f + scan * y2 / h;
        if ((t0 < ta) != (t0 + w < tb)) {
            return true;
        }
    }
    return false;
}

bool orb_geom_te_meets_window(int64_t start_min, int64_t start_max, int64_t wire_min, int64_t wire_max,
                             int32_t y1, int32_t y2, int64_t fall, int64_t p, int64_t scan,
                             int64_t phase_uncertainty, int32_t h)
{
    if (start_min > start_max || wire_min < 0 || wire_min > wire_max || phase_uncertainty < 0 ||
        p <= 0 || scan < 0 || h <= 0 || y1 < 0 || y1 > y2 || y2 > h) {
        return true;
    }
    int64_t end_min = start_min + wire_min, end_max = start_max + wire_max;
    for (int k = -1; k <= 3; k++) {
        int64_t f = fall + k * p;
        int64_t ta = f + scan * y1 / h, tb = f + scan * y2 / h;
        /* Every possible transfer must stay on one side of this scan at
         * both band edges. A narrow unsafe interval can lie between two
         * individually safe endpoint transfers. */
        bool before = start_max < ta - phase_uncertainty && end_max < tb - phase_uncertainty;
        bool after = start_min >= ta + phase_uncertainty && end_min >= tb + phase_uncertainty;
        if (!before && !after) return true;
    }
    return false;
}

int64_t orb_geom_te_delay_window(int64_t start_min, int64_t start_span, int64_t wire_min, int64_t wire_max,
                                int32_t y1, int32_t y2, int64_t fall, int64_t p, int64_t scan,
                                int64_t phase_uncertainty, int32_t h)
{
    if (start_span < 0 || wire_min < 0 || wire_min > wire_max || phase_uncertainty < 0 ||
        p <= 0 || scan < 0 || h <= 0 || y1 < 0 || y1 > y2 || y2 > h) {
        return -1;
    }
    const int64_t quantum = 250;
    const int64_t limit = (2 * p + quantum - 1) / quantum * quantum;
    const int64_t row_start = scan * y1 / h, row_end = scan * y2 / h;
    int64_t delay = 0;
    for (int k = -1; k <= 3; k++) {
        int64_t f = fall + k * p;
        int64_t ta = f + row_start, tb = f + row_end;
        /* A transfer is safe before this scan only when both latest edges
         * are strictly before it; safe after it when both earliest edges
         * are at or after it. Their complement is one forbidden interval
         * [before, after). These intervals are ordered by scan cycle. */
        int64_t before_start = ta - phase_uncertainty;
        int64_t before_end = tb - phase_uncertainty - wire_max;
        int64_t before = (before_start < before_end ? before_start : before_end) - start_span;
        int64_t after_start = ta + phase_uncertainty;
        int64_t after_end = tb + phase_uncertainty - wire_min;
        int64_t after = after_start > after_end ? after_start : after_end;
        int64_t candidate = start_min + delay;
        if (candidate >= before && candidate < after) {
            delay = (after - start_min + quantum - 1) / quantum * quantum;
            if (delay >= limit) return limit;
        }
    }
    return delay;
}
