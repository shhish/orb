#include "orb_memory.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks, s_allocations;
static bool s_fail;
static size_t s_available = 2 * 1024 * 1024;
#define CHECK(test) do { s_checks++; if (!(test)) { \
    fprintf(stderr, "%d: %s\n", __LINE__, #test); exit(1); \
} } while (0)

void *heap_caps_malloc(size_t bytes, unsigned caps)
{
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_allocations++;
    return s_fail ? NULL : malloc(bytes);
}

void heap_caps_free(void *pointer) { free(pointer); }
size_t heap_caps_get_free_size(unsigned caps)
{
    CHECK(caps == MALLOC_CAP_SPIRAM);
    return s_available;
}

static void cache_floor(size_t backing, size_t expected)
{
    orb_memory_cache_set_flash_backing(backing);
    size_t margin = orb_memory_cache_margin();
    CHECK(margin == expected);
    s_available = margin - 1;
    CHECK(!orb_memory_cache_admit(ORB_MEMORY_MASKS, 1));
    CHECK(!orb_memory_reserve(ORB_MEMORY_GLASS, 1));
    s_available = margin;
    CHECK(!orb_memory_cache_admit(ORB_MEMORY_MASKS, 1));
    s_available = margin + 64;
    CHECK(!orb_memory_cache_admit(ORB_MEMORY_MASKS, 65));
    CHECK(!orb_memory_reserve(ORB_MEMORY_GLASS, 65));
    CHECK(orb_memory_cache_admit(ORB_MEMORY_MASKS, 64));
    CHECK(orb_memory_cache_used() == 64 && s_allocations == 2);
    orb_memory_release(ORB_MEMORY_MASKS, 64);
    CHECK(orb_memory_reserve(ORB_MEMORY_GLASS, 64));
    orb_memory_release(ORB_MEMORY_GLASS, 64);
    CHECK(orb_memory_cache_used() == 0);
}

int main(void)
{
    char *data = orb_memory_alloc(ORB_MEMORY_LVGL, 128);
    CHECK(data && s_allocations == 1);
    memset(data, 0x5a, 128);
    s_fail = true;
    CHECK(orb_memory_realloc(ORB_MEMORY_LVGL, data, 256) == NULL);
    CHECK(data[127] == 0x5a && orb_memory_size(data) == 128);
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_LVGL, &stats);
    CHECK(stats.used == 128 && stats.rejections == 1 && s_allocations == 2);
    orb_memory_free(data);
    orb_memory_get(ORB_MEMORY_LVGL, &stats);
    CHECK(stats.used == 0);
    cache_floor(0, 640u * 1024u);
    cache_floor(16u * 1024u, 640u * 1024u);
    cache_floor(32u * 1024u, 656u * 1024u);
    cache_floor(888077, 768u * 1024u);
    cache_floor(SIZE_MAX, 768u * 1024u);
    cache_floor(0, 640u * 1024u);
    size_t margin = orb_memory_cache_margin();
    /* The margin gates optional caches; it is not a physical pool withheld
     * from required LVGL allocations. OOM still fails in the backend. */
    s_available = margin / 2;
    s_fail = false;
    data = orb_memory_alloc(ORB_MEMORY_LVGL, 128);
    CHECK(data && s_allocations == 3);
    orb_memory_free(data);
    printf("ESP memory policy: %u checks passed\n", s_checks);
    return 0;
}
