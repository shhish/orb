#include "orb_memory.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif

typedef struct {
    atomic_size_t used, tracked, peak;
    atomic_uint rejections;
} owner_t;

typedef union {
    max_align_t alignment;
    struct { size_t bytes; orb_memory_owner_t owner; } value;
} allocation_t;

static owner_t s_owners[ORB_MEMORY_COUNT];
static atomic_size_t s_cache_used;
static atomic_size_t s_cache_flash_backing;
#ifdef ESP_PLATFORM
static uint64_t s_cache_clock;
static portMUX_TYPE s_clock_lock = portMUX_INITIALIZER_UNLOCKED;
#else
static atomic_uint_fast64_t s_cache_clock;
#endif
typedef struct {
    orb_memory_cache_oldest_t oldest;
    orb_memory_cache_evict_t evict;
    void *context;
} cache_t;
static cache_t s_caches[ORB_MEMORY_COUNT];
#define CACHE_BUDGET (1024u * 1024u)
/* Admission floor, not reserved memory: required allocations may consume it.
 * A screenshot needs 434,312 RGB565 bytes plus a 178,944-byte overlay band;
 * it trims optional caches on allocation failure. Workspace growth, TLS/OTA
 * and draw scratch also need measured headroom. Flash bitmap savings fund up
 * to 128 KiB more headroom, after a conservative metadata/header allowance.
 * Unmapped accessibility faces retain the old floor. Assets OTA removes the
 * credit when bitmaps migrate into PSRAM, or fails safely before erasing. */
#define PSRAM_MARGIN (640u * 1024u)
#define PSRAM_FLASH_HEADROOM (128u * 1024u)
#define PSRAM_FLASH_OVERHEAD (16u * 1024u)
static const char *const s_names[ORB_MEMORY_COUNT] = {
    "glass", "glass-masks", "raster", "page", "mips", "row-fonts", "nav",
    "fonts-assets", "lvgl-heap", "games", "network", "model", "workspace", "raster-build",
};
static const size_t s_budgets[ORB_MEMORY_COUNT] = {
    192 * 1024, 640 * 1024, 768 * 1024, 448 * 1024, 192 * 1024, 512 * 1024,
    /* games: the Play picker (~35 KB) stays beneath a running game, whose
     * runtime and view each hold a state and sprite set of similar size. */
    1280 * 1024, 5 * 1024 * 1024, 512 * 1024, 160 * 1024, 384 * 1024,
    256 * 1024, 256 * 1024,
    /* Matches the rasterizer's bounded transparent build envelope, outside the
     * retained-cache pool so preparation cannot evict its own output. */
    256 * 1024,
};

static bool valid(orb_memory_owner_t owner)
{
    return (unsigned)owner < ORB_MEMORY_COUNT;
}

static bool cache_owner(orb_memory_owner_t owner)
{
    return (unsigned)owner <= ORB_MEMORY_ROW_FONTS;
}

static void peak(owner_t *owner, size_t bytes)
{
    size_t previous = atomic_load_explicit(&owner->peak, memory_order_relaxed);
    while (bytes > previous && !atomic_compare_exchange_weak_explicit(
        &owner->peak, &previous, bytes, memory_order_relaxed, memory_order_relaxed)) {}
}

const char *orb_memory_name(orb_memory_owner_t owner)
{
    return valid(owner) ? s_names[owner] : "invalid";
}

void orb_memory_get(orb_memory_owner_t owner, orb_memory_stats_t *stats)
{
    if (!stats) return;
    *stats = (orb_memory_stats_t){0};
    if (!valid(owner)) return;
    stats->used = atomic_load_explicit(&s_owners[owner].used, memory_order_relaxed) +
        atomic_load_explicit(&s_owners[owner].tracked, memory_order_relaxed);
    stats->peak = atomic_load_explicit(&s_owners[owner].peak, memory_order_relaxed);
    stats->rejections = atomic_load_explicit(&s_owners[owner].rejections, memory_order_relaxed);
    stats->budget = s_budgets[owner];
}

bool orb_memory_reserve(orb_memory_owner_t owner, size_t bytes)
{
    if (!valid(owner)) return false;
    if (cache_owner(owner)) {
#ifdef ESP_PLATFORM
        size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t margin = orb_memory_cache_margin();
        if (available < margin || bytes > available - margin) {
            atomic_fetch_add_explicit(&s_owners[owner].rejections, 1, memory_order_relaxed);
            return false;
        }
#endif
        size_t used = atomic_load_explicit(&s_cache_used, memory_order_relaxed);
        do {
            if (used > CACHE_BUDGET || bytes > CACHE_BUDGET - used) {
                atomic_fetch_add_explicit(&s_owners[owner].rejections, 1, memory_order_relaxed);
                return false;
            }
        } while (!atomic_compare_exchange_weak_explicit(&s_cache_used, &used, used + bytes,
            memory_order_relaxed, memory_order_relaxed));
    }
    owner_t *state = &s_owners[owner];
    size_t used = atomic_load_explicit(&state->used, memory_order_relaxed);
    do {
        if (used > s_budgets[owner] || bytes > s_budgets[owner] - used) {
            if (cache_owner(owner)) atomic_fetch_sub_explicit(&s_cache_used, bytes, memory_order_relaxed);
            atomic_fetch_add_explicit(&state->rejections, 1, memory_order_relaxed);
            return false;
        }
    } while (!atomic_compare_exchange_weak_explicit(&state->used, &used, used + bytes,
        memory_order_relaxed, memory_order_relaxed));
    peak(state, used + bytes + atomic_load_explicit(&state->tracked, memory_order_relaxed));
    return true;
}

void orb_memory_release(orb_memory_owner_t owner, size_t bytes)
{
    if (!valid(owner)) return;
    owner_t *state = &s_owners[owner];
    size_t used = atomic_load_explicit(&state->used, memory_order_relaxed);
    /* Saturate on a bad caller rather than turning underflow into a huge live
     * charge that permanently blocks admission. Tests check balanced ownership. */
    while (!atomic_compare_exchange_weak_explicit(&state->used, &used,
        bytes > used ? 0 : used - bytes, memory_order_relaxed, memory_order_relaxed)) {}
    if (cache_owner(owner)) atomic_fetch_sub_explicit(&s_cache_used, bytes > used ? used : bytes,
        memory_order_relaxed);
}

void orb_memory_track(orb_memory_owner_t owner, size_t bytes)
{
    if (!valid(owner) || cache_owner(owner)) return;
    owner_t *state = &s_owners[owner];
    size_t tracked = atomic_fetch_add_explicit(&state->tracked, bytes, memory_order_relaxed);
    peak(state, tracked + bytes + atomic_load_explicit(&state->used, memory_order_relaxed));
}

void orb_memory_untrack(orb_memory_owner_t owner, size_t bytes)
{
    if (!valid(owner) || cache_owner(owner)) return;
    owner_t *state = &s_owners[owner];
    size_t tracked = atomic_load_explicit(&state->tracked, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&state->tracked, &tracked,
        bytes > tracked ? 0 : tracked - bytes, memory_order_relaxed, memory_order_relaxed)) {}
}

void orb_memory_observe(orb_memory_owner_t owner, size_t bytes)
{
    if (!valid(owner)) return;
    atomic_store_explicit(&s_owners[owner].used, bytes, memory_order_relaxed);
    peak(&s_owners[owner], bytes + atomic_load_explicit(&s_owners[owner].tracked, memory_order_relaxed));
}

void orb_memory_cache_register(orb_memory_owner_t owner, orb_memory_cache_oldest_t oldest,
                               orb_memory_cache_evict_t evict, void *context)
{
    if (valid(owner) && cache_owner(owner)) s_caches[owner] = (cache_t){oldest, evict, context};
}

size_t orb_memory_cache_used(void)
{
    return atomic_load_explicit(&s_cache_used, memory_order_relaxed);
}

size_t orb_memory_cache_budget(void)
{
    return CACHE_BUDGET;
}

size_t orb_memory_cache_margin(void)
{
    size_t backing = atomic_load_explicit(&s_cache_flash_backing, memory_order_relaxed);
    size_t credit = backing > PSRAM_FLASH_OVERHEAD ? backing - PSRAM_FLASH_OVERHEAD : 0;
    return PSRAM_MARGIN + (credit < PSRAM_FLASH_HEADROOM ? credit : PSRAM_FLASH_HEADROOM);
}

void orb_memory_cache_set_flash_backing(size_t bytes)
{
    atomic_store_explicit(&s_cache_flash_backing, bytes, memory_order_relaxed);
}

uint64_t orb_memory_cache_touch(void)
{
    /* Workers and UI compare one sequence. A tiny critical section avoids both
     * tick-wrap ordering and non-lock-free 64-bit S3 atomic helpers. */
#ifdef ESP_PLATFORM
    portENTER_CRITICAL(&s_clock_lock);
    uint64_t stamp = ++s_cache_clock;
    portEXIT_CRITICAL(&s_clock_lock);
    return stamp;
#else
    return atomic_fetch_add_explicit(&s_cache_clock, 1, memory_order_relaxed) + 1;
#endif
}

bool orb_memory_cache_admit(orb_memory_owner_t owner, size_t bytes)
{
    if (!valid(owner) || !cache_owner(owner) || bytes > s_budgets[owner] || bytes > CACHE_BUDGET) return false;
    /* No allocator, lock, or recursive admission in this loop. All callbacks
     * retire unpinned entries under their existing UI ownership. */
    for (;;) {
        size_t used = atomic_load_explicit(&s_owners[owner].used, memory_order_relaxed);
        size_t total = orb_memory_cache_used();
        bool own_fit = used <= s_budgets[owner] && bytes <= s_budgets[owner] - used;
        bool total_fit = total <= CACHE_BUDGET && bytes <= CACHE_BUDGET - total;
        bool margin_fit = true;
#ifdef ESP_PLATFORM
        size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t margin = orb_memory_cache_margin();
        margin_fit = available >= margin && bytes <= available - margin;
#endif
        if (own_fit && total_fit && margin_fit) return orb_memory_reserve(owner, bytes);
        unsigned victim = ORB_MEMORY_COUNT;
        uint64_t oldest = UINT64_MAX;
        for (unsigned i = 0; i < ORB_MEMORY_COUNT; i++) {
            if ((!own_fit && i != (unsigned)owner) || !s_caches[i].oldest || !s_caches[i].evict) continue;
            uint64_t age = s_caches[i].oldest(s_caches[i].context);
            if (age < oldest) { oldest = age; victim = i; }
        }
        size_t before = orb_memory_cache_used();
        if (victim == ORB_MEMORY_COUNT || !s_caches[victim].evict(s_caches[victim].context) ||
            orb_memory_cache_used() >= before) {
            atomic_fetch_add_explicit(&s_owners[owner].rejections, 1, memory_order_relaxed);
            return false;
        }
    }
}

size_t orb_memory_cache_trim(size_t bytes)
{
    size_t start = orb_memory_cache_used();
    while (start - orb_memory_cache_used() < bytes) {
        unsigned victim = ORB_MEMORY_COUNT;
        uint64_t oldest = UINT64_MAX;
        for (unsigned i = 0; i < ORB_MEMORY_COUNT; i++) {
            if (!s_caches[i].oldest || !s_caches[i].evict) continue;
            uint64_t age = s_caches[i].oldest(s_caches[i].context);
            if (age < oldest) { oldest = age; victim = i; }
        }
        size_t before = orb_memory_cache_used();
        if (victim == ORB_MEMORY_COUNT || !s_caches[victim].evict(s_caches[victim].context) ||
            orb_memory_cache_used() >= before) break;
    }
    size_t now = orb_memory_cache_used();
    return now < start ? start - now : 0;
}

static void *backend_alloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

static void backend_free(void *pointer)
{
#ifdef ESP_PLATFORM
    heap_caps_free(pointer);
#else
    free(pointer);
#endif
}

void *orb_memory_alloc(orb_memory_owner_t owner, size_t bytes)
{
    if (!bytes || bytes > SIZE_MAX - sizeof(allocation_t) || !orb_memory_reserve(owner, bytes)) return NULL;
    allocation_t *allocation = backend_alloc(sizeof(*allocation) + bytes);
    if (!allocation) {
        orb_memory_release(owner, bytes);
        atomic_fetch_add_explicit(&s_owners[owner].rejections, 1, memory_order_relaxed);
        return NULL;
    }
    allocation->value.bytes = bytes;
    allocation->value.owner = owner;
    return allocation + 1;
}

void *orb_memory_calloc(orb_memory_owner_t owner, size_t count, size_t bytes)
{
    if (count && bytes > SIZE_MAX / count) return NULL;
    void *pointer = orb_memory_alloc(owner, count * bytes);
    if (pointer) memset(pointer, 0, count * bytes);
    return pointer;
}

size_t orb_memory_size(const void *pointer)
{
    return pointer ? ((const allocation_t *)pointer - 1)->value.bytes : 0;
}

void orb_memory_free(void *pointer)
{
    if (!pointer) return;
    allocation_t *allocation = (allocation_t *)pointer - 1;
    orb_memory_release(allocation->value.owner, allocation->value.bytes);
    backend_free(allocation);
}

void *orb_memory_realloc(orb_memory_owner_t owner, void *pointer, size_t bytes)
{
    if (!pointer) return orb_memory_alloc(owner, bytes);
    if (!bytes) { orb_memory_free(pointer); return NULL; }
    allocation_t *previous = (allocation_t *)pointer - 1;
    if (previous->value.owner != owner || bytes > SIZE_MAX - sizeof(allocation_t)) return NULL;
    size_t old_size = previous->value.bytes;
    if (bytes > old_size && !orb_memory_reserve(owner, bytes - old_size)) return NULL;
    /* A resize charges its final logical size. Physical overlap is short-lived
     * and belongs to the documented workspace/fragmentation margin. */
    allocation_t *replacement = backend_alloc(sizeof(*replacement) + bytes);
    if (!replacement) {
        if (bytes > old_size) orb_memory_release(owner, bytes - old_size);
        atomic_fetch_add_explicit(&s_owners[owner].rejections, 1, memory_order_relaxed);
        return NULL;
    }
    replacement->value.bytes = bytes;
    replacement->value.owner = owner;
    memcpy(replacement + 1, pointer, bytes < old_size ? bytes : old_size);
    backend_free(previous);
    if (bytes < old_size) orb_memory_release(owner, old_size - bytes);
    return replacement + 1;
}

