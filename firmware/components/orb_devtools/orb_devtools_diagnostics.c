#include "orb_devtools_diagnostics.h"
#include "orb_devtools_heaptrace.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "orb_display.h"
#include "orb_sound.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_mem.h"
#include "orb_memory.h"
#include "orb_model.h"
#include "orb_ui.h"

typedef struct {
    orb_glass_build_stats_t masks;
    orb_display_counters_t display;
} heap_detail_t;

void orb_devtools_heap_detail(orb_devtools_report_fn report)
{
    /* This exact live payload is part of HEAP_PSRAM residual, not another
     * owner charge. Other service/IDF allocations remain unattributed. */
    orb_sound_stats_t sound;
    orb_sound_stats(&sound);
    report("@@ORBDEV HEAP_UNOWNED sound_assets=%lu\n", (unsigned long)sound.asset_bytes);
    /* Neither snapshot is used by DMA/ISRs. A single bounded PSRAM block
     * avoids the old >1KiB report-only frame on the 4KiB console task. */
    orb_devtools_heaptrace_report(report);
    heap_detail_t *detail = heap_caps_calloc(1, sizeof(*detail), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!detail) {
        report("@@ORBDEV HEAP_DETAIL available=0 reason=psram_scratch_oom bytes=%u\n", (unsigned)sizeof(*detail));
        return;
    }
    orb_glass_build_stats_t *masks = &detail->masks;
    lv_lock();
    orb_glass_build_stats(masks);
    lv_unlock();
    report("@@ORBDEV MASKS builds=%lu failures=%lu entries=%lu bytes=%u peak=%u budget=%u max_build_us=%llu circle_max_us=%llu fallback_draws=%lu\n",
           (unsigned long)masks->builds, (unsigned long)masks->failures,
           (unsigned long)masks->entries, (unsigned)masks->bytes,
           (unsigned)masks->peak_bytes, 640u * 1024u,
           (unsigned long long)masks->max_build_us, (unsigned long long)masks->circle_max_us,
           (unsigned long)masks->fallback_draws);
    orb_display_counters_t *display = &detail->display;
    orb_display_counters(display);
    report("@@ORBDEV DISPLAY pipeline=%d frames=%llu bands=%llu copied=%llu dropped=%llu errors=%llu failed_frames=%llu last_error=%ld\n",
           orb_display_pipeline_active(), (unsigned long long)display->frames,
           (unsigned long long)display->bands, (unsigned long long)display->pipeline_bands,
           (unsigned long long)display->pipeline_dropped, (unsigned long long)display->transfer_errors,
           (unsigned long long)display->failed_frames, (long)display->last_transfer_error);
    heap_caps_free(detail);
}

void orb_devtools_game_cpu_begin(orb_devtools_game_cpu_t *cpu)
{
    memset(cpu, 0, sizeof(*cpu));
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    cpu->clock_start = (configRUN_TIME_COUNTER_TYPE)portGET_RUN_TIME_COUNTER_VALUE();
#endif
}

void orb_devtools_game_cpu_stop(orb_devtools_game_cpu_t *cpu, TaskHandle_t worker, bool joined)
{
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    if (!joined || !worker) return;
    /* The done semaphore precedes self-suspension. Do not sample a running
     * TCB: its last slice is uncommitted. No caller resumes this owned worker,
     * so once suspended it stays frozen until the caller deletes it. */
    int64_t until = esp_timer_get_time() + 5000;
    while (eTaskGetState(worker) != eSuspended) {
        if (esp_timer_get_time() >= until) return;
        vTaskDelay(1);
    }
    TaskStatus_t status;
    vTaskGetInfo(worker, &status, pdFALSE, eSuspended);
    uint64_t end = (configRUN_TIME_COUNTER_TYPE)portGET_RUN_TIME_COUNTER_VALUE();
    /* IDF drops the slice crossing its runtime clock wrap. Refuse that
     * window instead of silently understating exclusive task CPU. */
    if (end < cpu->clock_start || status.ulRunTimeCounter > end - cpu->clock_start) return;
    cpu->worker_us = status.ulRunTimeCounter;
    cpu->worker_available = true;
#else
    (void)cpu; (void)worker; (void)joined;
#endif
}

void orb_devtools_game_cpu_report(const orb_devtools_game_cpu_t *cpu, uint64_t steps,
                                  orb_devtools_report_fn report)
{
    report("@@ORBDEV GAME_CPU worker_available=%d worker_task_us=%llu worker_mean_upper_us=%.3f basis=task_runtime_us scope=worker_lifetime_solver_pose_publication_loop_irq_charged sender_receipt=separate\n",
        cpu->worker_available, (unsigned long long)cpu->worker_us,
        cpu->worker_available && steps ? cpu->worker_us / (double)steps : 0);
}

void orb_devtools_game_beads_report(const orb_game_view_stats_t *before,
                                    const orb_game_view_stats_t *after, orb_devtools_report_fn report)
{
    report("@@ORBDEV GAME_BEADS draw_calls=%llu masks_missing=%llu ineligible=%llu serial=%llu dispatched=%llu serial_drain_us=%llu serial_native_us=%llu custom_slices=%llu custom_slice_wall_us=%llu custom_lifetime_slice_max_us=%llu fallback_slices=%llu fallback_slice_wall_us=%llu prepares=%lu rejected_prepares=%lu masks_ready=%d source_ready=%d eligible=%d total_sprites=%u batch_sprites=%u first_reject_index=%u first_reject_shape=%u first_reject_radius=%u first_reject_flags=%u reject_flag_index=1 reject_flag_shape=2 reject_flag_radius=4\n",
        (unsigned long long)(after->bead_draw_calls - before->bead_draw_calls),
        (unsigned long long)(after->bead_masks_missing - before->bead_masks_missing),
        (unsigned long long)(after->bead_ineligible - before->bead_ineligible),
        (unsigned long long)(after->bead_serial - before->bead_serial),
        (unsigned long long)(after->bead_dispatched - before->bead_dispatched),
        (unsigned long long)(after->bead_drain_us - before->bead_drain_us),
        (unsigned long long)(after->bead_native_us - before->bead_native_us),
        (unsigned long long)(after->bead_custom_slices - before->bead_custom_slices),
        (unsigned long long)(after->bead_custom_us - before->bead_custom_us),
        (unsigned long long)after->bead_custom_max_us,
        (unsigned long long)(after->bead_sync_slices - before->bead_sync_slices),
        (unsigned long long)(after->bead_sync_us - before->bead_sync_us),
        (unsigned long)(after->bead_prepares - before->bead_prepares),
        (unsigned long)(after->bead_rejected_prepares - before->bead_rejected_prepares),
        after->bead_masks_ready, after->bead_source_ready, after->bead_eligible,
        after->bead_total_sprites, after->bead_batch_sprites, after->bead_reject_index,
        after->bead_reject_shape, after->bead_reject_radius, after->bead_reject_flags);
}

static void print_heap(orb_devtools_report_fn report, const char *name, uint32_t caps)
{
    multi_heap_info_t i;
    orb_mem_heap_info(&i, caps);
    report("@@HEAP %-8s free %7u  largest %7u  min-free %7u\n", name, (unsigned)i.total_free_bytes,
           (unsigned)i.largest_free_block, (unsigned)i.minimum_free_bytes);
}

void orb_devtools_heap_report(orb_devtools_report_fn report, orb_devtools_reply_fn reply)
{
    print_heap(report, "internal", MALLOC_CAP_INTERNAL);
    print_heap(report, "dma", MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    print_heap(report, "psram", MALLOC_CAP_SPIRAM);
    orb_memory_observe(ORB_MEMORY_MODEL, orb_model_memory_bytes() + orb_home_memory_bytes());
    reply("@@ORBDEV HEAP_CACHE used=%u budget=%u margin=%u\n",
        (unsigned)orb_memory_cache_used(), (unsigned)orb_memory_cache_budget(), (unsigned)orb_memory_cache_margin());
    size_t tracked = 0;
    for (unsigned owner = 0; owner < ORB_MEMORY_COUNT; owner++) {
        orb_memory_stats_t memory;
        orb_memory_get((orb_memory_owner_t)owner, &memory);
        tracked += memory.used;
        reply("@@ORBDEV HEAP_OWNER owner=%s used=%u peak=%u budget=%u rejections=%u\n",
            orb_memory_name((orb_memory_owner_t)owner), (unsigned)memory.used,
            (unsigned)memory.peak, (unsigned)memory.budget, memory.rejections);
    }
    size_t allocated = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) -
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    reply("@@ORBDEV HEAP_PSRAM tracked=%u heap_allocated=%u residual=%u\n",
        (unsigned)tracked, (unsigned)allocated, (unsigned)(allocated > tracked ? allocated - tracked : 0));
    orb_fonts_load_stats_t fonts;
    orb_fonts_load_stats(&fonts);
    reply("@@ORBDEV HEAP_FONTS resident=%u mapped=%u bitmap_copy=%u loads=%lu failures=%lu\n",
        (unsigned)fonts.resident_bytes, (unsigned)fonts.mapped_bytes, (unsigned)fonts.bitmap_copy_bytes,
        (unsigned long)fonts.loads, (unsigned long)fonts.failures);
}

#define MAX_TASKS 40

/* CPU per task over one second, from FreeRTOS run-time counters. */
void __attribute__((noinline)) orb_devtools_top(orb_devtools_report_fn report)
{
    /* Diagnostic copies are never used by an ISR or DMA. Do not reserve
     * 3.2 KiB of internal BSS for a command that runs only on demand. */
    TaskStatus_t *a = heap_caps_calloc(2 * MAX_TASKS, sizeof(*a), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a) {
        report("@@TOP ERROR no memory\n");
        return;
    }
    TaskStatus_t *b = a + MAX_TASKS;
    configRUN_TIME_COUNTER_TYPE ta = 0, tb = 0;
    UBaseType_t na = uxTaskGetSystemState(a, MAX_TASKS, &ta);
    if (!na) {
        report("@@TOP ERROR task snapshot unavailable; capacity=%u\n", MAX_TASKS);
        heap_caps_free(a);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    UBaseType_t nb = uxTaskGetSystemState(b, MAX_TASKS, &tb);
    if (!nb) {
        report("@@TOP ERROR task snapshot unavailable; capacity=%u\n", MAX_TASKS);
        heap_caps_free(a);
        return;
    }
    /* Two cores: total time available is twice the elapsed time. */
    uint64_t total = (uint64_t)(tb - ta) * portNUM_PROCESSORS;
    /* pcTaskName and xHandle refer to live TCB memory which another core may
     * free after the snapshot. Print copied fields only; handles are compared
     * as identities but never passed to APIs or otherwise dereferenced. */
    report("@@TOP %-16s core prio  cpu%%  stack-free (core -2: unavailable)\n", "task#");
    for (UBaseType_t i = 0; i < nb; i++) {
        configRUN_TIME_COUNTER_TYPE before = 0;
        for (UBaseType_t k = 0; k < na; k++) {
            if (a[k].xHandle == b[i].xHandle && a[k].xTaskNumber == b[i].xTaskNumber) {
                before = a[k].ulRunTimeCounter;
            }
        }
        uint64_t used = b[i].ulRunTimeCounter - before;
        int core = -2;
#if configTASKLIST_INCLUDE_COREID == 1
        core = b[i].xCoreID == tskNO_AFFINITY ? -1 : (int)b[i].xCoreID;
#endif
        report("@@TOP #%-15u %4d %4u %5.1f %6u\n", (unsigned)b[i].xTaskNumber, core,
               (unsigned)b[i].uxCurrentPriority,
               total ? 100.0 * used / total : 0.0, (unsigned)b[i].usStackHighWaterMark);
    }
    heap_caps_free(a);
}
