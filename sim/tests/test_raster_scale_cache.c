/* Exact scaled variants must match the uncached software filter, including
 * partial bands, opacity, source invalidation and cache eviction. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "sim_display.h"

#define D 466
static uint32_t s_now;
static uint8_t s_reference[D * D * 3], s_candidate[D * D * 3];
static unsigned s_checks;
static bool s_fail_alloc;
static void *cache_malloc(size_t bytes, lv_color_format_t cf)
{
    LV_UNUSED(cf);
    return s_fail_alloc ? NULL : malloc(bytes);
}
static uint32_t tick(void) { return s_now; }
#define CHECK(c) do { s_checks++; if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static double wall_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}

static void render(lv_display_t *display, lv_obj_t *screen)
{
    lv_obj_invalidate(screen);
    lv_refr_now(display);
}

static void redraw(lv_display_t *display, lv_obj_t *screen, uint8_t *rgb)
{
    render(display, screen);
    sim_display_rgb(rgb);
}

static void compare(lv_display_t *display, lv_obj_t *screen)
{
    lv_obj_raster_scale_cache_set_enabled(false);
    redraw(display, screen, s_reference);
    lv_obj_raster_scale_cache_set_enabled(true);
    redraw(display, screen, s_candidate);
    if (memcmp(s_reference, s_candidate, sizeof(s_reference))) {
        unsigned differences = 0;
        for (size_t i = 0; i < sizeof(s_reference); i++) differences += s_reference[i] != s_candidate[i];
        fprintf(stderr, "scaled cache: %u differing channels\n", differences);
        exit(1);
    }
    CHECK(true);
    lv_obj_raster_scale_cache_stats_t stats;
    lv_obj_raster_scale_cache_get_stats(&stats);
    CHECK(stats.bytes <= stats.budget);
}

int main(int argc, char **argv)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_obj_raster_set_allocator(cache_malloc, free);
    lv_display_t *display = sim_display_create();
    if (argc > 1 && !strcmp(argv[1], "--native")) {
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x17203A), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 66, 170);
    lv_obj_set_size(row, 332, 85);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xA1420C), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 17, 0);
    for (int i = 0; i < 14; i++) {
        lv_obj_t *stripe = lv_obj_create(row);
        lv_obj_remove_style_all(stripe);
        lv_obj_set_pos(stripe, 10 + i * 22, 9 + i % 3 * 7);
        lv_obj_set_size(stripe, 15, 50);
        lv_obj_set_style_bg_color(stripe, lv_color_hex(0x123456u * (i + 1)), 0);
        lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(stripe, 5, 0);
    }
    lv_obj_set_raster_matte(row, lv_color_black());
    lv_obj_raster_prepare(screen);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    for (unsigned scale = 1; scale <= LV_SCALE_NONE; scale++) {
        lv_obj_set_raster_scale(row, scale);
        lv_obj_set_raster_opa(row, (lv_opa_t)(scale % 3 ? 255 : 143));
        compare(display, screen);
    }
    lv_obj_set_raster_scale(row, 0);
    compare(display, screen);
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds);

    /*Translation and opacity are not cache keys; every settled refresh should
     *reuse its exact resample, including clipped first/last band pieces.*/
    lv_obj_set_raster_scale(row, 193);
    lv_obj_raster_scale_cache_set_enabled(true);
    redraw(display, screen, s_candidate);
    lv_obj_raster_scale_cache_stats_t warm, now;
    lv_obj_raster_scale_cache_get_stats(&warm);
    for (int y = -30; y < 470; y += 11) {
        lv_obj_move_to(row, 66, y);
        lv_obj_set_raster_opa(row, (lv_opa_t)(127 + y % 127));
        compare(display, screen);
    }
    lv_obj_raster_scale_cache_get_stats(&now);
    printf("cache stats warm h%u b%u blit%u now h%u b%u blit%u evict%u bytes%u budget%u\n", warm.hits, warm.builds, warm.blits, now.hits, now.builds, now.blits, now.evictions, now.bytes, now.budget);
    CHECK(now.builds == warm.builds);
    if (now.budget) CHECK(now.hits > warm.hits && now.blits > warm.blits && now.evictions > 0);

    lv_obj_move_to(row, 66, 170);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x108392), 0);
    compare(display, screen);
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds + 1);

    /*Odd-width sources currently split into two opaque-plane parts and use the
     *original renderer. Resizing must purge variants rather than reusing their
     *old shape, then the even full-plane shape becomes cacheable again.*/
    lv_obj_set_width(row, 333);
    compare(display, screen);
    lv_obj_set_width(row, 332);
    compare(display, screen);

    /*Allocation failure must preserve the generic path and release every
     *reservation; a later refresh retries the same exact scale.*/
    lv_obj_raster_scale_cache_get_stats(&warm);
    lv_obj_set_raster_scale(row, 201);
    s_fail_alloc = true;
    compare(display, screen);
    s_fail_alloc = false;
    lv_obj_raster_scale_cache_get_stats(&now);
    if (now.budget) CHECK(now.alloc_fail > warm.alloc_fail);
    compare(display, screen);

    /*Timing is a reported host ratio, never an S3 frame-rate assertion.*/
    lv_obj_set_raster_opa(row, LV_OPA_COVER);
    const unsigned frames = 60;
    lv_obj_raster_scale_cache_set_enabled(false);
    double start = wall_ms();
    for (unsigned i = 0; i < frames; i++) render(display, screen);
    double control = wall_ms() - start;
    sim_display_rgb(s_reference);
    lv_obj_raster_scale_cache_set_enabled(true);
    redraw(display, screen, s_candidate);
    start = wall_ms();
    for (unsigned i = 0; i < frames; i++) render(display, screen);
    double cached = wall_ms() - start;
    sim_display_rgb(s_candidate);
    CHECK(!memcmp(s_reference, s_candidate, sizeof(s_reference)));
    printf("scale cache timing: control %.3f ms/frame cached %.3f ms/frame ratio %.3f (host only)\n",
           control / frames, cached / frames, cached / control);
    /*More simultaneously visible full-size variants than the budget admits:
     *selected sources must stay pinned until all their queued bands finish.
     *The remainder falls back, and a repeated refresh must not LRU-thrash the
     *same set of already-selected owners.*/
    lv_obj_t *owners[16];
    for (unsigned i = 0; i < 16; i++) {
        owners[i] = lv_obj_create(screen);
        lv_obj_remove_style_all(owners[i]);
        lv_obj_set_pos(owners[i], 66, 170);
        lv_obj_set_size(owners[i], 332, 85);
        lv_obj_set_style_bg_color(owners[i], lv_color_hex(0x315479u * (i + 1)), 0);
        lv_obj_set_style_bg_opa(owners[i], LV_OPA_COVER, 0);
        lv_obj_set_raster_matte(owners[i], lv_color_black());
        lv_obj_set_raster_scale(owners[i], 255);
    }
    lv_obj_raster_prepare(screen);
    compare(display, screen);
    lv_obj_raster_scale_cache_get_stats(&warm);
    compare(display, screen);
    lv_obj_raster_scale_cache_get_stats(&now);
    printf("pin stats warm b%u h%u m%u bytes%u now b%u h%u m%u bytes%u\n", warm.builds, warm.hits, warm.misses, warm.bytes, now.builds, now.hits, now.misses, now.bytes);
    CHECK(now.builds == warm.builds);
    for (unsigned i = 0; i < 16; i++) lv_obj_delete(owners[i]);
    lv_obj_delete(row);
    lv_obj_raster_scale_cache_get_stats(&now);
    CHECK(now.bytes == 0);
    lv_display_delete(display);
    lv_deinit();
    printf("scale cache: %u exact pixel/budget/lifetime checks passed\n", s_checks);
    return 0;
}
