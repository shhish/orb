/* Host tests: the round panel's band geometry and the scan model. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "orb_display_geom.h"

#define D 466

static int s_failed;
static uint64_t s_checks;

#define CHECK(cond)                                                     \
    do {                                                                \
        s_checks++;                                                     \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failed++;                                                 \
        }                                                               \
    } while (0)

static int16_t s_half[D];
static uint32_t s_buf_px;

/* Is pixel (x, y) on the panel's glass? Its centre within the circle. */
static int visible(int x, int y)
{
    float dx = x + 0.5f - D / 2.0f, dy = y + 0.5f - D / 2.0f;
    return dx * dx + dy * dy <= (D / 2.0f) * (D / 2.0f);
}

static void test_chords(void)
{
    for (int y = 0; y < D; y++) {
        CHECK(s_half[y] == s_half[D - 1 - y]);
        CHECK(s_half[y] > 0);
    }
    CHECK(s_half[0] < s_half[D / 4] && s_half[D / 4] < s_half[D / 2]);
    CHECK(abs(s_half[D / 2] - D / 2) <= 1);
}

/* Bands that tile the whole screen: in order, in the buffer, aligned, holding every visible pixel. */
static void check_tiling(orb_geom_guard_t guard)
{
    int32_t y = 0, bands = 0;
    long px = 0;
    while (y < D) {
        int32_t y2, x1, x2;
        orb_geom_band(s_half, D, y, D - 1, 0, D - 1, s_buf_px, guard, &y2, &x1, &x2);
        CHECK(y2 >= y && y2 < D);
        CHECK(x1 >= 0 && x2 <= D - 1 && x1 <= x2);
        CHECK(x1 % 2 == 0 && x2 % 2 == 1);
        CHECK((long)(x2 - x1 + 1) * (y2 - y + 1) <= s_buf_px);
        CHECK(y2 == D - 1 || (y2 - y + 1) % 2 == 0);
        for (int32_t r = y; r <= y2; r++) {
            for (int32_t x = 0; x < D; x++) {
                if (visible(x, r)) {
                    CHECK(x >= x1 && x <= x2);
                }
            }
        }
        px += (long)(x2 - x1 + 1) * (y2 - y + 1);
        y = y2 + 1;
        bands++;
    }
    CHECK(bands >= 4 && bands <= 8);
    /* The corners stay unsent: less than the full square. */
    CHECK(px < (long)D * D * 95 / 100);
}

/* A glass control in rows [100, 179]: bands may not end inside it. */
static int32_t guard(int32_t y, int32_t y_last, int32_t rows)
{
    (void)y_last;
    int32_t end = y + rows - 1;
    if (100 > y && 100 <= end && 179 > end) {
        end = 99;
    }
    return end - y + 1;
}

static void test_guard(void)
{
    int32_t y = 0;
    while (y < D) {
        int32_t y2, x1, x2;
        orb_geom_band(s_half, D, y, D - 1, 0, D - 1, s_buf_px, guard, &y2, &x1, &x2);
        CHECK(!(y2 >= 100 && y2 < 179));
        y = y2 + 1;
    }
}

static void test_small_area(void)
{
    /* A corner of the square: nothing to draw. */
    int32_t y2, x1, x2;
    orb_geom_band(s_half, D, 0, 20, 0, 40, s_buf_px, NULL, &y2, &x1, &x2);
    CHECK(x1 > x2);
    /* A small area in the middle keeps its own columns. */
    orb_geom_band(s_half, D, 200, 251, 100, 201, s_buf_px, NULL, &y2, &x1, &x2);
    CHECK(y2 == 251 && x1 == 100 && x2 == 201);
}

static void test_te(void)
{
    /* Scan: 16 ms over the rows, every 16.8 ms, starting at t = 1000. */
    int64_t fall = 1000, p = 16800, scan = 16000;
    /* Rows 0..99 written well after the scan passed them, done before it comes round again. */
    CHECK(!orb_geom_te_meets(fall + 4000, 2000, 0, 100, fall, p, scan, D));
    /* The same rows, written while the scan crosses them. */
    CHECK(orb_geom_te_meets(fall + 1000, 3000, 0, 200, fall, p, scan, D));
    /* A full-screen write just ahead of the scan: faster than it, it stays ahead... */
    CHECK(!orb_geom_te_meets(fall - 100, 12000, 0, 466, fall, p, scan, D));
    /* ...slower, the scan overtakes it. */
    CHECK(orb_geom_te_meets(fall - 100, 20000, 0, 466, fall, p, scan, D));
}

static uint32_t random_u32(void)
{
    static uint32_t state = 0x13a9c8e7;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void test_te_window(void)
{
    const int64_t fall = 100000, p = 16900, scan = 16000;
    const int64_t ta = fall + scan * 100 / D, tb = fall + scan * 101 / D;
    /* A 14 us unsafe start interval lies between two safe observations.
     * Sampling only earliest/latest starts would certify this incorrectly. */
    CHECK(!orb_geom_te_meets(ta - 33, 20, 100, 101, fall, p, scan, D));
    CHECK(!orb_geom_te_meets(ta + 67, 20, 100, 101, fall, p, scan, D));
    CHECK(orb_geom_te_meets(ta + 1, 20, 100, 101, fall, p, scan, D));
    CHECK(orb_geom_te_meets_window(ta - 33, ta + 67, 20, 20, 100, 101, fall, p, scan, 0, D));
    /* Both phase endpoints are safe too; the interior phase still crosses. */
    CHECK(!orb_geom_te_meets(ta + 1, 20, 100, 101, fall - 100, p, scan, D));
    CHECK(!orb_geom_te_meets(ta + 1, 20, 100, 101, fall + 100, p, scan, D));
    CHECK(orb_geom_te_meets_window(ta + 1, ta + 1, 20, 20, 100, 101, fall, p, scan, 100, D));
    CHECK(!orb_geom_te_meets_window(ta - 500, ta - 200, 20, 50, 100, 101, fall, p, scan, 100, D));
    CHECK(!orb_geom_te_meets_window(tb + 100, tb + 400, 20, 50, 100, 101, fall, p, scan, 100, D));
    /* Equality uses exactly the existing strict-before / inclusive-after rule. */
    CHECK(orb_geom_te_meets_window(ta, ta, tb - ta - 1, tb - ta - 1, 100, 101, fall, p, scan, 0, D));
    CHECK(!orb_geom_te_meets_window(ta, ta, tb - ta, tb - ta, 100, 101, fall, p, scan, 0, D));
    CHECK(orb_geom_te_meets_window(ta + 1, ta, 20, 20, 100, 101, fall, p, scan, 0, D));
    CHECK(orb_geom_te_meets_window(ta, ta, -1, 20, 100, 101, fall, p, scan, 0, D));
    CHECK(orb_geom_te_meets_window(ta, ta, 20, 19, 100, 101, fall, p, scan, 0, D));
    CHECK(orb_geom_te_meets_window(ta, ta, 20, 20, 100, 101, fall, p, scan, -1, D));
    CHECK(orb_geom_te_meets_window(ta, ta, 20, 20, 100, 101, fall, 0, scan, 0, D));
    CHECK(orb_geom_te_meets_window(ta, ta, 20, 20, 100, 101, fall, p, scan, 0, 0));

    for (unsigned i = 0; i < 100000; i++) {
        int64_t period = 8000 + random_u32() % 32001;
        int64_t duration = period - random_u32() % 1000;
        int64_t start = fall + (int64_t)(random_u32() % (4 * period)) - period;
        int64_t wire = random_u32() % 40001;
        int32_t y1 = random_u32() % D, y2 = y1 + 1 + random_u32() % (D - y1);
        CHECK(orb_geom_te_meets_window(start, start, wire, wire, y1, y2, fall, period, duration, 0, D) ==
              orb_geom_te_meets(start, wire, y1, y2, fall, period, duration, D));
    }

    /* Exhaust every integer combination in randomly positioned small windows.
     * A conservative rejection may include extra combinations; it may never
     * certify clear when the original exact scan model finds a crossing. */
    for (unsigned i = 0; i < 10000; i++) {
        int64_t period = 8000 + random_u32() % 32001;
        int64_t duration = period - random_u32() % 1000;
        int32_t y1 = random_u32() % D, y2 = y1 + 1 + random_u32() % (D - y1);
        int64_t wire_min = random_u32() % 22001, wire_max = wire_min + random_u32() % 8;
        int64_t phase = random_u32() % 5;
        /* Concentrate half the windows near either band-edge crossing. */
        int64_t start_min = i % 2 ? fall + duration * y1 / D - 4 + random_u32() % 9 :
            fall + duration * y2 / D - wire_min - 4 + random_u32() % 9;
        start_min += ((int32_t)(random_u32() % 4) - 1) * period;
        int64_t start_max = start_min + random_u32() % 8;
        bool possible = orb_geom_te_meets_window(start_min, start_max, wire_min, wire_max,
                                                y1, y2, fall, period, duration, phase, D);
        for (int64_t start = start_min; start <= start_max; start++) {
            for (int64_t wire = wire_min; wire <= wire_max; wire++) {
                for (int64_t offset = -phase; offset <= phase; offset++) {
                    CHECK(possible || !orb_geom_te_meets(start, wire, y1, y2, fall + offset,
                                                        period, duration, D));
                }
            }
        }
    }
    unsigned clear_windows = 0, possible_windows = 0;
    for (unsigned i = 0; i < 4000; i++) {
        int64_t period = 14000 + random_u32() % 6001;
        int64_t duration = period - 100 - random_u32() % 1000;
        int32_t y1 = random_u32() % D, y2 = y1 + 1 + random_u32() % (D - y1);
        int64_t start_min = fall + (int64_t)(random_u32() % (4 * period)) - period;
        int64_t start_max = start_min + random_u32() % 601;
        int64_t wire_min = 100 + random_u32() % 4000;
        int64_t wire_max = wire_min + random_u32() % 2001;
        int64_t phase = random_u32() % 201;
        bool possible = orb_geom_te_meets_window(start_min, start_max, wire_min, wire_max,
                                                y1, y2, fall, period, duration, phase, D);
        if (possible) possible_windows++;
        else clear_windows++;
        for (unsigned si = 0; si <= 4; si++) {
            for (unsigned wi = 0; wi <= 4; wi++) {
                for (unsigned pi = 0; pi <= 4; pi++) {
                    int64_t start = start_min + (start_max - start_min) * si / 4;
                    int64_t wire = wire_min + (wire_max - wire_min) * wi / 4;
                    int64_t offset = -phase + 2 * phase * pi / 4;
                    CHECK(possible || !orb_geom_te_meets(start, wire, y1, y2, fall + offset,
                                                        period, duration, D));
                }
            }
        }
    }
    CHECK(clear_windows > 0 && possible_windows > 0);
}

/* The old sender searches the 250 us grid. Preserve every admission decision,
 * including strict equality, interior uncertainty and the two-period cutoff. */
static int64_t te_delay_oracle(int64_t start, int64_t span, int64_t lo, int64_t hi,
                              int32_t y1, int32_t y2, int64_t fall, int64_t p,
                              int64_t scan, int64_t phase, int32_t h)
{
    int64_t delay = 0;
    while (delay < 2 * p && orb_geom_te_meets_window(start + delay, start + delay + span,
            lo, hi, y1, y2, fall, p, scan, phase, h)) delay += 250;
    return delay;
}

static void check_te_delay(int64_t start, int64_t span, int64_t lo, int64_t hi,
                           int32_t y1, int32_t y2, int64_t fall, int64_t p,
                           int64_t scan, int64_t phase, int32_t h)
{
    int64_t actual = orb_geom_te_delay_window(start, span, lo, hi, y1, y2, fall, p, scan, phase, h);
    CHECK(actual == te_delay_oracle(start, span, lo, hi, y1, y2, fall, p, scan, phase, h));
    CHECK(actual >= 0 && actual % 250 == 0);
    if (actual < 2 * p) {
        CHECK(!orb_geom_te_meets_window(start + actual, start + actual + span,
            lo, hi, y1, y2, fall, p, scan, phase, h));
    }
}

static void test_te_delay_window(void)
{
    const int64_t fall = 100000, period = 16900, scan = 16000;
    const int64_t ta = fall + scan * 100 / D;
    /* Earliest/latest starts and phase endpoints can each be safe while an
     * interior point tears. The helper must reject that whole start window. */
    CHECK(orb_geom_te_delay_window(ta - 33, 100, 20, 20, 100, 101,
        fall, period, scan, 0, D) > 0);
    CHECK(orb_geom_te_delay_window(ta + 1, 0, 20, 20, 100, 101,
        fall, period, scan, 100, D) > 0);
    for (int offset = -600; offset <= 600; offset++) {
        check_te_delay(ta + offset, 500, 1, 100, 100, 101, fall, period, scan, 150, D);
        check_te_delay(ta + offset, 0, 20, 20, 100, 101, fall, period, scan, 0, D);
    }
    for (unsigned i = 0; i < 100000; i++) {
        int64_t p = 8000 + random_u32() % 32001;
        int64_t duration = p - random_u32() % 1000;
        int64_t start = fall + (int64_t)(random_u32() % (4 * p)) - p;
        int64_t span = random_u32() % (2 * p + 1);
        int64_t lo = random_u32() % 40001, hi = lo + random_u32() % 8001;
        int64_t phase = random_u32() % 1001;
        int32_t y1 = random_u32() % (D + 1), y2 = y1 + random_u32() % (D - y1 + 1);
        check_te_delay(start, span, lo, hi, y1, y2, fall, p, duration, phase, D);
    }
    CHECK(orb_geom_te_delay_window(0, -1, 0, 1, 0, D, 0, period, scan, 0, D) == -1);
    CHECK(orb_geom_te_delay_window(0, 0, -1, 1, 0, D, 0, period, scan, 0, D) == -1);
    CHECK(orb_geom_te_delay_window(0, 0, 2, 1, 0, D, 0, period, scan, 0, D) == -1);
    CHECK(orb_geom_te_delay_window(0, 0, 0, 1, 0, D, 0, period, scan, -1, D) == -1);
    CHECK(orb_geom_te_delay_window(0, 0, 0, 1, 0, D, 0, 0, scan, 0, D) == -1);
    CHECK(orb_geom_te_delay_window(0, 0, 0, 1, 0, D, 0, period, -1, 0, D) == -1);
    CHECK(orb_geom_te_delay_window(0, 0, 0, 1, 0, D, 0, period, scan, 0, 0) == -1);
    CHECK(orb_geom_te_delay_window(0, 0, 0, 1, -1, D, 0, period, scan, 0, D) == -1);
    CHECK(orb_geom_te_delay_window(0, 0, 0, 1, 1, 0, 0, period, scan, 0, D) == -1);
    CHECK(orb_geom_te_delay_window(0, 0, 0, 1, 0, D + 1, 0, period, scan, 0, D) == -1);
}

int main(void)
{
    orb_geom_half_chords(s_half, D);
    test_chords();
    const unsigned rows[] = { 84, 104 };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        s_buf_px = D * rows[i];
        check_tiling(NULL);
        check_tiling(guard);
        test_guard();
        test_small_area();
    }
    test_te();
    test_te_window();
    test_te_delay_window();
    if (s_failed) {
        fprintf(stderr, "%d check(s) failed\n", s_failed);
        return 1;
    }
    printf("orb_display geometry: %llu checks passed\n", (unsigned long long)s_checks);
    return 0;
}
