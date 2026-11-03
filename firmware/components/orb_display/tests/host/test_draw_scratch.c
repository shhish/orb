/* Exercise real allocator policy, OOM handling and shared two-core budget. */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl/stdlib/lv_mem.h"
#include "esp_heap_caps.h"

static atomic_uint s_checks;
#define CHECK(cond) do { s_checks++; if(!(cond)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
} } while(0)

typedef union {
    max_align_t align;
    struct { size_t bytes; bool internal; } value;
} heap_header_t;
static pthread_mutex_t s_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t s_free = 128 * 1024, s_largest = 32 * 1024, s_heap_live;
static bool s_fail_internal, s_fail_psram, s_low_after;
static unsigned s_internal_calls, s_psram_calls;
static atomic_uint s_generic_calls;
static char s_zero;

void *lv_malloc(size_t bytes)
{
    if(!bytes) return &s_zero;
    atomic_fetch_add(&s_generic_calls, 1);
    return malloc(bytes);
}

void lv_free(void *ptr)
{
    if(ptr != &s_zero) free(ptr);
}

void lv_memset(void *ptr, uint8_t byte, size_t bytes)
{
    memset(ptr, byte, bytes);
}

void *heap_caps_malloc(size_t bytes, uint32_t caps)
{
    pthread_mutex_lock(&s_heap_lock);
    bool internal = (caps & MALLOC_CAP_INTERNAL) != 0;
    if(internal) s_internal_calls++;
    else s_psram_calls++;
    if((internal && s_fail_internal) || (!internal && s_fail_psram)) {
        pthread_mutex_unlock(&s_heap_lock);
        return NULL;
    }
    heap_header_t *header = malloc(bytes + sizeof(*header));
    if(!header) abort();
    header->value.bytes = bytes;
    header->value.internal = internal;
    if(internal) s_heap_live += bytes;
    pthread_mutex_unlock(&s_heap_lock);
    return header + 1;
}

void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, uint32_t caps)
{
    /* IDF's ordinary capability allocator guarantees only 4-byte alignment;
     * the production call must explicitly request the header's alignment. */
    CHECK(alignment == _Alignof(max_align_t));
    return heap_caps_malloc(bytes, caps);
}

void heap_caps_free(void *ptr)
{
    if(!ptr) return;
    heap_header_t *header = (heap_header_t *)ptr - 1;
    pthread_mutex_lock(&s_heap_lock);
    if(header->value.internal) s_heap_live -= header->value.bytes;
    pthread_mutex_unlock(&s_heap_lock);
    free(header);
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    pthread_mutex_lock(&s_heap_lock);
    size_t value = s_low_after && s_heap_live ? 20000 : s_free - s_heap_live;
    pthread_mutex_unlock(&s_heap_lock);
    return value;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return s_largest;
}

static bool enabled(lv_draw_scratch_kind_t kind)
{
#ifdef ESP_PLATFORM
    return (kind == LV_DRAW_SCRATCH_IMAGE && CONFIG_ORB_DISPLAY_INTERNAL_IMAGE_SCRATCH) ||
           (kind == LV_DRAW_SCRATCH_BLUR && CONFIG_ORB_DISPLAY_INTERNAL_BLUR_SCRATCH) ||
           (kind == LV_DRAW_SCRATCH_MASK && CONFIG_ORB_DISPLAY_INTERNAL_DRAW_MASKS);
#else
    (void)kind;
    return false;
#endif
}

static bool explicit_heap(void)
{
#ifdef ESP_PLATFORM
    return true;
#else
    return false;
#endif
}

static void check_empty(void)
{
    lv_draw_scratch_stats_t stats;
    lv_draw_scratch_get_stats(&stats);
    CHECK(stats.internal_in_use == 0);
    CHECK(stats.peak_internal <= 16 * 1024);
    CHECK(s_heap_live == 0);
}

static void check_kind(lv_draw_scratch_kind_t kind)
{
    unsigned calls = s_internal_calls, psram = s_psram_calls, generic = atomic_load(&s_generic_calls);
    void *ptr = lv_malloc_draw(3728, kind);
    CHECK(ptr != NULL);
    CHECK((uintptr_t)ptr % _Alignof(max_align_t) == 0);
    memset(ptr, 0xab, 3728);
    CHECK(s_internal_calls == calls + (enabled(kind) ? 1 : 0));
    CHECK(s_psram_calls == psram + (explicit_heap() && !enabled(kind) ? 1 : 0));
    CHECK(atomic_load(&s_generic_calls) == generic + (explicit_heap() ? 0 : 1));
    lv_free_draw(ptr, kind);
    lv_free_draw(NULL, kind);
    lv_free_draw(lv_malloc_draw(0, kind), kind);
    check_empty();
    if(!explicit_heap()) return;

    /* Tiny masks must keep max_align_t alignment and use the same bounded
     * policy as image tiles. An aligned header must never overlap payload. */
    for(size_t bytes = 1; bytes <= 16; bytes++) {
        ptr = lv_malloc_draw(bytes, kind);
        CHECK(ptr != NULL);
        CHECK((uintptr_t)ptr % _Alignof(max_align_t) == 0);
        memset(ptr, 0x5a, bytes);
        lv_free_draw(ptr, kind);
        check_empty();
    }

    /* Low total memory, fragmented DMA, failed allocation and a race after
     * admission all fall back to PSRAM and release the internal budget. */
    for(unsigned failure = 0; enabled(kind) && failure < 4; failure++) {
        s_free = failure == 0 ? 26000 : 128 * 1024;
        s_largest = failure == 1 ? 8191 : 32 * 1024;
        s_fail_internal = failure == 2;
        s_low_after = failure == 3;
        psram = s_psram_calls;
        ptr = lv_malloc_draw(3728, kind);
        CHECK(ptr != NULL);
        CHECK(s_psram_calls == psram + 1);
        memset(ptr, 0xcd, 3728);
        lv_free_draw(ptr, kind);
        check_empty();
    }
    s_free = 128 * 1024;
    s_largest = 32 * 1024;
    s_low_after = false;
    s_fail_internal = false;

    /* Oversized scratch never steals the reserve; total exhaustion fails
     * without attempting a generic allocator that could consume internal RAM. */
    calls = s_internal_calls;
    ptr = lv_malloc_draw(17000, kind);
    CHECK(ptr != NULL);
    CHECK(s_internal_calls == calls);
    lv_free_draw(ptr, kind);
    s_fail_internal = true;
    s_fail_psram = true;
    generic = atomic_load(&s_generic_calls);
    CHECK(lv_malloc_draw(1000, kind) == NULL);
    CHECK(lv_malloc_draw(1, kind) == NULL);
    CHECK(lv_malloc_draw(SIZE_MAX, kind) == NULL);
    CHECK(atomic_load(&s_generic_calls) == generic);
    check_empty();
    s_fail_internal = s_fail_psram = false;
}

static atomic_uint s_ready;
static atomic_bool s_release;
static lv_draw_scratch_kind_t s_thread_kind;

static void *allocate_worker(void *unused)
{
    (void)unused;
    void *ptr = lv_malloc_draw(6000, s_thread_kind);
    if(!ptr) abort();
    memset(ptr, 0xef, 6000);
    atomic_fetch_add(&s_ready, 1);
    while(!atomic_load(&s_release)) sched_yield();
    lv_free_draw(ptr, s_thread_kind);
    return NULL;
}

static void concurrent_budget(lv_draw_scratch_kind_t kind)
{
    if(!enabled(kind)) return;
    s_thread_kind = kind;
    atomic_store(&s_ready, 0);
    atomic_store(&s_release, false);
    pthread_t threads[4];
    unsigned psram = s_psram_calls;
    for(unsigned i = 0; i < 4; i++) CHECK(!pthread_create(&threads[i], NULL, allocate_worker, NULL));
    while(atomic_load(&s_ready) != 4) sched_yield();
    lv_draw_scratch_stats_t stats;
    lv_draw_scratch_get_stats(&stats);
    CHECK(stats.internal_in_use >= 12000 && stats.internal_in_use <= 16 * 1024);
    CHECK(s_psram_calls == psram + 2);
    atomic_store(&s_release, true);
    for(unsigned i = 0; i < 4; i++) CHECK(!pthread_join(threads[i], NULL));
    check_empty();
}

int main(void)
{
    for(unsigned i = 0; i < LV_DRAW_SCRATCH_KIND_COUNT; i++) {
        check_kind(i);
        concurrent_budget(i);
    }
    /* A direct widget-style lv_malloc call must retain its original policy. */
    unsigned calls = s_internal_calls + s_psram_calls;
    void *object = lv_malloc(64);
    CHECK(object != NULL);
    lv_free(object);
    CHECK(s_internal_calls + s_psram_calls == calls);
    object = lv_malloc_draw(8, (lv_draw_scratch_kind_t)-1);
    CHECK(object != NULL);
    lv_free_draw(object, (lv_draw_scratch_kind_t)-1);
    object = lv_malloc_draw(8, LV_DRAW_SCRATCH_KIND_COUNT);
    CHECK(object != NULL);
    lv_free_draw(object, LV_DRAW_SCRATCH_KIND_COUNT);
    CHECK(s_internal_calls + s_psram_calls == calls);
    lv_draw_scratch_get_stats(NULL);
    check_empty();
    printf("draw scratch %d/%d/%d: %u checks passed\n",
           enabled(LV_DRAW_SCRATCH_IMAGE), enabled(LV_DRAW_SCRATCH_BLUR),
           enabled(LV_DRAW_SCRATCH_MASK), s_checks);
    return 0;
}
