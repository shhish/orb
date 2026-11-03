/* ORB PATCH: capability-explicit temporary drawing memory, independent of the
 * generic LVGL heap. Hosts retain lv_malloc/free. */
#include "../lvgl_public.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifdef ESP_PLATFORM

#include <stdatomic.h>
#include <stddef.h>
#include "esp_heap_caps.h"

#define DRAW_SCRATCH_BUDGET (16U * 1024U)
#define DRAW_SCRATCH_FREE_RESERVE (24U * 1024U)
#define DRAW_SCRATCH_DMA_RESERVE (8U * 1024U)
#define DRAW_SCRATCH_HEAP_SLOP 64U
#define DRAW_SCRATCH_INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define DRAW_SCRATCH_DMA_CAPS (DRAW_SCRATCH_INTERNAL_CAPS | MALLOC_CAP_DMA)

/* Header alignment preserves malloc's contract for all scratch element types.
 * A charge includes conservative heap overhead, so two workers share one cap. */
typedef union {
    max_align_t alignment;
    size_t internal_charge;
} draw_scratch_header_t;

static atomic_size_t s_internal_in_use;
static atomic_size_t s_peak_internal;
static atomic_uint s_internal_allocations[LV_DRAW_SCRATCH_KIND_COUNT];
static atomic_uint s_psram_allocations[LV_DRAW_SCRATCH_KIND_COUNT];
static atomic_uint s_failures[LV_DRAW_SCRATCH_KIND_COUNT];

static bool scratch_enabled(lv_draw_scratch_kind_t kind)
{
    switch(kind) {
#if CONFIG_ORB_DISPLAY_INTERNAL_IMAGE_SCRATCH
        case LV_DRAW_SCRATCH_IMAGE: return true;
#endif
#if CONFIG_ORB_DISPLAY_INTERNAL_BLUR_SCRATCH
        case LV_DRAW_SCRATCH_BLUR: return true;
#endif
#if CONFIG_ORB_DISPLAY_INTERNAL_DRAW_MASKS
        case LV_DRAW_SCRATCH_MASK: return true;
#endif
        default: return false;
    }
}

static bool scratch_claim(size_t charge)
{
    size_t used = atomic_load_explicit(&s_internal_in_use, memory_order_relaxed);
    do {
        if(charge > DRAW_SCRATCH_BUDGET - used) return false;
    } while(!atomic_compare_exchange_weak_explicit(&s_internal_in_use, &used, used + charge,
                                                  memory_order_relaxed, memory_order_relaxed));
    return true;
}

void * lv_malloc_draw(size_t size, lv_draw_scratch_kind_t kind)
{
    if((unsigned)kind >= LV_DRAW_SCRATCH_KIND_COUNT || size == 0) return lv_malloc(size);
    if(size > SIZE_MAX - sizeof(draw_scratch_header_t) - DRAW_SCRATCH_HEAP_SLOP) {
        atomic_fetch_add_explicit(&s_failures[kind], 1, memory_order_relaxed);
        return NULL;
    }

    size_t bytes = size + sizeof(draw_scratch_header_t);
    size_t charge = bytes + DRAW_SCRATCH_HEAP_SLOP;
    draw_scratch_header_t * header = NULL;
    if(scratch_enabled(kind) && scratch_claim(charge)) {
        /* Free and DMA views overlap: check both, never add them. Recheck after
         * allocation to reject a race/fragmentation loss before drawing. This
         * is an admission check, not a reservation against later radio work. */
        if(heap_caps_get_free_size(DRAW_SCRATCH_INTERNAL_CAPS) >= charge + DRAW_SCRATCH_FREE_RESERVE &&
           heap_caps_get_free_size(DRAW_SCRATCH_DMA_CAPS) >= charge + DRAW_SCRATCH_FREE_RESERVE) {
            header = heap_caps_aligned_alloc(_Alignof(draw_scratch_header_t), bytes, DRAW_SCRATCH_INTERNAL_CAPS);
            if(header && (heap_caps_get_free_size(DRAW_SCRATCH_INTERNAL_CAPS) < DRAW_SCRATCH_FREE_RESERVE ||
                          heap_caps_get_free_size(DRAW_SCRATCH_DMA_CAPS) < DRAW_SCRATCH_FREE_RESERVE ||
                          heap_caps_get_largest_free_block(DRAW_SCRATCH_DMA_CAPS) < DRAW_SCRATCH_DMA_RESERVE)) {
                heap_caps_free(header);
                header = NULL;
            }
        }
        if(header) {
            header->internal_charge = charge;
            atomic_fetch_add_explicit(&s_internal_allocations[kind], 1, memory_order_relaxed);
            size_t used = atomic_load_explicit(&s_internal_in_use, memory_order_relaxed);
            size_t peak = atomic_load_explicit(&s_peak_internal, memory_order_relaxed);
            while(peak < used && !atomic_compare_exchange_weak_explicit(&s_peak_internal, &peak, used,
                    memory_order_relaxed, memory_order_relaxed)) {}
            return header + 1;
        }
        atomic_fetch_sub_explicit(&s_internal_in_use, charge, memory_order_relaxed);
    }

    /* This also covers kinds whose internal experiment is disabled. Generic
     * malloc would place their small per-band masks in internal/DMA RAM and
     * could fall back there for larger requests when PSRAM is exhausted. Keep
     * the zero-internal policy explicit instead of bypassing the shared cap. */
    header = heap_caps_aligned_alloc(_Alignof(draw_scratch_header_t), bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!header) {
        atomic_fetch_add_explicit(&s_failures[kind], 1, memory_order_relaxed);
        return NULL;
    }
    header->internal_charge = 0;
    atomic_fetch_add_explicit(&s_psram_allocations[kind], 1, memory_order_relaxed);
    return header + 1;
}

void lv_free_draw(void * data, lv_draw_scratch_kind_t kind)
{
    if((unsigned)kind >= LV_DRAW_SCRATCH_KIND_COUNT) {
        lv_free(data);
        return;
    }
    if(!data) return;
    /* lv_malloc(0) returns LVGL's shared zero-size sentinel. */
    if(data == lv_malloc(0)) return;
    draw_scratch_header_t * header = (draw_scratch_header_t *)data - 1;
    size_t charge = header->internal_charge;
    heap_caps_free(header);
    if(charge) atomic_fetch_sub_explicit(&s_internal_in_use, charge, memory_order_relaxed);
}

void lv_draw_scratch_get_stats(lv_draw_scratch_stats_t * stats)
{
    if(!stats) return;
    stats->internal_in_use = atomic_load_explicit(&s_internal_in_use, memory_order_relaxed);
    stats->peak_internal = atomic_load_explicit(&s_peak_internal, memory_order_relaxed);
    for(unsigned i = 0; i < LV_DRAW_SCRATCH_KIND_COUNT; i++) {
        stats->internal_allocations[i] = atomic_load_explicit(&s_internal_allocations[i], memory_order_relaxed);
        stats->psram_allocations[i] = atomic_load_explicit(&s_psram_allocations[i], memory_order_relaxed);
        stats->failures[i] = atomic_load_explicit(&s_failures[i], memory_order_relaxed);
    }
}

#else

void * lv_malloc_draw(size_t size, lv_draw_scratch_kind_t kind)
{
    LV_UNUSED(kind);
    return lv_malloc(size);
}

void lv_free_draw(void * data, lv_draw_scratch_kind_t kind)
{
    LV_UNUSED(kind);
    lv_free(data);
}

void lv_draw_scratch_get_stats(lv_draw_scratch_stats_t * stats)
{
    if(stats) lv_memzero(stats, sizeof(*stats));
}

#endif
