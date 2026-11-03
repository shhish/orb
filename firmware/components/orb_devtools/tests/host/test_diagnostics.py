"""Run the staged production diagnostics with deterministic allocation/scheduler seams."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

component = Path(__file__).resolve().parents[2]
source = Path(
    os.environ.get("ORB_DEVTOOLS_DIAGNOSTICS", str(component / "orb_devtools_diagnostics.c"))
).read_text()
source = source[: source.index("\nstatic void print_heap(")]
source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
view_header = (component.parent / "orb_games/include/orb_game_view.h").read_text()
start = view_header.index("typedef struct {")
end = view_header.index("} orb_game_view_stats_t;", start) + len("} orb_game_view_stats_t;")
source = view_header[start:end] + "\n" + source

prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define configGENERATE_RUN_TIME_STATS TEST_STATS
#define configUSE_TRACE_FACILITY TEST_STATS
#define CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER TEST_STATS
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdFALSE 0
#define eSuspended 3
#if TEST_BITS == 32
typedef uint32_t configRUN_TIME_COUNTER_TYPE;
#else
typedef uint64_t configRUN_TIME_COUNTER_TYPE;
#endif
typedef void *TaskHandle_t;
typedef struct { configRUN_TIME_COUNTER_TYPE ulRunTimeCounter; } TaskStatus_t;
typedef int (*orb_devtools_report_fn)(const char *format, ...);
typedef struct { uint64_t clock_start, worker_us; bool worker_available; } orb_devtools_game_cpu_t;
typedef struct { unsigned builds, failures, entries, bytes, peak_bytes; uint64_t max_build_us, circle_max_us; unsigned fallback_draws; } orb_glass_build_stats_t;
typedef struct { uint64_t frames, bands, pipeline_bands, pipeline_dropped, transfer_errors, failed_frames; long last_transfer_error; uint8_t histogram[1024]; } orb_display_counters_t;
typedef struct { uint32_t asset_bytes; } orb_sound_stats_t;
static unsigned sound_assets=344008;
static void orb_sound_stats(orb_sound_stats_t *s) { s->asset_bytes=sound_assets; }
static bool oom, locked;
#if TEST_STATS
static bool timeout;
#endif
static unsigned allocations, frees, state_queries, runtime_queries, delays, masks_queries, display_queries, checks;
static uint64_t runtime_clock;
#if TEST_STATS
static uint64_t worker_runtime;
static int64_t wall_clock;
static unsigned suspension_after;
#endif
static char output[4096];
#define CHECK(value) do { checks++; if (!(value)) { fprintf(stderr,"%d: %s\n",__LINE__,#value); abort(); } } while (0)
static void *heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    CHECK(count == 1 && size < 2048 && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    allocations++; return oom ? NULL : calloc(count,size);
}
static void heap_caps_free(void *p) { CHECK(p); frees++; free(p); }
static void lv_lock(void) { CHECK(!locked); locked=true; }
static void lv_unlock(void) { CHECK(locked); locked=false; }
static void orb_glass_build_stats(orb_glass_build_stats_t *s) { CHECK(locked); masks_queries++; s->builds=5; }
static void orb_display_counters(orb_display_counters_t *s) { CHECK(!locked); display_queries++; s->frames=19; }
static int orb_display_pipeline_active(void) { return 1; }
static void orb_devtools_heaptrace_report(orb_devtools_report_fn reply) { (void)reply; }
static int report(const char *format, ...) {
    CHECK(!locked); va_list args; va_start(args,format);
    int n=vsnprintf(output+strlen(output),sizeof(output)-strlen(output),format,args);
    va_end(args); return n;
}
#if TEST_STATS
#define portGET_RUN_TIME_COUNTER_VALUE() (runtime_clock)
static int64_t esp_timer_get_time(void) { return wall_clock; }
static int eTaskGetState(TaskHandle_t worker) {
    CHECK(worker==(void *)1); state_queries++;
    return !timeout && delays>=suspension_after ? eSuspended : 0;
}
static void vTaskDelay(unsigned ticks) { CHECK(ticks==1); delays++; wall_clock+=1000; runtime_clock+=1000; }
static void vTaskGetInfo(TaskHandle_t worker, TaskStatus_t *s, int highwater, int state) {
    CHECK(worker==(void *)1 && !timeout && delays>=suspension_after && !highwater && state==eSuspended);
    runtime_queries++; s->ulRunTimeCounter=(configRUN_TIME_COUNTER_TYPE)worker_runtime;
}
#endif
"""
tests = r"""
int main(void) {
    /* A failed optional detail allocation neither samples nor dereferences
     * snapshots. The caller's basic heap/owners/phase receipts precede this. */
    oom=true; orb_devtools_heap_detail(report);
    CHECK(allocations==1 && frees==0 && !locked && !masks_queries && !display_queries);
    CHECK(strstr(output,"HEAP_DETAIL available=0 reason=psram_scratch_oom") && !strstr(output,"@@ORBDEV MASKS"));
    CHECK(strstr(output,"@@ORBDEV HEAP_UNOWNED sound_assets=344008"));
    output[0]=0; oom=false; sound_assets=0; orb_devtools_heap_detail(report);
    CHECK(allocations==2 && frees==1 && !locked && masks_queries==1 && display_queries==1);
    CHECK(strstr(output,"@@ORBDEV MASKS builds=5") && strstr(output,"@@ORBDEV DISPLAY pipeline=1 frames=19"));
    CHECK(strstr(output,"@@ORBDEV HEAP_UNOWNED sound_assets=0"));
    for (unsigned i=0;i<100;i++) orb_devtools_heap_detail(report), output[0]=0;
    CHECK(allocations==frees+1);
    orb_devtools_game_cpu_t cpu;
    runtime_clock=1000; orb_devtools_game_cpu_begin(&cpu);
    CHECK(!cpu.worker_available && !cpu.worker_us);
    orb_devtools_game_cpu_stop(&cpu,(void *)1,false);
    CHECK(!cpu.worker_available && !runtime_queries && !state_queries);
    orb_devtools_game_cpu_stop(&cpu,NULL,true);
    CHECK(!cpu.worker_available && !runtime_queries && !state_queries);
#if TEST_STATS
    runtime_clock=5000; worker_runtime=3000; suspension_after=2;
    orb_devtools_game_cpu_stop(&cpu,(void *)1,true);
    CHECK(cpu.worker_available && cpu.worker_us==3000 && runtime_queries==1 && delays==2 && state_queries==3);
    output[0]=0; orb_devtools_game_cpu_report(&cpu,3,report);
    CHECK(strstr(output,"worker_available=1 worker_task_us=3000 worker_mean_upper_us=1000.000"));
    CHECK(strstr(output,"solver_pose_publication_loop_irq_charged sender_receipt=separate"));
    /* Done may wake the UI before worker suspension; a missing suspension
     * remains unavailable rather than reading the stale running TCB. */
    orb_devtools_game_cpu_begin(&cpu); timeout=true; delays=0;
    unsigned before=runtime_queries;
    orb_devtools_game_cpu_stop(&cpu,(void *)1,true);
    CHECK(!cpu.worker_available && runtime_queries==before && delays==5);
    timeout=false; suspension_after=0;
    runtime_clock=1000; orb_devtools_game_cpu_begin(&cpu);
    runtime_clock=2000; worker_runtime=1001;
    orb_devtools_game_cpu_stop(&cpu,(void *)1,true); CHECK(!cpu.worker_available);
    /* IDF drops the task slice crossing the runtime clock wrap. */
    runtime_clock=UINT64_MAX-2000; orb_devtools_game_cpu_begin(&cpu);
    runtime_clock=1000; worker_runtime=500;
    orb_devtools_game_cpu_stop(&cpu,(void *)1,true); CHECK(!cpu.worker_available);
    runtime_clock=1000; orb_devtools_game_cpu_begin(&cpu);
    runtime_clock=2000; worker_runtime=1000;
    orb_devtools_game_cpu_stop(&cpu,(void *)1,true);
    CHECK(cpu.worker_available && cpu.worker_us==1000);
#else
    CHECK(!cpu.worker_available && !runtime_queries && !state_queries && !delays);
    output[0]=0; orb_devtools_game_cpu_report(&cpu,0,report);
    CHECK(strstr(output,"worker_available=0 worker_task_us=0 worker_mean_upper_us=0.000"));
#endif
    orb_game_view_stats_t view_before={.bead_draw_calls=7,.bead_masks_missing=1,.bead_ineligible=3,
        .bead_serial=2,.bead_dispatched=1,.bead_custom_slices=5,.bead_custom_us=100,.bead_prepares=22,.bead_rejected_prepares=2};
    orb_game_view_stats_t view_after={.bead_draw_calls=11,.bead_masks_missing=2,.bead_ineligible=3,
        .bead_serial=5,.bead_dispatched=1,.bead_custom_slices=9,.bead_custom_us=160,.bead_custom_max_us=19,
        .bead_prepares=26,.bead_rejected_prepares=3,.bead_masks_ready=true,.bead_source_ready=true,
        .bead_total_sprites=268,.bead_batch_sprites=256,.bead_reject_index=256,.bead_reject_shape=17,
        .bead_reject_radius=3,.bead_reject_flags=7};
    output[0]=0; orb_devtools_game_beads_report(&view_before,&view_after,report);
    CHECK(strstr(output,"draw_calls=4 masks_missing=1 ineligible=0 serial=3 dispatched=0"));
    CHECK(strstr(output,"custom_slices=4 custom_slice_wall_us=60 custom_lifetime_slice_max_us=19"));
    CHECK(strstr(output,"prepares=4 rejected_prepares=1 masks_ready=1 source_ready=1 eligible=0 total_sprites=268 batch_sprites=256"));
    CHECK(strstr(output,"first_reject_index=256 first_reject_shape=17 first_reject_radius=3 first_reject_flags=7"));
    printf("devtools diagnostics: %u checks; runtime=%d bits=%d; PSRAM/OOM cleanup and frozen worker-runtime races passed\n",checks,TEST_STATS,TEST_BITS);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="orb-devtools-diag-") as temporary:
    directory = Path(temporary)
    harness = directory / "diagnostics.c"
    harness.write_text(prelude + source + tests)
    for stats, bits in ((0, 32), (1, 32), (1, 64)):
        binary = directory / f"diagnostics-{stats}-{bits}"
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "clang")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-O1",
                "-g",
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                f"-DTEST_STATS={stats}",
                f"-DTEST_BITS={bits}",
                str(harness),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
