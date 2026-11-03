#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Payload ownership is exclusive. Explicit raster allocations must not also be
 * charged to LVGL; allocator headers/fragmentation remain operating margin. */
typedef enum {
    ORB_MEMORY_GLASS,
    ORB_MEMORY_MASKS,
    ORB_MEMORY_RASTER,
    ORB_MEMORY_PAGE,
    ORB_MEMORY_MIPS,
    ORB_MEMORY_ROW_FONTS,
    ORB_MEMORY_NAV,
    ORB_MEMORY_FONTS,
    ORB_MEMORY_LVGL,
    ORB_MEMORY_GAMES,
    ORB_MEMORY_NETWORK,
    ORB_MEMORY_MODEL,
    ORB_MEMORY_WORKSPACE,
    /* Short-lived transparent raster preparation, separate from retained caches. */
    ORB_MEMORY_RASTER_BUILD,
    ORB_MEMORY_COUNT,
} orb_memory_owner_t;

typedef struct {
    size_t used, peak, budget;
    unsigned rejections;
} orb_memory_stats_t;

const char *orb_memory_name(orb_memory_owner_t owner);
void orb_memory_get(orb_memory_owner_t owner, orb_memory_stats_t *stats);
/* Atomic, allocation-free admission. Release exactly the admitted payload. */
bool orb_memory_reserve(orb_memory_owner_t owner, size_t bytes);
void orb_memory_release(orb_memory_owner_t owner, size_t bytes);
/* Add an exact successful legacy allocation to a non-cache owner's telemetry
 * without admission. This preserves unbudgeted host allocation semantics;
 * tracked bytes do not consume admission headroom. Device callers still reserve
 * before allocation. Pair with untrack(), never reserve()/release() for the
 * same payload. Invalid/cache owners are ignored. */
void orb_memory_track(orb_memory_owner_t owner, size_t bytes);
void orb_memory_untrack(orb_memory_owner_t owner, size_t bytes);
/* Existing caches own their exact allocation metadata and LRU. Publishing their
 * live payload keeps accounting lightweight; their local budget is authoritative. */
void orb_memory_observe(orb_memory_owner_t owner, size_t bytes);

/* Optional caches share 1 MiB and require the admission margin to remain free.
 * This is not a reservation: subsequent required allocations can consume it.
 * Registration/admission run under the UI lock, including callbacks. An oldest
 * callback returns UINT64_MAX when every entry is pinned. An eviction callback
 * releases exactly one eligible entry and calls orb_memory_release(). */
typedef uint64_t (*orb_memory_cache_oldest_t)(void *context);
typedef bool (*orb_memory_cache_evict_t)(void *context);
void orb_memory_cache_register(orb_memory_owner_t owner, orb_memory_cache_oldest_t oldest,
                               orb_memory_cache_evict_t evict, void *context);
bool orb_memory_cache_admit(orb_memory_owner_t owner, size_t bytes);
/* Evicts the oldest unpinned entries across all caches until at least bytes
 * are released or nothing is left; returns the bytes released. For one-off
 * large allocations (a screenshot) that cache fragmentation can block. */
size_t orb_memory_cache_trim(size_t bytes);
size_t orb_memory_cache_used(void);
size_t orb_memory_cache_budget(void);
/* Actual free-PSRAM admission floor, also used by diagnostics. */
size_t orb_memory_cache_margin(void);
/* Assets publishes the current flash-only bitmap payload under its own lock.
 * PSRAM migration removes that credit. This API never allocates or evicts. */
void orb_memory_cache_set_flash_backing(size_t bytes);
/* Worker-safe monotonic shared LRU stamp; never invokes eviction callbacks. */
uint64_t orb_memory_cache_touch(void);

/* ESP allocates only PSRAM, never silently consumes the internal/DMA reserve.
 * Results preserve max_align_t alignment. realloc failure preserves the input. */
void *orb_memory_alloc(orb_memory_owner_t owner, size_t bytes);
void *orb_memory_calloc(orb_memory_owner_t owner, size_t count, size_t bytes);
void *orb_memory_realloc(orb_memory_owner_t owner, void *pointer, size_t bytes);
size_t orb_memory_size(const void *pointer);
void orb_memory_free(void *pointer);

