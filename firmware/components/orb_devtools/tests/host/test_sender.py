"""Execute sender CPU snapshots against deterministic task-state and wrap seams."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

component = Path(__file__).resolve().parents[2]
source = (component / "orb_devtools_sender.c").read_text()
source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
prelude = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define configGENERATE_RUN_TIME_STATS TEST_STATS
#define configUSE_TRACE_FACILITY TEST_STATS
#define CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER TEST_STATS
#define pdFALSE 0
#define eBlocked 2
#if TEST_BITS == 32
 typedef uint32_t configRUN_TIME_COUNTER_TYPE;
#else
 typedef uint64_t configRUN_TIME_COUNTER_TYPE;
#endif
 typedef void *TaskHandle_t;
 typedef struct {configRUN_TIME_COUNTER_TYPE ulRunTimeCounter;} TaskStatus_t;
 typedef int (*orb_devtools_report_fn)(const char *, ...);
 typedef struct {TaskHandle_t task; uint64_t clock_start,before_us,elapsed_us; bool begun,available;} orb_devtools_sender_cpu_t;
static unsigned checks;
static char output[512];
#define CHECK(c) do {++checks; if (!(c)) {fprintf(stderr,"sender:%d %s\n",__LINE__,#c);abort();}} while(0)
static int report(const char *format, ...) {
 va_list args;va_start(args,format);int n=vsnprintf(output,sizeof(output),format,args);va_end(args);return n;
}
#if TEST_STATS
static uint64_t runtime_clock, sender_runtime;
static int64_t wall_clock;
static unsigned delays, blocked_after, runtime_queries;
static bool missing, replaced, timeout;
#define portGET_RUN_TIME_COUNTER_VALUE() runtime_clock
static int64_t esp_timer_get_time(void) {return wall_clock;}
static TaskHandle_t xTaskGetHandle(const char *name) {CHECK(!strcmp(name,"lcd_send"));return missing?NULL:(void *)(uintptr_t)(replaced?3:2);}
static int eTaskGetState(TaskHandle_t task) {CHECK(task==(void *)2);return !timeout&&delays>=blocked_after?eBlocked:0;}
static void vTaskDelay(unsigned ticks) {CHECK(ticks==1);++delays;wall_clock+=1000;runtime_clock+=1000;}
static void vTaskGetInfo(TaskHandle_t task,TaskStatus_t *status,int highwater,int state) {
 CHECK(task==(void *)2&&!highwater&&state==eBlocked&&!timeout&&delays>=blocked_after);
 ++runtime_queries;status->ulRunTimeCounter=(configRUN_TIME_COUNTER_TYPE)sender_runtime;
}
#endif
"""
tests = r"""
int main(void) {
 orb_devtools_sender_cpu_t sample;
 orb_devtools_sender_begin(&sample);
#if TEST_STATS
 CHECK(sample.begun&&!sample.available);sender_runtime=3000;runtime_clock=6000;
 orb_devtools_sender_end(&sample);CHECK(sample.available&&sample.elapsed_us==3000);
 orb_devtools_sender_report(&sample,3,report);CHECK(strstr(output,"sender_mean_us=1000.000"));
 orb_devtools_sender_report(&sample,0,report);CHECK(strstr(output,"sender_mean_us=0.000"));
 missing=true;orb_devtools_sender_begin(&sample);CHECK(!sample.begun);
 orb_devtools_sender_end(&sample);CHECK(!sample.available);missing=false;
 runtime_clock=1000;sender_runtime=100;blocked_after=2;delays=0;
 orb_devtools_sender_begin(&sample);CHECK(sample.begun&&delays==2);
 sender_runtime=300;runtime_clock=5000;orb_devtools_sender_end(&sample);CHECK(sample.available&&sample.elapsed_us==200);
 timeout=true;delays=0;unsigned queries=runtime_queries;
 orb_devtools_sender_begin(&sample);CHECK(!sample.begun&&delays==2&&runtime_queries==queries);
 timeout=false;blocked_after=0;delays=0;runtime_clock=1000;sender_runtime=100;
 orb_devtools_sender_begin(&sample);timeout=true;orb_devtools_sender_end(&sample);CHECK(!sample.available);
 timeout=false;orb_devtools_sender_begin(&sample);replaced=true;
 orb_devtools_sender_end(&sample);CHECK(!sample.available);replaced=false;
 runtime_clock=UINT64_MAX-1000;orb_devtools_sender_begin(&sample);
 runtime_clock=100;orb_devtools_sender_end(&sample);CHECK(!sample.available);
 runtime_clock=1000;sender_runtime=1000;orb_devtools_sender_begin(&sample);
 runtime_clock=2000;sender_runtime=100;orb_devtools_sender_end(&sample);CHECK(!sample.available);
 runtime_clock=1000;sender_runtime=100;orb_devtools_sender_begin(&sample);
 runtime_clock=2000;sender_runtime=1101;orb_devtools_sender_end(&sample);CHECK(!sample.available);
 for(unsigned i=0;i<100;++i) {
  runtime_clock=1000;sender_runtime=0;orb_devtools_sender_begin(&sample);
  runtime_clock=7000;sender_runtime=4000;orb_devtools_sender_end(&sample);
  CHECK(sample.available&&sample.elapsed_us==4000);
 }
#else
 CHECK(!sample.begun&&!sample.available);orb_devtools_sender_end(&sample);CHECK(!sample.available);
#endif
 orb_devtools_sender_report(&sample,4,report);CHECK(strstr(output,"cache_stalls_separated=0"));
 printf("sender CPU: %u actual-source checks, stats=%d bits=%d\n",checks,TEST_STATS,TEST_BITS);
}
"""
with tempfile.TemporaryDirectory(prefix="orb-sender-cpu-") as temporary:
    directory = Path(temporary)
    harness = directory / "sender.c"
    harness.write_text(prelude + source + tests)
    for stats, bits in ((0, 32), (1, 32), (1, 64)):
        binary = directory / f"sender-{stats}-{bits}"
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "clang")),
                "-std=c11",
                "-O1",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
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
