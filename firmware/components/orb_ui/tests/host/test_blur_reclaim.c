/* Production shared-LRU callbacks: leases and unlocked allocator hooks. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define CACHE_SLOTS 4
#define LV_UNUSED(value) ((void)(value))
typedef void (*lv_draw_sw_blur_cache_free_cb_t)(void *);
typedef struct { void *data; size_t capacity; uint64_t used; bool valid, busy; } lv_blur_cache_entry_t;
static struct {
    int lock;
    lv_blur_cache_entry_t entries[CACHE_SLOTS];
    struct { lv_draw_sw_blur_cache_free_cb_t free_cb; } config;
    struct { size_t bytes; unsigned evictions; } stats;
    bool initialized, reconfiguring;
} cache;
static unsigned s_locked, s_freed;
static size_t s_owner_bytes;
static void lv_mutex_lock(int *lock) { assert(lock == &cache.lock && !s_locked); s_locked = 1; }
static void lv_mutex_unlock(int *lock) { assert(lock == &cache.lock && s_locked); s_locked = 0; }
static uint64_t cache_oldest(void *context);
static void release(void *pointer)
{
    assert(!s_locked && pointer == (void *)(uintptr_t)3);
    /* The allocator hook can reenter diagnostics without a metadata deadlock. */
    assert(cache.entries[2].busy && cache_oldest(NULL) == 9);
    s_freed++;
}
static void data_free(void *pointer, size_t bytes, lv_draw_sw_blur_cache_free_cb_t free_cb)
{
    assert(bytes == 300 && s_owner_bytes >= bytes);
    free_cb(pointer);
    s_owner_bytes -= bytes;
}
/* PRODUCTION_CALLBACKS */
int main(void)
{
    assert(cache_oldest(NULL) == UINT64_MAX && !cache_evict(NULL));
    cache.initialized = true;
    cache.config.free_cb = release;
    cache.entries[0] = (lv_blur_cache_entry_t){(void *)(uintptr_t)1, 100, 1, true, true};
    cache.entries[1] = (lv_blur_cache_entry_t){(void *)(uintptr_t)2, 200, 9, true, false};
    cache.entries[2] = (lv_blur_cache_entry_t){(void *)(uintptr_t)3, 300, 4, true, false};
    cache.stats.bytes = s_owner_bytes = 600;
    assert(cache_oldest(NULL) == 4);
    assert(cache_evict(NULL));
    assert(s_freed == 1 && s_owner_bytes == 300 && cache.stats.bytes == 300);
    assert(cache.stats.evictions == 1 && !cache.entries[2].busy && !cache.entries[2].data);
    assert(cache.entries[0].busy && cache.entries[0].data && cache.entries[1].valid);
    cache.reconfiguring = true;
    assert(cache_oldest(NULL) == UINT64_MAX && !cache_evict(NULL));
    cache.reconfiguring = false;
    cache.entries[1].busy = true;
    assert(cache_oldest(NULL) == UINT64_MAX && !cache_evict(NULL));
    assert(!s_locked && s_freed == 1);
    puts("blur shared LRU: oldest idle selection, leased protection, balanced accounting and reentrant free passed");
    return 0;
}
