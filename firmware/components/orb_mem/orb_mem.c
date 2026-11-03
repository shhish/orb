#include "orb_mem.h"

#if CONFIG_ORB_DEVTOOLS
#include <limits.h>
#include <stdio.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    size_t internal, dma, psram;
} minimum_t;

typedef struct {
    unsigned active, completed;
    minimum_t current, worst;
} phase_t;

typedef struct {
    unsigned count;
    size_t size;
    uint32_t caps;
} failure_t;

static const char *const s_names[] = {
    "link-connect", "tls-handshake", "hello", "json-message", "snapshot-parse",
    "card-show", "voice-service", "voice-start", "ota", "scroll", "nav", "wifi-start", "ui-refresh",
    "board-init", "nav-reserve", "assets-verify", "storage-mount", "font-preload", "glass-prewarm",
};
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static phase_t *s_phases;
static failure_t s_failures[ORB_MEM_PHASE_COUNT + 1];
static uint32_t s_active_mask;
static bool s_monitoring;

static minimum_t empty_minimum(void)
{
    return (minimum_t){SIZE_MAX, SIZE_MAX, SIZE_MAX};
}

static void merge(minimum_t *to, minimum_t value)
{
    if (value.internal < to->internal) to->internal = value.internal;
    if (value.dma < to->dma) to->dma = value.dma;
    if (value.psram < to->psram) to->psram = value.psram;
}

/* Called only at task boundaries under our mutex. Repeated IDF start() calls
 * reset local minima while retaining each region's historical minimum. */
static void capture(void)
{
    if (!s_monitoring) return;
    minimum_t value = {
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
    };
    for (unsigned i = 0; i < ORB_MEM_PHASE_COUNT; i++) {
        if (!s_phases[i].active) continue;
        merge(&s_phases[i].current, value);
        merge(&s_phases[i].worst, value);
    }
}

static void rotate(void)
{
    if (s_active_mask) {
        ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_start());
        s_monitoring = true;
    } else if (s_monitoring) {
        ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_stop());
        s_monitoring = false;
    }
}

/* Allocation failures can occur with the flash cache disabled. This callback
 * touches only internal BSS and lock-free 32-bit atomics: no heap, locks or log. */
static void IRAM_ATTR allocation_failed(size_t size, uint32_t caps, const char *function)
{
    (void)function;
    uint32_t mask = __atomic_load_n(&s_active_mask, __ATOMIC_RELAXED);
    for (unsigned i = 0; i <= ORB_MEM_PHASE_COUNT; i++) {
        if (i != ORB_MEM_PHASE_COUNT && !(mask & (1u << i))) continue;
        __atomic_store_n(&s_failures[i].size, size, __ATOMIC_RELAXED);
        __atomic_store_n(&s_failures[i].caps, caps, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_failures[i].count, 1, __ATOMIC_RELAXED);
    }
}

void orb_mem_init(void)
{
    /* One call from app_main before any service workers can enter a window. */
    if (s_mutex) return;
    /* Phase receipts are task-only and can grow without taking DMA/internal
     * space. The flash-safe allocation-failure counters below stay in BSS. */
    s_phases = heap_caps_calloc(ORB_MEM_PHASE_COUNT, sizeof(*s_phases),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_phases) return;
    for (unsigned i = 0; i < ORB_MEM_PHASE_COUNT; i++) {
        s_phases[i].current = s_phases[i].worst = empty_minimum();
    }
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(allocation_failed));
}

void orb_mem_begin(orb_mem_phase_t phase)
{
    if (!s_mutex || (unsigned)phase >= ORB_MEM_PHASE_COUNT) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    capture();
    if (!s_phases[phase].active++) s_phases[phase].current = empty_minimum();
    __atomic_fetch_or(&s_active_mask, 1u << phase, __ATOMIC_RELAXED);
    rotate();
    xSemaphoreGive(s_mutex);
}

void orb_mem_end_as(orb_mem_phase_t phase, orb_mem_phase_t attributed)
{
    if (!s_mutex || (unsigned)phase >= ORB_MEM_PHASE_COUNT) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_phases[phase].active) {
        capture();
        if (!--s_phases[phase].active) {
            s_phases[phase].completed++;
            if ((unsigned)attributed < ORB_MEM_PHASE_COUNT && attributed != phase) {
                merge(&s_phases[attributed].worst, s_phases[phase].current);
                s_phases[attributed].completed++;
            }
            __atomic_fetch_and(&s_active_mask, ~(1u << phase), __ATOMIC_RELAXED);
        }
        rotate();
    }
    xSemaphoreGive(s_mutex);
}

void orb_mem_end(orb_mem_phase_t phase)
{
    orb_mem_end_as(phase, phase);
}

static void suspend(void)
{
    capture();
    if (s_monitoring) {
        ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_stop());
        s_monitoring = false;
    }
}

void orb_mem_heap_regions_begin(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Stop restores the old array before esp_bt_mem_release can prepend new
     * internal heaps. A fresh start then snapshots the new region ordering. */
    suspend();
}

void orb_mem_heap_regions_end(void)
{
    if (!s_mutex) return;
    rotate();
    xSemaphoreGive(s_mutex);
}

void orb_mem_heap_info(multi_heap_info_t *info, uint32_t caps)
{
    if (!s_mutex) {
        heap_caps_get_info(info, caps);
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    suspend();
    heap_caps_get_info(info, caps);
    rotate();
    xSemaphoreGive(s_mutex);
}

void orb_mem_phase_info(orb_mem_phase_t phase, multi_heap_info_t *info, uint32_t caps)
{
    if (!s_mutex || (unsigned)phase >= ORB_MEM_PHASE_COUNT) {
        orb_mem_heap_info(info, caps);
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    suspend();
    heap_caps_get_info(info, caps);
    if (s_phases[phase].active) {
        minimum_t value = s_phases[phase].current;
        if (caps == MALLOC_CAP_INTERNAL) info->minimum_free_bytes = value.internal;
        else if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) info->minimum_free_bytes = value.dma;
        else if (caps == MALLOC_CAP_SPIRAM) info->minimum_free_bytes = value.psram;
    }
    rotate();
    xSemaphoreGive(s_mutex);
}

void orb_mem_report_to(FILE *out)
{
    if (!s_mutex) return;
    phase_t phases[ORB_MEM_PHASE_COUNT];
    multi_heap_info_t internal;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    suspend();
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL);
    for (unsigned i = 0; i < ORB_MEM_PHASE_COUNT; i++) phases[i] = s_phases[i];
    rotate();
    xSemaphoreGive(s_mutex);
    fprintf(out, "@@HEAPPHASE historical internal=%u target=24576 pass=%u\n",
           (unsigned)internal.minimum_free_bytes, internal.minimum_free_bytes >= 24 * 1024);
    for (unsigned i = 0; i <= ORB_MEM_PHASE_COUNT; i++) {
        unsigned failures = __atomic_load_n(&s_failures[i].count, __ATOMIC_RELAXED);
        if (i < ORB_MEM_PHASE_COUNT) {
            phase_t *phase = &phases[i];
            if (!phase->active && !phase->completed && !failures) continue;
            /* An unobserved phase is omitted, never represented as a pass. */
            fprintf(out, "@@HEAPPHASE %s count=%u active=%u internal=%u dma=%u psram=%u\n",
                   s_names[i], phase->completed, phase->active, (unsigned)phase->worst.internal,
                   (unsigned)phase->worst.dma, (unsigned)phase->worst.psram);
        }
        if (failures) {
            fprintf(out, "@@HEAPFAIL %s count=%u last_size=%u caps=0x%08x\n",
                   i == ORB_MEM_PHASE_COUNT ? "all" : s_names[i], failures,
                   (unsigned)__atomic_load_n(&s_failures[i].size, __ATOMIC_RELAXED),
                   (unsigned)__atomic_load_n(&s_failures[i].caps, __ATOMIC_RELAXED));
        }
    }
}
void orb_mem_report(void) { orb_mem_report_to(stdout); }
#endif
