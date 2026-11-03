#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_game_orbit.h"
#include "src/draw/sw/lv_draw_sw_mask_private.h"

static unsigned s_checks, s_calls, s_fail_on, s_mask_calls, s_mask_fail_on, s_mask_frees;
static size_t s_live, s_compared;
static struct { void *pointer; size_t bytes; } s_blocks[8];
#define CHECK(value) do { ++s_checks; if (!(value)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); } } while (0)

static void *test_allocate(size_t size)
{
    if (++s_calls == s_fail_on) return NULL;
    void *pointer = malloc(size);
    CHECK(pointer);
    for (unsigned i = 0; i < 8; ++i) if (!s_blocks[i].pointer) {
        s_blocks[i].pointer = pointer; s_blocks[i].bytes = size; s_live += size;
        return pointer;
    }
    abort();
}
static void test_release(void *pointer)
{
    if (!pointer) return;
    for (unsigned i = 0; i < 8; ++i) if (s_blocks[i].pointer == pointer) {
        s_live -= s_blocks[i].bytes; s_blocks[i].pointer = NULL; free(pointer); return;
    }
    abort();
}
static lv_result_t test_mask_init(lv_draw_sw_mask_radius_param_t *param, const lv_area_t *area,
                                   int32_t radius, bool inverse)
{
    if (++s_mask_calls == s_mask_fail_on) return LV_RESULT_INVALID;
    return lv_draw_sw_mask_radius_init(param, area, radius, inverse);
}
static void test_mask_free(void *param) { ++s_mask_frees; lv_draw_sw_mask_free_param(param); }

#define malloc test_allocate
#define free test_release
#define lv_draw_sw_mask_radius_init test_mask_init
#define lv_draw_sw_mask_free_param test_mask_free
#include "../../firmware/components/orb_games/orb_game_ring.c"

static orb_game_orbit_ring_t *reference_game_ring_prepare(unsigned radius, unsigned width, uint32_t *bytes)
{
    /* Native BORDER masks have only short AA edges around a flat opaque run.
     * Keep one quadrant, including its centre axes, instead of a 400×400 A8
     * bitmap. Build once at entry; playback allocates no masks or draw tasks. */
    if (radius > 233 || width >= radius) return NULL;
    lv_area_t outer = {0, 0, (int32_t)radius * 2, (int32_t)radius * 2};
    lv_area_t inner = {(int32_t)width, (int32_t)width,
        (int32_t)(radius * 2 - width), (int32_t)(radius * 2 - width)};
    lv_draw_sw_mask_radius_param_t outer_mask, inner_mask;
    if (lv_draw_sw_mask_radius_init(&inner_mask, &inner, (int32_t)(radius - width), true) != LV_RESULT_OK)
        return NULL;
    if (lv_draw_sw_mask_radius_init(&outer_mask, &outer, (int32_t)radius, false) != LV_RESULT_OK) {
        lv_draw_sw_mask_free_param(&inner_mask);
        return NULL;
    }
    void *masks[] = {&inner_mask, &outer_mask, NULL};
    ring_row_t rows[234];
    lv_opa_t line[234];
    size_t alpha_bytes = 0;
    for (unsigned y = 0; y <= radius; ++y) {
        memset(line, 255, radius + 1);
        lv_draw_sw_mask_res_t result = lv_draw_sw_mask_apply(masks, line, 0, (int32_t)y, (int32_t)radius + 1);
        if (result == LV_DRAW_SW_MASK_RES_TRANSP) memset(line, 0, radius + 1);
        unsigned first = 0, last = radius;
        while (first <= radius && !line[first]) ++first;
        if (first > radius) { rows[y] = (ring_row_t){1, 0, 1, 0, 0}; continue; }
        while (last > first && !line[last]) --last;
        unsigned opaque_first = first, opaque_last = last;
        while (opaque_first <= last && line[opaque_first] != 255) ++opaque_first;
        if (opaque_first <= last) {
            while (opaque_last > opaque_first && line[opaque_last] != 255) --opaque_last;
        } else opaque_last = last;
        rows[y] = (ring_row_t){(uint8_t)first, (uint8_t)last,
            (uint8_t)opaque_first, (uint8_t)opaque_last, (uint16_t)alpha_bytes};
        alpha_bytes += opaque_first - first + last - opaque_last;
    }
    size_t total = sizeof(ring_cache_t) + (radius + 1) * sizeof(ring_row_t) + alpha_bytes;
#ifdef ESP_PLATFORM
    ring_cache_t *cache = orb_memory_alloc(ORB_MEMORY_GAMES, total);
#else
    ring_cache_t *cache = malloc(total);
#endif
    if (cache) {
        cache->radius = (uint16_t)radius;
        cache->width = (uint16_t)width;
        memcpy(cache->rows, rows, (radius + 1) * sizeof(ring_row_t));
        uint8_t *alpha = (uint8_t *)(cache->rows + radius + 1);
        for (unsigned y = 0; y <= radius; ++y) {
            const ring_row_t *row = &rows[y];
            if (row->first > row->last) continue;
            memset(line, 255, radius + 1);
            (void)lv_draw_sw_mask_apply(masks, line, 0, (int32_t)y, (int32_t)radius + 1);
            size_t before = row->opaque_first - row->first;
            size_t after = row->last - row->opaque_last;
            memcpy(alpha + row->alpha_offset, line + row->first, before);
            memcpy(alpha + row->alpha_offset + before, line + row->opaque_last + 1, after);
        }
        *bytes += (uint32_t)total;
    }
    lv_draw_sw_mask_free_param(&inner_mask);
    lv_draw_sw_mask_free_param(&outer_mask);
    return cache;
}


#undef malloc
#undef free
#undef lv_draw_sw_mask_radius_init
#undef lv_draw_sw_mask_free_param

static void compare(unsigned radius, unsigned width)
{
    uint32_t expected_bytes = 17, actual_bytes = 17;
    ring_cache_t *reference = reference_game_ring_prepare(radius, width, &expected_bytes);
    ring_cache_t *candidate = orb_game_ring_prepare(radius, width, &actual_bytes);
    CHECK(reference && candidate && expected_bytes == actual_bytes);
    CHECK(!memcmp(reference, candidate, expected_bytes - 17));
    s_compared += expected_bytes - 17;
    test_release(reference); test_release(candidate);
    CHECK(!s_live);
}
static void fail(unsigned allocation, unsigned mask, unsigned expected_calls, unsigned expected_frees)
{
    CHECK(!s_live);
    s_calls = s_mask_calls = s_mask_frees = 0;
    s_fail_on = allocation; s_mask_fail_on = mask;
    uint32_t bytes = 123;
    CHECK(!orb_game_ring_prepare(107, 2, &bytes));
    CHECK(bytes == 123 && !s_live && s_mask_calls == expected_calls && s_mask_frees == expected_frees);
    s_fail_on = s_mask_fail_on = 0;
}
int main(void)
{
    lv_init();
    CHECK(sizeof(ring_scratch_t) == 1638);
    for (unsigned radius = 1; radius <= 233; ++radius) {
        compare(radius, 0);
        compare(radius, radius - 1);
        if (radius > 2) compare(radius, 2);
    }
    fail(1, 0, 0, 0); /* Games scratch OOM: no masks created. */
    fail(2, 0, 2, 2); /* Retained cache OOM: masks and scratch retired. */
    fail(0, 1, 1, 0); /* Inner radius initialization refusal. */
    fail(0, 2, 2, 1); /* Outer refusal retires the inner mask. */
    uint32_t bytes = 23;
    CHECK(!orb_game_ring_prepare(234, 2, &bytes) && bytes == 23 && !s_live);
    CHECK(!orb_game_ring_prepare(2, 2, &bytes) && bytes == 23 && !s_live);
    printf("ring scratch: %u checks; %zu exact cache bytes compared\n", s_checks, s_compared);
    lv_deinit();
    return 0;
}
