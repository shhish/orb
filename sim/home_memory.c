/* Home-only CLIB provider. Link this object before the LVGL archive so its
 * uninstrumented provider is not extracted. Counts requested LVGL payload and
 * allocation calls; malloc headers, external font memory and board headroom
 * are excluded. No board memory budget is imposed on the desktop allocator. */
#include "lvgl.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>


typedef union {
    max_align_t alignment;
    size_t bytes;
} allocation_t;

static atomic_size_t s_live;
static atomic_size_t s_peak;
static atomic_size_t s_blocks;
static atomic_size_t s_allocations;

size_t home_memory_allocations(void)
{
    return atomic_load_explicit(&s_allocations, memory_order_relaxed);
}

static void add_bytes(size_t bytes)
{
    size_t used = atomic_fetch_add_explicit(&s_live, bytes, memory_order_relaxed) + bytes;
    size_t previous = atomic_load_explicit(&s_peak, memory_order_relaxed);
    while (used > previous && !atomic_compare_exchange_weak_explicit(
        &s_peak, &previous, used, memory_order_relaxed, memory_order_relaxed)) {}
}

void lv_mem_init(void)
{
    /* CLIB has no pool lifecycle. In particular, never erase a live charge. */
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *memory, size_t bytes)
{
    (void)memory;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    if (size > SIZE_MAX - sizeof(allocation_t)) return NULL;
    atomic_fetch_add_explicit(&s_allocations, 1, memory_order_relaxed);
    allocation_t *allocation = malloc(sizeof(*allocation) + size);
    if (!allocation) return NULL;
    allocation->bytes = size;
    add_bytes(size);
    atomic_fetch_add_explicit(&s_blocks, 1, memory_order_relaxed);
    return allocation + 1;
}

void lv_free_core(void *pointer)
{
    if (!pointer) return;
    allocation_t *allocation = (allocation_t *)pointer - 1;
    atomic_fetch_sub_explicit(&s_live, allocation->bytes, memory_order_relaxed);
    atomic_fetch_sub_explicit(&s_blocks, 1, memory_order_relaxed);
    free(allocation);
}

void *lv_realloc_core(void *pointer, size_t size)
{
    if (!pointer) return lv_malloc_core(size);
    if (!size) {
        lv_free_core(pointer);
        return NULL;
    }
    if (size > SIZE_MAX - sizeof(allocation_t)) return NULL;
    allocation_t *allocation = (allocation_t *)pointer - 1;
    size_t previous = allocation->bytes;
    atomic_fetch_add_explicit(&s_allocations, 1, memory_order_relaxed);
    allocation_t *resized = realloc(allocation, sizeof(*allocation) + size);
    if (!resized) return NULL;
    resized->bytes = size;
    if (size > previous) add_bytes(size - previous);
    else atomic_fetch_sub_explicit(&s_live, previous - size, memory_order_relaxed);
    return resized + 1;
}

void lv_mem_monitor_core(lv_mem_monitor_t *monitor)
{
    if (!monitor) return;
    memset(monitor, 0, sizeof(*monitor));
    /* total_size is tracked requested payload, not a finite desktop heap.
     * Free space and fragmentation are unavailable with CLIB; leave them zero
     * rather than presenting process memory as LVGL's free pool. Header bytes,
     * malloc overhead, fonts allocated outside LVGL, and SDL are excluded. */
    monitor->total_size = atomic_load_explicit(&s_live, memory_order_relaxed);
    monitor->max_used = atomic_load_explicit(&s_peak, memory_order_relaxed);
    if (monitor->max_used < monitor->total_size) monitor->max_used = monitor->total_size;
    monitor->used_cnt = atomic_load_explicit(&s_blocks, memory_order_relaxed);
    monitor->used_pct = monitor->total_size ? 100 : 0;
}

lv_result_t lv_mem_test_core(void)
{
    /* CLIB does not expose a heap-integrity walk. */
    return LV_RESULT_OK;
}
