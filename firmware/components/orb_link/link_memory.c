#include "orb_link_memory.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <pthread.h>
#endif

static bool s_initialized;
static unsigned char *s_parse_arena;
static char *s_rx_pool;
static atomic_bool s_rx_used[ORB_LINK_RX_SMALL_SLOTS + ORB_LINK_RX_LARGE_SLOTS];

static char *rx_slot(size_t i, size_t *capacity)
{
    if (i < ORB_LINK_RX_SMALL_SLOTS) {
        *capacity = ORB_LINK_RX_SMALL_BYTES;
        return s_rx_pool + i * ORB_LINK_RX_SMALL_BYTES;
    }
    *capacity = ORB_LINK_RX_LARGE_BYTES;
    return s_rx_pool + ORB_LINK_RX_SMALL_SLOTS * ORB_LINK_RX_SMALL_BYTES +
           (i - ORB_LINK_RX_SMALL_SLOTS) * ORB_LINK_RX_LARGE_BYTES;
}

void orb_link_memory_rx_free(char *pointer)
{
    if (!s_rx_pool || !pointer) return;
    for (size_t i = 0; i < ORB_LINK_RX_SMALL_SLOTS + ORB_LINK_RX_LARGE_SLOTS; ++i) {
        size_t capacity;
        if (rx_slot(i, &capacity) == pointer) {
            atomic_store_explicit(&s_rx_used[i], false, memory_order_release);
            return;
        }
    }
}

char *orb_link_memory_rx_resize(char *pointer, size_t bytes)
{
    if (!s_rx_pool || !bytes || bytes > ORB_LINK_RX_LARGE_BYTES) return NULL;
    size_t old_capacity = 0;
    if (pointer) {
        for (size_t i = 0; i < ORB_LINK_RX_SMALL_SLOTS + ORB_LINK_RX_LARGE_SLOTS; ++i) {
            size_t capacity;
            if (rx_slot(i, &capacity) == pointer) { old_capacity = capacity; break; }
        }
        if (!old_capacity) return NULL;
        if (bytes <= old_capacity) return pointer;
    }
    size_t first = bytes > ORB_LINK_RX_SMALL_BYTES ? ORB_LINK_RX_SMALL_SLOTS : 0;
    for (size_t i = first; i < ORB_LINK_RX_SMALL_SLOTS + ORB_LINK_RX_LARGE_SLOTS; ++i) {
        bool expected = false;
        if (!atomic_compare_exchange_strong_explicit(&s_rx_used[i], &expected, true,
                                                     memory_order_acquire, memory_order_relaxed)) continue;
        size_t capacity;
        char *result = rx_slot(i, &capacity);
        if (pointer) {
            memcpy(result, pointer, old_capacity);
            orb_link_memory_rx_free(pointer);
        }
        return result;
    }
    return NULL;
}
static size_t s_parse_used;
static atomic_uintptr_t s_parse_owner;
static atomic_flag s_parse_lock = ATOMIC_FLAG_INIT;

static uintptr_t current_owner(void)
{
#ifdef ESP_PLATFORM
    return (uintptr_t)xTaskGetCurrentTaskHandle();
#else
    return (uintptr_t)pthread_self();
#endif
}

bool orb_link_memory_begin_parse(void)
{
    if (!s_parse_arena || atomic_flag_test_and_set_explicit(&s_parse_lock, memory_order_acquire)) return false;
    s_parse_used = 0;
    atomic_store_explicit(&s_parse_owner, current_owner(), memory_order_release);
    return true;
}

void orb_link_memory_end_parse(void)
{
    if (atomic_load_explicit(&s_parse_owner, memory_order_acquire) != current_owner()) return;
    atomic_store_explicit(&s_parse_owner, 0, memory_order_release);
    atomic_flag_clear_explicit(&s_parse_lock, memory_order_release);
}

size_t orb_link_memory_parse_used(void) { return s_parse_used; }

static void *json_alloc(size_t size)
{
    if (atomic_load_explicit(&s_parse_owner, memory_order_acquire) == current_owner()) {
        const size_t align = _Alignof(max_align_t);
        size_t padded = size > SIZE_MAX - (align - 1) ? SIZE_MAX : (size + align - 1) & ~(align - 1);
        if (padded > ORB_LINK_PARSE_ARENA_BYTES - s_parse_used) return NULL;
        void *result = s_parse_arena + s_parse_used;
        s_parse_used += padded;
        return result;
    }
    /* JSON never serves DMA/ISR/cache-disabled work. Do not borrow internal
     * RAM on PSRAM exhaustion: that reserve belongs to Wi-Fi and the display. */
#ifdef ESP_PLATFORM
    return orb_memory_alloc(ORB_MEMORY_NETWORK, size);
#else
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

static void json_free(void *ptr)
{
    /* Parsed envelopes never escape the synchronous link callback. cJSON may
     * delete their nodes after the arena allocation bracket has ended. */
    uintptr_t address = (uintptr_t)ptr, base = (uintptr_t)s_parse_arena;
    if (base && address >= base && address - base < ORB_LINK_PARSE_ARENA_BYTES) return;
#ifdef ESP_PLATFORM
    orb_memory_free(ptr);
#else
    heap_caps_free(ptr);
#endif
}

void orb_link_memory_init(void)
{
    /* cJSON hooks are global, including voice/OTA objects and printed strings.
     * app_main installs these before service workers exist, and they remain
     * immutable thereafter; installing hooks lazily in a parser would race. */
    if (s_initialized) return;
#ifdef ESP_PLATFORM
    s_parse_arena = orb_memory_alloc(ORB_MEMORY_NETWORK, ORB_LINK_PARSE_ARENA_BYTES);
    s_rx_pool = orb_memory_alloc(ORB_MEMORY_NETWORK, ORB_LINK_RX_POOL_BYTES);
#else
    s_parse_arena = heap_caps_malloc(ORB_LINK_PARSE_ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rx_pool = heap_caps_malloc(ORB_LINK_RX_POOL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    cJSON_Hooks hooks = { .malloc_fn = json_alloc, .free_fn = json_free };
    cJSON_InitHooks(&hooks);
    s_initialized = true;
}
