/* Exercise the production payload adapter without a screen or LVGL lifetime. */
#include "orb_home_payload.h"
#include "orb_memory.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static unsigned s_checks;
#define CHECK(test) do { s_checks++; if (!(test)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)

#ifdef ESP_PLATFORM
static bool s_fail;
static unsigned s_allocations;
static size_t s_last_bytes;

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_allocations++;
    s_last_bytes = count * size;
    return s_fail ? NULL : calloc(count, size);
}
#endif

static void usage(size_t model, size_t workspace)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.used == model);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    CHECK(stats.used == workspace);
}

static void zeroed(const void *pointer, size_t bytes)
{
    const unsigned char *payload = pointer;
    CHECK(payload != NULL);
    for (size_t i = 0; i < bytes; i++) CHECK(payload[i] == 0);
}

int main(void)
{
    orb_memory_stats_t model, workspace;
    orb_memory_get(ORB_MEMORY_MODEL, &model);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &workspace);
    usage(0, 0);

#ifdef ESP_PLATFORM
    /* Each admission failure must avoid allocating and preserve other charges. */
    CHECK(orb_memory_reserve(ORB_MEMORY_MODEL, model.budget));
    CHECK(orb_home_payload_create(64, 32) == NULL && s_allocations == 0);
    usage(model.budget, 0);
    orb_memory_release(ORB_MEMORY_MODEL, model.budget);
    CHECK(orb_memory_reserve(ORB_MEMORY_WORKSPACE, workspace.budget));
    CHECK(orb_home_payload_create(64, 32) == NULL && s_allocations == 0);
    usage(0, workspace.budget);
    orb_memory_release(ORB_MEMORY_WORKSPACE, workspace.budget);

    s_fail = true;
    CHECK(orb_home_payload_create(64, 32) == NULL && s_allocations == 1);
    CHECK(s_last_bytes == 96);
    usage(0, 0);
    s_fail = false;
    size_t view_bytes = 64, model_bytes = 32;
#else
    /* Host payloads continue to allocate even when larger than board budgets. */
    size_t view_bytes = workspace.budget + 7, model_bytes = model.budget + 11;
#endif

    void *first = orb_home_payload_create(view_bytes, model_bytes);
    zeroed(first, view_bytes + model_bytes);
    usage(model_bytes, view_bytes);
    void *second = orb_home_payload_create(17, 19);
    zeroed(second, 36);
    usage(model_bytes + 19, view_bytes + 17);
#ifndef ESP_PLATFORM
    CHECK(orb_memory_reserve(ORB_MEMORY_MODEL, model.budget));
    CHECK(orb_memory_reserve(ORB_MEMORY_WORKSPACE, workspace.budget));
    usage(model_bytes + 19 + model.budget, view_bytes + 17 + workspace.budget);
    orb_memory_release(ORB_MEMORY_MODEL, model.budget);
    orb_memory_release(ORB_MEMORY_WORKSPACE, workspace.budget);
    usage(model_bytes + 19, view_bytes + 17);
#endif
    orb_home_payload_destroy(NULL, 100, 200);
    usage(model_bytes + 19, view_bytes + 17);
    orb_home_payload_destroy(first, view_bytes, model_bytes);
    usage(19, 17);
    orb_home_payload_destroy(second, 17, 19);
    usage(0, 0);

    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.budget == model.budget && stats.peak >= model_bytes + 19);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    CHECK(stats.budget == workspace.budget && stats.peak >= view_bytes + 17);
#ifndef ESP_PLATFORM
    orb_memory_get(ORB_MEMORY_MODEL, &stats);
    CHECK(stats.rejections == model.rejections);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    CHECK(stats.rejections == workspace.rejections);
#else
    CHECK(s_allocations == 3 && s_last_bytes == 36);
#endif
    printf("Home payload accounting: %u checks passed\n", s_checks);
    return 0;
}
