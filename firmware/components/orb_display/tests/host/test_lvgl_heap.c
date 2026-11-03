/* Exercise the actual ESP LVGL/cJSON adapters and shared owner allocator. */
#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "orb_link_memory.h"
#include "orb_memory.h"

void *lv_malloc_core(size_t bytes);
void *lv_realloc_core(void *pointer, size_t bytes);
void lv_free_core(void *pointer);

static size_t s_allocations, s_live;
static bool s_fail;
size_t heap_caps_get_free_size(unsigned caps)
{
    assert(caps == MALLOC_CAP_SPIRAM);
    return 8 * 1024 * 1024;
}
static unsigned s_refresh_begins, s_refresh_ends;
#define ORB_MEM_UI_REFRESH 1
static void orb_mem_begin(int phase) { assert(phase == ORB_MEM_UI_REFRESH); s_refresh_begins++; }
static void orb_mem_end(int phase) { assert(phase == ORB_MEM_UI_REFRESH); s_refresh_ends++; }
#include "orb_refresh_memory_impl.inc"

void *heap_caps_malloc(size_t bytes, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_allocations++;
    if (s_fail) return NULL;
    void *pointer = malloc(bytes);
    assert(pointer);
    s_live++;
    return pointer;
}

void heap_caps_free(void *pointer)
{
    if (pointer) { assert(s_live); s_live--; }
    free(pointer);
}

int main(void)
{
    void *widgets[256];
    orb_memory_stats_t stats;
    for (size_t i = 0; i < 256; i++) {
        widgets[i] = lv_malloc_core(128);
        assert(widgets[i] && (uintptr_t)widgets[i] % alignof(max_align_t) == 0);
        memset(widgets[i], (int)i, 128);
    }
    orb_memory_get(ORB_MEMORY_LVGL, &stats);
    assert(stats.used == 256 * 128 && stats.peak == stats.used);
    assert(s_allocations == 256 && s_live == 256);

    unsigned char *grown = lv_realloc_core(widgets[42], 2048);
    assert(grown);
    for (unsigned i = 0; i < 128; i++) assert(grown[i] == 42);
    widgets[42] = grown;
    orb_memory_get(ORB_MEMORY_LVGL, &stats);
    assert(stats.used == 255 * 128 + 2048);

    s_fail = true;
    assert(!lv_realloc_core(grown, 4096));
    for (unsigned i = 0; i < 128; i++) assert(grown[i] == 42);
    assert(!lv_malloc_core(64));
    orb_memory_get(ORB_MEMORY_LVGL, &stats);
    assert(stats.used == 255 * 128 + 2048 && stats.rejections == 2);
    s_fail = false;
    for (unsigned i = 0; i < 256; i++) lv_free_core(widgets[i]);
    orb_memory_get(ORB_MEMORY_LVGL, &stats);
    assert(stats.used == 0 && s_live == 0);

    /* Owner budget rejection never reaches the backend or any internal heap. */
    size_t calls = s_allocations;
    assert(!lv_malloc_core(stats.budget + 1));
    assert(s_allocations == calls);
    assert(!lv_malloc_core(SIZE_MAX));
    assert(!lv_realloc_core(lv_malloc_core(8), 0));
    assert(!s_live);

    orb_link_memory_init();
    /* The link now retains its bounded parser arena and RX pool at startup.
     * JSON nodes must release back to that genuine persistent baseline. */
    orb_memory_get(ORB_MEMORY_NETWORK, &stats);
    size_t network_baseline = stats.used, live_baseline = s_live;
    cJSON *message = cJSON_Parse("{\"t\":\"evt\",\"m\":\"sessions.snapshot\",\"p\":{\"sessions\":[]}}");
    assert(message);
    char *encoded = cJSON_PrintUnformatted(message);
    assert(encoded && strstr(encoded, "sessions.snapshot"));
    orb_memory_get(ORB_MEMORY_NETWORK, &stats);
    assert(stats.used > strlen(encoded) && stats.peak >= stats.used);
    orb_memory_get(ORB_MEMORY_LVGL, &stats);
    assert(!stats.used);
    cJSON_Delete(message);
    cJSON_free(encoded);
    orb_memory_get(ORB_MEMORY_NETWORK, &stats);
    assert(stats.used == network_baseline && s_live == live_baseline);
    s_fail = true;
    assert(!cJSON_CreateObject());
    orb_memory_get(ORB_MEMORY_NETWORK, &stats);
    assert(stats.used == network_baseline && stats.rejections == 1 && s_live == live_baseline);
    cJSON_InitHooks(NULL);
    refresh_memory(false, 0);
    refresh_memory(true, 1000000);
    refresh_memory(true, 1000001);
    refresh_memory(false, 1001000);
    assert(s_refresh_begins == 1 && s_refresh_ends == 1);
    refresh_memory(true, 19999999);
    refresh_memory(false, 20000001);
    assert(s_refresh_begins == 2 && s_refresh_ends == 2);
    refresh_memory(true, 21000000);
    refresh_memory(false, 21001000);
    assert(s_refresh_begins == 2 && s_refresh_ends == 2);
    puts("LVGL/network heap: 256 small objects, resize ownership, PSRAM-only OOM, budget admission and JSON lifetimes passed");
    return 0;
}
