/* Use the actual component, including its allocation-failure callback. The
 * heap stub models IDF's per-region reset/restore behavior, not a single global
 * minimum (which would miss errors when regions hit their lows separately). */
#include "orb_mem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
static bool s_in_failure;
static char s_output[8192];
static size_t s_output_len;

#define CHECK(test) do { \
    ++s_checks; \
    if (!(test)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

static int test_fprintf(FILE *out, const char *format, ...)
{
    CHECK(!s_in_failure);
    CHECK(out == stdout);
    va_list args;
    va_start(args, format);
    int length = vsnprintf(s_output + s_output_len, sizeof(s_output) - s_output_len, format, args);
    va_end(args);
    CHECK(length >= 0 && (size_t)length < sizeof(s_output) - s_output_len);
    s_output_len += (size_t)length;
    return length;
}

#define fprintf test_fprintf
#define malloc(size) ((s_in_failure ? abort() : (void)0), malloc(size))
#define calloc(count, size) ((s_in_failure ? abort() : (void)0), calloc(count, size))
#define realloc(ptr, size) ((s_in_failure ? abort() : (void)0), realloc(ptr, size))
#define free(ptr) ((s_in_failure ? abort() : (void)0), free(ptr))
#include "../../orb_mem.c"
#undef fprintf
#undef malloc
#undef calloc
#undef realloc
#undef free

typedef struct {
    uint32_t caps;
    size_t free, minimum, saved;
} fake_region_t;

static fake_region_t s_regions[4];
static unsigned s_region_order[4], s_region_count;
static size_t s_saved_minima[4];
static unsigned s_saved_count;
static bool s_local_monitor;
static unsigned s_starts, s_stops, s_registrations;
static esp_alloc_failed_hook_t s_failure_callback;

void *heap_caps_calloc(size_t count, size_t size, uint32_t caps)
{
    CHECK(!s_in_failure && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return calloc(count, size);
}

static size_t smaller(size_t a, size_t b) { return a < b ? a : b; }

/* Mirrors IDF heap_caps.c and multi_heap.c: repeated start saves every region's
 * previous low, then resets its visible low to current free; stop merges them. */
esp_err_t heap_caps_monitor_local_minimum_free_size_start(void)
{
    CHECK(!s_in_failure);
    ++s_starts;
    if (!s_local_monitor) {
        s_saved_count = s_region_count;
        for (unsigned i = 0; i < s_saved_count; ++i) s_saved_minima[i] = SIZE_MAX;
    }
    for (unsigned i = 0; i < s_saved_count; ++i) {
        fake_region_t *region = &s_regions[s_region_order[i]];
        s_saved_minima[i] = smaller(s_saved_minima[i], region->minimum);
        region->minimum = region->free;
    }
    s_local_monitor = true;
    return ESP_OK;
}

esp_err_t heap_caps_monitor_local_minimum_free_size_stop(void)
{
    CHECK(!s_in_failure && s_local_monitor);
    ++s_stops;
    for (unsigned i = 0; i < s_saved_count; ++i) {
        fake_region_t *region = &s_regions[s_region_order[i]];
        region->minimum = smaller(region->minimum, s_saved_minima[i]);
    }
    s_local_monitor = false;
    return ESP_OK;
}

esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback)
{
    CHECK(!s_in_failure && callback != NULL);
    ++s_registrations;
    s_failure_callback = callback;
    return ESP_OK;
}

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    CHECK(!s_in_failure);
    size_t minimum = 0;
    for (unsigned i = 0; i < s_region_count; ++i)
        if ((s_regions[i].caps & caps) == caps) minimum += s_regions[i].minimum;
    return minimum;
}

void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps)
{
    CHECK(!s_in_failure);
    memset(info, 0, sizeof(*info));
    info->minimum_free_bytes = heap_caps_get_minimum_free_size(caps);
    for (unsigned i = 0; i < s_region_count; ++i) {
        if ((s_regions[i].caps & caps) != caps) continue;
        info->total_free_bytes += s_regions[i].free;
        if (s_regions[i].free > info->largest_free_block) info->largest_free_block = s_regions[i].free;
    }
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    CHECK(!s_in_failure);
    *storage = 0;
    return storage;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t ticks)
{
    CHECK(!s_in_failure && semaphore != NULL && !*semaphore && ticks == portMAX_DELAY);
    *semaphore = 1;
    return 1;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    CHECK(!s_in_failure && semaphore != NULL && *semaphore);
    *semaphore = 0;
    return 1;
}

static void fixture(void)
{
    free(s_phases);
    s_phases = NULL;
    memset(s_failures, 0, sizeof(s_failures));
    s_mutex = NULL;
    s_active_mask = 0;
    s_monitoring = s_local_monitor = false;
    s_starts = s_stops = s_registrations = 0;
    s_output_len = 0;
    s_output[0] = 0;
    s_regions[0] = (fake_region_t){MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, 100000, 30000, 0};
    s_regions[1] = (fake_region_t){MALLOC_CAP_INTERNAL, 40000, 5000, 0};
    s_regions[2] = (fake_region_t){MALLOC_CAP_SPIRAM, 4000000, 3000000, 0};
    s_regions[3] = (fake_region_t){0};
    /* IDF adds PSRAM after the initial internal heaps, at the list head. */
    s_region_order[0] = 2;
    s_region_order[1] = 0;
    s_region_order[2] = 1;
    s_region_count = 3;
    s_saved_count = 0;
    orb_mem_init();
    orb_mem_init();
    CHECK(s_registrations == 1);
}

static void dip(unsigned region, size_t available)
{
    CHECK(region < 3 && available <= s_regions[region].free);
    s_regions[region].minimum = smaller(s_regions[region].minimum, available);
    /* The short-lived allocation is freed before the next marker: current
     * free is healthy, so sampling only current free would miss this dip. */
}

static void test_local_and_historical(void)
{
    fixture();
    orb_mem_begin(ORB_MEM_LINK_CONNECT);
    dip(0, 80000);
    orb_mem_end(ORB_MEM_LINK_CONNECT);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].completed == 1);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].worst.internal == 120000);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].worst.dma == 80000);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].worst.psram == 4000000);
    CHECK(!s_local_monitor && !s_monitoring && s_active_mask == 0);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) == 35000);

    orb_mem_begin(ORB_MEM_LINK_CONNECT);
    dip(0, 70000);
    orb_mem_end(ORB_MEM_LINK_CONNECT);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].completed == 2);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].current.internal == 110000);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].worst.internal == 110000);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) == 35000);
}

static void test_overlap_and_nesting(void)
{
    fixture();
    orb_mem_begin(ORB_MEM_LINK_CONNECT);
    dip(0, 80000);
    orb_mem_begin(ORB_MEM_TLS_HANDSHAKE);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].worst.internal == 120000);
    dip(1, 20000);
    orb_mem_begin(ORB_MEM_TLS_HANDSHAKE);
    dip(0, 70000);
    orb_mem_end(ORB_MEM_TLS_HANDSHAKE);
    CHECK(s_phases[ORB_MEM_TLS_HANDSHAKE].active == 1);
    CHECK(s_phases[ORB_MEM_TLS_HANDSHAKE].completed == 0);
    orb_mem_end(ORB_MEM_TLS_HANDSHAKE);
    CHECK(s_phases[ORB_MEM_TLS_HANDSHAKE].active == 0);
    CHECK(s_phases[ORB_MEM_TLS_HANDSHAKE].completed == 1);
    CHECK(s_phases[ORB_MEM_TLS_HANDSHAKE].worst.internal == 110000);
    CHECK(s_phases[ORB_MEM_TLS_HANDSHAKE].worst.dma == 70000);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].active == 1 && s_local_monitor);
    orb_mem_end(ORB_MEM_LINK_CONNECT);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].worst.internal == 110000);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].completed == 1);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) == 35000);
    unsigned starts = s_starts;
    orb_mem_end(ORB_MEM_LINK_CONNECT);
    orb_mem_begin((orb_mem_phase_t)-1);
    orb_mem_begin(ORB_MEM_PHASE_COUNT);
    orb_mem_end((orb_mem_phase_t)-1);
    CHECK(s_starts == starts && !s_local_monitor && s_active_mask == 0);
}

static void test_attribution(void)
{
    fixture();
    orb_mem_begin(ORB_MEM_MESSAGE_PARSE);
    dip(0, 60000);
    orb_mem_end_as(ORB_MEM_MESSAGE_PARSE, ORB_MEM_SNAPSHOT_PARSE);
    CHECK(s_phases[ORB_MEM_MESSAGE_PARSE].completed == 1);
    CHECK(s_phases[ORB_MEM_SNAPSHOT_PARSE].completed == 1);
    CHECK(s_phases[ORB_MEM_SNAPSHOT_PARSE].worst.internal == 100000);
    CHECK(s_phases[ORB_MEM_SNAPSHOT_PARSE].worst.dma == 60000);
    CHECK(!s_local_monitor && s_phases[ORB_MEM_SNAPSHOT_PARSE].active == 0);
    orb_mem_begin(ORB_MEM_MESSAGE_PARSE);
    dip(0, 20000);
    orb_mem_end_as(ORB_MEM_MESSAGE_PARSE, ORB_MEM_PHASE_COUNT);
    CHECK(s_phases[ORB_MEM_MESSAGE_PARSE].completed == 2);
    CHECK(s_phases[ORB_MEM_SNAPSHOT_PARSE].completed == 1);
    CHECK(s_phases[ORB_MEM_SNAPSHOT_PARSE].worst.internal == 100000);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) == 25000);
}

static void fail_allocation(size_t size, uint32_t caps)
{
    s_in_failure = true;
    s_failure_callback(size, caps, "private caller name is never retained");
    s_in_failure = false;
}

static void test_failures(void)
{
    fixture();
    fail_allocation(321, MALLOC_CAP_INTERNAL);
    CHECK(s_failures[ORB_MEM_PHASE_COUNT].count == 1);
    CHECK(s_failures[ORB_MEM_LINK_CONNECT].count == 0);
    orb_mem_begin(ORB_MEM_LINK_CONNECT);
    orb_mem_begin(ORB_MEM_TLS_HANDSHAKE);
    fail_allocation(1024, MALLOC_CAP_SPIRAM);
    CHECK(s_failures[ORB_MEM_PHASE_COUNT].count == 2);
    CHECK(s_failures[ORB_MEM_LINK_CONNECT].count == 1);
    CHECK(s_failures[ORB_MEM_TLS_HANDSHAKE].count == 1);
    CHECK(s_failures[ORB_MEM_TLS_HANDSHAKE].size == 1024);
    CHECK(s_failures[ORB_MEM_TLS_HANDSHAKE].caps == MALLOC_CAP_SPIRAM);
    CHECK(s_failures[ORB_MEM_HELLO].count == 0);
    orb_mem_end(ORB_MEM_TLS_HANDSHAKE);
    fail_allocation(44, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    CHECK(s_failures[ORB_MEM_LINK_CONNECT].count == 2);
    CHECK(s_failures[ORB_MEM_TLS_HANDSHAKE].count == 1);
    orb_mem_end(ORB_MEM_LINK_CONNECT);
    orb_mem_report();
    CHECK(strstr(s_output, "@@HEAPFAIL all count=3 last_size=44 caps=0x00000003") != NULL);
    CHECK(strstr(s_output, "@@HEAPFAIL tls-handshake count=1 last_size=1024 caps=0x00000004") != NULL);
    CHECK(strstr(s_output, "private caller") == NULL);
}

static void test_active_report(void)
{
    fixture();
    orb_mem_begin(ORB_MEM_LINK_CONNECT);
    /* Region minima happen at different times; their sum must be retained. */
    dip(0, 80000);
    dip(1, 20000);
    multi_heap_info_t info;
    orb_mem_heap_info(&info, MALLOC_CAP_INTERNAL);
    CHECK(info.minimum_free_bytes == 35000 && info.total_free_bytes == 140000);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].current.internal == 100000);
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].active == 1 && s_local_monitor && s_monitoring);
    dip(0, 25000);
    orb_mem_report();
    CHECK(strstr(s_output, "@@HEAPPHASE historical internal=30000 target=24576 pass=1") != NULL);
    CHECK(strstr(s_output, "@@HEAPPHASE link-connect count=0 active=1 internal=65000 dma=25000") != NULL);
    CHECK(strstr(s_output, "tls-handshake") == NULL); /* No invented phase pass. */
    CHECK(s_phases[ORB_MEM_LINK_CONNECT].active == 1 && s_local_monitor);
    dip(0, 10000);
    orb_mem_end(ORB_MEM_LINK_CONNECT);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) == 15000);
    s_output_len = 0;
    s_output[0] = 0;
    orb_mem_report();
    CHECK(strstr(s_output, "historical internal=15000 target=24576 pass=0") != NULL);
    CHECK(!s_local_monitor && s_stops > 0);
}

static void test_before_init(void)
{
    fixture();
    s_mutex = NULL;
    orb_mem_begin(ORB_MEM_HELLO);
    orb_mem_end(ORB_MEM_HELLO);
    orb_mem_report();
    CHECK(s_starts == 0 && s_output_len == 0);
    multi_heap_info_t info;
    orb_mem_heap_info(&info, MALLOC_CAP_INTERNAL);
    CHECK(info.minimum_free_bytes == 35000);
}

static void test_scroll_window(void)
{
    fixture();
    /* A boot-time DMA low is not evidence of a scroll-time allocation. Keep
     * the historical and invocation receipts distinct, including freed dips. */
    s_regions[0].minimum = 8;
    orb_mem_begin(ORB_MEM_SCROLL);
    dip(0, 50000);
    orb_mem_begin(ORB_MEM_MESSAGE_PARSE);
    dip(0, 40000);
    dip(1, 30000);
    orb_mem_end(ORB_MEM_MESSAGE_PARSE);
    multi_heap_info_t info;
    orb_mem_phase_info(ORB_MEM_SCROLL, &info, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    CHECK(info.minimum_free_bytes == 40000);
    CHECK(info.total_free_bytes == 100000 && info.largest_free_block == 100000);
    orb_mem_phase_info(ORB_MEM_SCROLL, &info, MALLOC_CAP_INTERNAL);
    CHECK(info.minimum_free_bytes == 70000 && info.total_free_bytes == 140000);
    orb_mem_phase_info(ORB_MEM_SCROLL, &info, MALLOC_CAP_SPIRAM);
    CHECK(info.minimum_free_bytes == 4000000);
    orb_mem_heap_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    CHECK(info.minimum_free_bytes == 8);
    fail_allocation(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    orb_mem_end(ORB_MEM_SCROLL);
    CHECK(!s_local_monitor && !s_monitoring && s_active_mask == 0);
    orb_mem_report();
    CHECK(strstr(s_output, "@@HEAPPHASE scroll count=1 active=0 internal=70000 dma=40000 psram=4000000") != NULL);
    CHECK(strstr(s_output, "@@HEAPFAIL scroll count=1 last_size=4096") != NULL);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) == 8);
}

static void test_nav_window(void)
{
    fixture();
    /* A boot-time DMA low is not evidence of a navigation-time allocation. Keep
     * the historical and invocation receipts distinct, including freed dips. */
    s_regions[0].minimum = 8;
    orb_mem_begin(ORB_MEM_NAV);
    dip(0, 50000);
    orb_mem_begin(ORB_MEM_MESSAGE_PARSE);
    dip(0, 40000);
    dip(1, 30000);
    orb_mem_end(ORB_MEM_MESSAGE_PARSE);
    multi_heap_info_t info;
    orb_mem_phase_info(ORB_MEM_NAV, &info, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    CHECK(info.minimum_free_bytes == 40000);
    CHECK(info.total_free_bytes == 100000 && info.largest_free_block == 100000);
    orb_mem_phase_info(ORB_MEM_NAV, &info, MALLOC_CAP_INTERNAL);
    CHECK(info.minimum_free_bytes == 70000 && info.total_free_bytes == 140000);
    orb_mem_phase_info(ORB_MEM_NAV, &info, MALLOC_CAP_SPIRAM);
    CHECK(info.minimum_free_bytes == 4000000);
    orb_mem_heap_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    CHECK(info.minimum_free_bytes == 8);
    fail_allocation(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    orb_mem_end(ORB_MEM_NAV);
    CHECK(!s_local_monitor && !s_monitoring && s_active_mask == 0);
    orb_mem_report();
    CHECK(strstr(s_output, "@@HEAPPHASE nav count=1 active=0 internal=70000 dma=40000 psram=4000000") != NULL);
    CHECK(strstr(s_output, "@@HEAPFAIL nav count=1 last_size=4096") != NULL);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) == 8);
}

static void release_bluetooth_region(void)
{
    /* heap_caps_add_region_with_caps prepends a region and gives a runtime
     * addition a historical minimum of zero. Restore still uses old ordinals. */
    CHECK(s_region_count == 3);
    for (unsigned i = s_region_count; i > 0; --i) s_region_order[i] = s_region_order[i - 1];
    s_region_order[0] = 3;
    s_region_count++;
    s_regions[3] = (fake_region_t){MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, 1024, 0, 0};
}

static void test_heap_region_insertion(void)
{
    fixture();
    s_regions[0].minimum = 7792;
    orb_mem_begin(ORB_MEM_WIFI_START);
    release_bluetooth_region();
    orb_mem_end(ORB_MEM_WIFI_START);
    /* Reproduce the real IDF bug: an internal minimum gets restored onto the
     * shifted PSRAM region although no PSRAM allocation happened at all. */
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) == 7792);

    fixture();
    s_regions[0].minimum = 7792;
    orb_mem_begin(ORB_MEM_WIFI_START);
    dip(2, 2500000);
    orb_mem_heap_regions_begin();
    CHECK(!s_monitoring && !s_local_monitor && *s_mutex == 1);
    release_bluetooth_region();
    orb_mem_heap_regions_end();
    CHECK(s_monitoring && s_local_monitor && !*s_mutex);
    dip(2, 2300000);
    orb_mem_end(ORB_MEM_WIFI_START);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) == 2300000);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) == 7792);
    CHECK(s_phases[ORB_MEM_WIFI_START].worst.psram == 2300000);

    /* Region changes without an active window do not start a local monitor. */
    fixture();
    orb_mem_heap_regions_begin();
    release_bluetooth_region();
    orb_mem_heap_regions_end();
    CHECK(!s_monitoring && !s_local_monitor && !*s_mutex);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) == 3000000);
}

int main(void)
{
    test_local_and_historical();
    test_overlap_and_nesting();
    test_attribution();
    test_failures();
    test_active_report();
    test_before_init();
    test_scroll_window();
    test_nav_window();
    test_heap_region_insertion();
    printf("orb_mem: %u checks passed (local/history, overlap, nesting, attribution, failures, active reports)\n", s_checks);
    return 0;
}
