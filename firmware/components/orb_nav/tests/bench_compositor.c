/* Host model: immutable 466x466 source planes in a large, streaming working
 * set and one reused 466x84 destination band. This models access shape, not
 * ESP32-S3 PSRAM latency or the PIE kernel's instruction throughput. */
#ifdef memcpy
#undef memcpy
#endif
#include "orb_nav_compositor.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SIDE = 466, ROWS = 84, SCENES = 128, REPEATS = 5, ITERATIONS = 512 };
static uint64_t s_tiny_calls, s_span_calls, s_checksum;

void *orb_nav_bench_memcpy(void *destination, const void *source, size_t bytes)
{
    if (bytes <= 4) s_tiny_calls++;
    return memcpy(destination, source, bytes);
}

/* An ordinary host memcpy stands in for the S3's contiguous SIMD transfer.
 * Keep it in a separate function, as the production compositor calls it. */
void orb_simd_copy_rgb565(void *destination, const void *source, size_t pixels)
{
#ifdef ORB_BENCH_COUNT
    s_span_calls++;
#endif
    memcpy(destination, source, pixels * 2);
}

static double seconds(void)
{
    struct timespec time;
    assert(!clock_gettime(CLOCK_MONOTONIC, &time));
    return time.tv_sec + time.tv_nsec / 1000000000.;
}

static int compare(const void *left, const void *right)
{
    double a = *(const double *)left, b = *(const double *)right;
    return a > b ? 1 : a < b ? -1 : 0;
}

static void reverse_prefix_benchmark(orb_nav_compositor_t **scenes, const char *name)
{
    const orb_nav_token_rect_t token = {197, 197, 72, 72};
    for (unsigned scene = 0; scene < SCENES; scene++) {
        orb_nav_compositor_cancel(scenes[scene]);
        assert(orb_nav_compositor_begin(scenes[scene], ORB_NAV_TRANSITION_IRIS, 0, 1,
            ORB_NAV_DIRECTION_RIGHT, &token, true, false));
    }
    double times[REPEATS];
    for (unsigned repeat = 0; repeat < REPEATS; repeat++) {
        double start = seconds();
        for (unsigned iteration = 0; iteration < ITERATIONS; iteration++)
            s_checksum += (unsigned)(orb_nav_compositor_reverse_iris_round_start(scenes[iteration % SCENES]) * 65536);
        times[repeat] = (seconds() - start) * 1000000. / ITERATIONS;
    }
    qsort(times, REPEATS, sizeof(times[0]), compare);
    printf("{\"name\":\"%s\",\"median_us\":%.3f,\"min_us\":%.3f,\"max_us\":%.3f}",
        name, times[REPEATS / 2], times[0], times[REPEATS - 1]);
}

int main(void)
{
    orb_nav_compositor_t *scenes[SCENES];
    uint32_t random = 17;
    for (unsigned scene = 0; scene < SCENES; scene++) {
        scenes[scene] = orb_nav_compositor_create(SIDE, SIDE);
        assert(scenes[scene]);
        for (unsigned slot = 0; slot < 2; slot++) {
            assert(orb_nav_compositor_capture_begin(scenes[scene], slot, 1));
            uint16_t *pixels = orb_nav_compositor_pixels(scenes[scene], slot);
            for (unsigned i = 0; i < SIDE * SIDE; i++) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                pixels[i] = (uint16_t)random;
            }
            assert(orb_nav_compositor_capture_complete(scenes[scene], slot));
        }
    }
    static const struct {
        const char *name;
        orb_nav_transition_kind_t kind;
        orb_nav_direction_t direction;
        bool reduce_motion;
    } cases[] = {
        {"slide-left", ORB_NAV_TRANSITION_SLIDE, ORB_NAV_DIRECTION_LEFT, false},
        {"slide-right", ORB_NAV_TRANSITION_SLIDE, ORB_NAV_DIRECTION_RIGHT, false},
        {"stack-up", ORB_NAV_TRANSITION_SHEET, ORB_NAV_DIRECTION_UP, false},
        {"shade-down", ORB_NAV_TRANSITION_SHEET, ORB_NAV_DIRECTION_DOWN, false},
        {"back", ORB_NAV_TRANSITION_BACK, ORB_NAV_DIRECTION_RIGHT, false},
        {"iris", ORB_NAV_TRANSITION_IRIS, ORB_NAV_DIRECTION_LEFT, false},
        {"fade-rm", ORB_NAV_TRANSITION_BACK, ORB_NAV_DIRECTION_RIGHT, true},
    };
    const orb_nav_token_rect_t token = {197, 197, 72, 72};
    uint16_t output[SIDE * ROWS] __attribute__((aligned(16)));
    printf("{\"source_bytes\":%zu,\"band_pixels\":%u,\"scenes\":%u,\"iterations\":%u,\"cases\":[",
        (size_t)SCENES * SIDE * SIDE * 2 * ORB_NAV_SNAPSHOT_SLOTS, SIDE * ROWS, SCENES, ITERATIONS);
    for (unsigned sample = 0; sample < sizeof(cases) / sizeof(cases[0]); sample++) {
        for (unsigned scene = 0; scene < SCENES; scene++) {
            assert(orb_nav_compositor_begin(scenes[scene], cases[sample].kind, 0, 1,
                cases[sample].direction, &token, false, cases[sample].reduce_motion));
            orb_nav_compositor_drag(scenes[scene], .5f);
        }
        double times[REPEATS];
        for (unsigned repeat = 0; repeat < REPEATS; repeat++) {
            double start = seconds();
            for (unsigned iteration = 0; iteration < ITERATIONS; iteration++) {
                unsigned scene = iteration % SCENES;
                int y = ((iteration / SCENES + repeat + scene) % 5) * ROWS;
                assert(orb_nav_compositor_render_band(scenes[scene], 0, y, SIDE, ROWS,
                    output, SIDE * 2, true));
                s_checksum += output[(iteration * 139) % (SIDE * ROWS)];
            }
            times[repeat] = (seconds() - start) * 1000000. / ITERATIONS;
        }
        qsort(times, REPEATS, sizeof(times[0]), compare);
        s_tiny_calls = s_span_calls = 0;
        assert(orb_nav_compositor_render_band(scenes[0], 0, 168, SIDE, ROWS,
            output, SIDE * 2, true));
        printf("%s{\"name\":\"%s\",\"median_us\":%.3f,\"min_us\":%.3f,\"max_us\":%.3f,"
            "\"tiny_memcpy_calls\":%" PRIu64 ",\"span_calls\":%" PRIu64 "}",
            sample ? "," : "", cases[sample].name, times[REPEATS / 2], times[0], times[REPEATS - 1],
            s_tiny_calls, s_span_calls);
    }
    printf("],\"reverse_prefix_admission\":[");
    reverse_prefix_benchmark(scenes, "dense");
    /* Mostly black synthetic clock/content bars model equality-heavy UI;
     * random planes above bound the differing-pixel geometry work. */
    for (unsigned scene = 0; scene < SCENES; scene++) {
        orb_nav_compositor_cancel(scenes[scene]);
        for (unsigned slot = 0; slot < 2; slot++) {
            assert(orb_nav_compositor_capture_begin(scenes[scene], slot, 2));
            uint16_t *pixels = orb_nav_compositor_pixels(scenes[scene], slot);
            for (unsigned y = 0; y < SIDE; y++) for (unsigned x = 0; x < SIDE; x++) {
                bool ink = x >= 100 && x < 366 && ((y >= 80 && y < 110) || (y >= 180 && y < 250));
                pixels[y * SIDE + x] = ink ? (slot ? 0xFFE0 : 0xFFFF) : 0;
            }
            assert(orb_nav_compositor_capture_complete(scenes[scene], slot));
        }
    }
    printf(","); reverse_prefix_benchmark(scenes, "sparse");
    printf("],\"checksum\":%" PRIu64 "}\n", s_checksum);
    for (unsigned scene = 0; scene < SCENES; scene++) orb_nav_compositor_destroy(scenes[scene]);
    return 0;
}
