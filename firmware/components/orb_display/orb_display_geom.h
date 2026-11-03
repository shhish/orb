#pragma once

/*
 * The round panel's geometry and its scan, as plain C (no ESP-IDF, no LVGL),
 * so the band choice and the tearing model are tested on the host.
 */

#include <stdbool.h>
#include <stdint.h>

/* Picks how many rows a band may have, starting at y of an area ending at
 * y_last, given that `rows` fit; orb_display_band_guard_t. */
typedef int32_t (*orb_geom_guard_t)(int32_t y, int32_t y_last, int32_t rows);

/*
 * half[y], for each of the d rows of a d x d panel: half the width of the
 * visible circle on that row, counting the pixels whose centres lie inside,
 * plus one for the bezel's edge.
 */
void orb_geom_half_chords(int16_t *half, int d);

/* Columns [*x1, *x2] the panel shows anywhere in rows [y1, y2]: the chord at
 * the row nearest the centre. x1 even and x2 odd, as the CO5300 wants. */
void orb_geom_circle_cols(const int16_t *half, int d, int32_t y1, int32_t y2, int32_t *x1, int32_t *x2);

/*
 * The next band of an invalidated area (columns [ax1, ax2], rows y to y_last)
 * for a draw buffer of buf_px pixels: as many rows as fit at the width the
 * circle needs, cut short by `guard` (may be NULL), an even count unless it
 * ends the area. Sets the band's last row and its columns; *x1 > *x2 when
 * nothing in those rows is visible.
 */
void orb_geom_band(const int16_t *half, int d, int32_t y, int32_t y_last, int32_t ax1, int32_t ax2, uint32_t buf_px,
                   orb_geom_guard_t guard, int32_t *y2, int32_t *x1, int32_t *x2);

/*
 * The panel scans its memory top to bottom over `scan` us, starting at each
 * `fall` of its tearing-effect line, every `p` us. A write of rows [y1, y2) of
 * an h-row panel, starting at t0 and taking w us, tears if it crosses a scan:
 * one reaches y1 first and the other y2. True if it meets any scan around it.
 */
bool orb_geom_te_meets(int64_t t0, int64_t w, int32_t y1, int32_t y2, int64_t fall, int64_t p, int64_t scan,
                       int32_t h);

/* Conservative predictive counterpart: the pixel transaction may start
 * anywhere in [start_min, start_max], take [wire_min, wire_max] us, and the
 * scan phase may differ by +/- phase_uncertainty us. True means some crossing
 * is possible, so endpoints alone are insufficient to prove a safe window.
 * Uses the same nearby TE cycles as te_meets; pass an epoch near start_min.
 * Invalid bounds fail closed. Equal bounds and zero phase uncertainty match
 * te_meets exactly. All times must leave room for nearby-cycle arithmetic. */
bool orb_geom_te_meets_window(int64_t start_min, int64_t start_max, int64_t wire_min, int64_t wire_max,
                             int32_t y1, int32_t y2, int64_t fall, int64_t p, int64_t scan,
                             int64_t phase_uncertainty, int32_t h);

/* First safe start on the existing 250 us trial grid, expressed as a delay
 * from start_min. Each trial admits a start uncertainty of start_span us and
 * the same wire/phase uncertainty as te_meets_window. Returns -1 for invalid
 * bounds; a result >= 2*p means no safe trial within the two-period horizon.
 * Computes each scan's forbidden interval once instead of repeatedly dividing
 * its band coordinates at every trial. Same nearby-cycle/arithmetic contract
 * as te_meets_window; this does not weaken its conservative safety proof. */
int64_t orb_geom_te_delay_window(int64_t start_min, int64_t start_span, int64_t wire_min, int64_t wire_max,
                                int32_t y1, int32_t y2, int64_t fall, int64_t p, int64_t scan,
                                int64_t phase_uncertainty, int32_t h);
