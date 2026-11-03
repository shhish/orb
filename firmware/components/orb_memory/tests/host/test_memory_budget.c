#include "orb_memory.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
#define CHECK(test) do { s_checks++; if (!(test)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)

typedef struct {
    orb_memory_owner_t owner;
    size_t bytes;
    uint64_t stamp;
    bool pinned;
    unsigned evictions;
} cache_fixture_t;

static uint64_t oldest(void *context)
{
    cache_fixture_t *cache = context;
    return cache->pinned || !cache->bytes ? UINT64_MAX : cache->stamp;
}

static bool evict(void *context)
{
    cache_fixture_t *cache = context;
    if (cache->pinned || !cache->bytes) return false;
    orb_memory_release(cache->owner, cache->bytes);
    cache->bytes = 0;
    cache->evictions++;
    return true;
}

static void *concurrent(void *context)
{
    (void)context;
    for (unsigned i = 0; i < 10000; i++) {
        if (orb_memory_reserve(ORB_MEMORY_LVGL, 1024)) orb_memory_release(ORB_MEMORY_LVGL, 1024);
    }
    return NULL;
}

static void tracked_allocation(void)
{
    orb_memory_stats_t before, stats;
    orb_memory_get(ORB_MEMORY_MODEL, &before);
    size_t bytes = before.budget + 1;
    void *payload = calloc(1, bytes);
    CHECK(payload);
    /* Host legacy payloads remain allocatable above the board envelope without
     * reducing admission headroom for other payloads sharing the same owner. */
    orb_memory_track(ORB_MEMORY_MODEL, bytes);
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.used == bytes && stats.peak == bytes && stats.budget == before.budget);
    CHECK(stats.rejections == before.rejections);
    CHECK(orb_memory_reserve(ORB_MEMORY_MODEL, before.budget));
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.used == bytes + before.budget && stats.peak == stats.used);
    CHECK(!orb_memory_reserve(ORB_MEMORY_MODEL, 1));
    orb_memory_release(ORB_MEMORY_MODEL, before.budget);
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.used == bytes); /* Release only changes admitted allocations. */
    orb_memory_untrack(ORB_MEMORY_MODEL, bytes);
    free(payload);
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.used == 0 && stats.peak == bytes + before.budget && stats.rejections == before.rejections + 1);
    CHECK(orb_memory_reserve(ORB_MEMORY_MODEL, before.budget));
    orb_memory_release(ORB_MEMORY_MODEL, before.budget);

    orb_memory_track(ORB_MEMORY_COUNT, 1);
    orb_memory_track(ORB_MEMORY_GLASS, 1);
    orb_memory_untrack(ORB_MEMORY_COUNT, 1);
    orb_memory_untrack(ORB_MEMORY_GLASS, 1);
    orb_memory_get(ORB_MEMORY_GLASS, &stats);
    CHECK(stats.used == 0 && stats.peak == 0 && stats.rejections == 0);
    CHECK(orb_memory_cache_used() == 0);
}

static void raster_build_owner(void)
{
    orb_memory_stats_t workspace, build;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &workspace);
    orb_memory_get(ORB_MEMORY_RASTER_BUILD, &build);
    CHECK(!strcmp(orb_memory_name(ORB_MEMORY_RASTER_BUILD), "raster-build"));
    CHECK(build.used == 0 && build.budget == 256u * 1024u);
    /* Home's measured scratch still fits when the live/preview workspace fills
     * its own envelope. Preparation must not consume optional cache capacity. */
    CHECK(orb_memory_reserve(ORB_MEMORY_WORKSPACE, workspace.budget));
    CHECK(orb_memory_reserve(ORB_MEMORY_RASTER_BUILD, 30808));
    orb_memory_get(ORB_MEMORY_WORKSPACE, &workspace);
    orb_memory_get(ORB_MEMORY_RASTER_BUILD, &build);
    CHECK(workspace.used == workspace.budget && build.used == 30808);
    CHECK(orb_memory_cache_used() == 0);
    orb_memory_release(ORB_MEMORY_RASTER_BUILD, 30808);
    CHECK(orb_memory_reserve(ORB_MEMORY_RASTER_BUILD, build.budget));
    CHECK(!orb_memory_reserve(ORB_MEMORY_RASTER_BUILD, 1));
    orb_memory_get(ORB_MEMORY_RASTER_BUILD, &build);
    CHECK(build.used == build.budget && build.peak == build.budget && build.rejections == 1);
    orb_memory_release(ORB_MEMORY_RASTER_BUILD, build.used);
    orb_memory_release(ORB_MEMORY_WORKSPACE, workspace.used);
    orb_memory_get(ORB_MEMORY_RASTER_BUILD, &build);
    CHECK(build.used == 0 && orb_memory_cache_used() == 0);
}

int main(void)
{
    tracked_allocation();
    raster_build_owner();
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_NAV, &stats);
    CHECK(stats.used == 0 && stats.budget == 1280u * 1024u);
    CHECK(orb_memory_reserve(ORB_MEMORY_NAV, 3u * 466u * 466u * 2u));
    CHECK(!orb_memory_reserve(ORB_MEMORY_NAV, SIZE_MAX));
    orb_memory_get(ORB_MEMORY_NAV, &stats);
    CHECK(stats.used == 1302936 && stats.peak == 1302936 && stats.rejections == 1);
    orb_memory_release(ORB_MEMORY_NAV, stats.used);
    CHECK(!orb_memory_reserve(ORB_MEMORY_COUNT, 1));
    CHECK(!strcmp(orb_memory_name(ORB_MEMORY_NAV), "nav"));

    unsigned char *data = orb_memory_calloc(ORB_MEMORY_NETWORK, 16, 8);
    CHECK(data && (uintptr_t)data % _Alignof(max_align_t) == 0);
    CHECK(orb_memory_size(data) == 128);
    CHECK(data[0] == 0 && data[127] == 0);
    memset(data, 0xa5, 128);
    data = orb_memory_realloc(ORB_MEMORY_NETWORK, data, 512);
    CHECK(data && orb_memory_size(data) == 512 && data[127] == 0xa5);
    CHECK(orb_memory_realloc(ORB_MEMORY_NETWORK, data, SIZE_MAX) == NULL && data[127] == 0xa5);
    CHECK(orb_memory_realloc(ORB_MEMORY_NAV, data, 100) == NULL);
    data = orb_memory_realloc(ORB_MEMORY_NETWORK, data, 64);
    CHECK(data && orb_memory_size(data) == 64 && data[63] == 0xa5);
    CHECK(orb_memory_calloc(ORB_MEMORY_NETWORK, SIZE_MAX, 2) == NULL);
    CHECK(orb_memory_realloc(ORB_MEMORY_NETWORK, data, 0) == NULL);
    orb_memory_get(ORB_MEMORY_NETWORK, &stats);
    CHECK(stats.used == 0 && stats.peak == 512);

    cache_fixture_t raster = {ORB_MEMORY_RASTER, 600u * 1024u, orb_memory_cache_touch(), false, 0};
    cache_fixture_t page = {ORB_MEMORY_PAGE, 400u * 1024u, orb_memory_cache_touch(), true, 0};
    orb_memory_cache_register(raster.owner, oldest, evict, &raster);
    orb_memory_cache_register(page.owner, oldest, evict, &page);
    CHECK(orb_memory_cache_admit(raster.owner, raster.bytes));
    CHECK(orb_memory_cache_admit(page.owner, page.bytes));
    CHECK(orb_memory_cache_used() == 1000u * 1024u);
    CHECK(orb_memory_cache_admit(ORB_MEMORY_MASKS, 100u * 1024u));
    CHECK(raster.evictions == 1 && page.evictions == 0 && page.bytes == 400u * 1024u);
    CHECK(orb_memory_cache_used() == 500u * 1024u);
    CHECK(!orb_memory_cache_admit(ORB_MEMORY_RASTER, 768u * 1024u));
    CHECK(page.evictions == 0); /* Pinned capture/current draw survives pressure. */
    orb_memory_release(ORB_MEMORY_MASKS, 100u * 1024u);
    page.pinned = false;
    CHECK(orb_memory_cache_admit(ORB_MEMORY_RASTER, 768u * 1024u));
    CHECK(page.evictions == 1 && orb_memory_cache_used() == 768u * 1024u);
    orb_memory_release(ORB_MEMORY_RASTER, 768u * 1024u);
    CHECK(orb_memory_cache_used() == 0 && orb_memory_cache_budget() == 1024u * 1024u);
    CHECK(!orb_memory_cache_admit(ORB_MEMORY_PAGE, 449u * 1024u));

    /* Trim frees the oldest unpinned entries for a one-off large allocation. */
    raster = (cache_fixture_t){ORB_MEMORY_RASTER, 300u * 1024u, orb_memory_cache_touch(), false, 0};
    page = (cache_fixture_t){ORB_MEMORY_PAGE, 200u * 1024u, orb_memory_cache_touch(), false, 0};
    CHECK(orb_memory_cache_admit(raster.owner, raster.bytes) && orb_memory_cache_admit(page.owner, page.bytes));
    CHECK(orb_memory_cache_trim(100u * 1024u) == 300u * 1024u && raster.evictions == 1 && page.evictions == 0);
    page.pinned = true;
    CHECK(orb_memory_cache_trim(1024u * 1024u) == 0 && page.bytes == 200u * 1024u);
    page.pinned = false;
    CHECK(orb_memory_cache_trim(1024u * 1024u) == 200u * 1024u && orb_memory_cache_used() == 0);

    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) CHECK(!pthread_create(&threads[i], NULL, concurrent, NULL));
    for (unsigned i = 0; i < 4; i++) CHECK(!pthread_join(threads[i], NULL));
    orb_memory_get(ORB_MEMORY_LVGL, &stats);
    CHECK(stats.used == 0 && stats.peak <= 4096 && stats.peak >= 1024);
    printf("memory budget: %u checks passed; 40000 concurrent reserve/release pairs\n", s_checks);
    return 0;
}
