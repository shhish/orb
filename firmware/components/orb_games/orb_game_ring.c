#include "orb_game_orbit.h"
#include "src/draw/sw/lv_draw_sw_mask_private.h"

#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#endif

typedef orb_game_orbit_ring_row_t ring_row_t;
typedef orb_game_orbit_ring_t ring_cache_t;
typedef struct {
    ring_row_t rows[234];
    lv_opa_t line[234];
} ring_scratch_t;

static void *ring_allocate(size_t bytes)
{
#ifdef ESP_PLATFORM
    return orb_memory_alloc(ORB_MEMORY_GAMES, bytes);
#else
    return malloc(bytes);
#endif
}

static void ring_release(void *pointer)
{
#ifdef ESP_PLATFORM
    orb_memory_free(pointer);
#else
    free(pointer);
#endif
}

orb_game_orbit_ring_t *orb_game_ring_prepare(unsigned radius, unsigned width, uint32_t *bytes)
{
    /* Native BORDER masks have only short AA edges around a flat opaque run.
     * Keep one quadrant, including its centre axes, instead of a 400×400 A8
     * bitmap. Build once at entry; playback allocates no masks or draw tasks. */
    if (radius > 233 || width >= radius) return NULL;
    /* These cold mask rows are transient games-owned PSRAM, with no internal
     * or stack fallback. Budget/OOM refusal keeps the existing generic renderer. */
    ring_scratch_t *scratch = ring_allocate(sizeof(*scratch));
    if (!scratch) return NULL;
    lv_area_t outer = {0, 0, (int32_t)radius * 2, (int32_t)radius * 2};
    lv_area_t inner = {(int32_t)width, (int32_t)width,
        (int32_t)(radius * 2 - width), (int32_t)(radius * 2 - width)};
    lv_draw_sw_mask_radius_param_t outer_mask, inner_mask;
    if (lv_draw_sw_mask_radius_init(&inner_mask, &inner, (int32_t)(radius - width), true) != LV_RESULT_OK) {
        ring_release(scratch);
        return NULL;
    }
    if (lv_draw_sw_mask_radius_init(&outer_mask, &outer, (int32_t)radius, false) != LV_RESULT_OK) {
        lv_draw_sw_mask_free_param(&inner_mask);
        ring_release(scratch);
        return NULL;
    }
    void *masks[] = {&inner_mask, &outer_mask, NULL};
    ring_row_t *rows = scratch->rows;
    lv_opa_t *line = scratch->line;
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
    ring_cache_t *cache = ring_allocate(total);
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
    ring_release(scratch);
    return cache;
}

