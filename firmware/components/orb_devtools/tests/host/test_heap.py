"""Compile the production heap receipt against the real memory policy."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

component = Path(__file__).resolve().parents[2]
source = Path(
    os.environ.get("ORB_DEVTOOLS_HEAP", str(component / "orb_devtools_diagnostics.c"))
).read_text()
start = source.index("static void print_heap(")
end = source.index("\n#define MAX_TASKS", start)
source = source[start:end]

prelude = r"""
#include "orb_memory.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_SPIRAM 4
typedef int (*orb_devtools_report_fn)(const char *format, ...);
typedef bool (*orb_devtools_reply_fn)(const char *format, ...);
typedef struct { size_t total_free_bytes, largest_free_block, minimum_free_bytes; } multi_heap_info_t;
typedef struct {
    size_t resident_bytes, mapped_bytes, bitmap_copy_bytes;
    unsigned loads, failures;
} orb_fonts_load_stats_t;
static void orb_fonts_load_stats(orb_fonts_load_stats_t *s) {
    *s=(orb_fonts_load_stats_t){.resident_bytes=1000,.mapped_bytes=888077,
        .bitmap_copy_bytes=512,.loads=20,.failures=0};
}
static char output[8192];
static void orb_mem_heap_info(multi_heap_info_t *info, unsigned caps) {
    assert(caps==1 || caps==3 || caps==4);
    *info=(multi_heap_info_t){.total_free_bytes=1000000,.largest_free_block=900000,.minimum_free_bytes=800000};
}
static size_t orb_model_memory_bytes(void) { return 80000; }
static size_t orb_home_memory_bytes(void) { return 40000; }
static size_t heap_caps_get_total_size(unsigned caps) { assert(caps==4); return 2000000; }
static size_t heap_caps_get_free_size(unsigned caps) { assert(caps==4); return 1000000; }
static int report(const char *format, ...) {
    va_list args; va_start(args,format);
    int n=vsnprintf(output+strlen(output),sizeof(output)-strlen(output),format,args);
    va_end(args); return n;
}
static bool reply(const char *format, ...) {
    va_list args; va_start(args,format);
    int n=vsnprintf(output+strlen(output),sizeof(output)-strlen(output),format,args);
    va_end(args); return n>=0;
}
"""
tests = r"""
int main(void) {
    orb_memory_cache_set_flash_backing(888077);
    assert(orb_memory_reserve(ORB_MEMORY_GLASS, 1024));
    assert(orb_memory_reserve(ORB_MEMORY_NETWORK, 512));
    orb_devtools_heap_report(report,reply);
    unsigned used=0,budget=0,margin=0;
    char *line=strstr(output,"@@ORBDEV HEAP_CACHE "); assert(line);
    assert(sscanf(line,"@@ORBDEV HEAP_CACHE used=%u budget=%u margin=%u",&used,&budget,&margin)==3);
    assert(used==orb_memory_cache_used() && budget==orb_memory_cache_budget());
    assert(margin==orb_memory_cache_margin() && margin==768u*1024u);
    assert(strstr(output,"@@HEAP psram") && strstr(output,"free 1000000  largest  900000  min-free  800000"));
    assert(strstr(output,"@@ORBDEV HEAP_FONTS resident=1000 mapped=888077 bitmap_copy=512 loads=20 failures=0"));
    assert(strstr(output,"@@ORBDEV HEAP_PSRAM tracked=121536 heap_allocated=1000000 residual=878464"));
    output[0]=0;
    orb_memory_cache_set_flash_backing(0);
    orb_devtools_heap_report(report,reply);
    line=strstr(output,"@@ORBDEV HEAP_CACHE "); assert(line);
    assert(sscanf(line,"@@ORBDEV HEAP_CACHE used=%u budget=%u margin=%u",&used,&budget,&margin)==3);
    assert(margin==orb_memory_cache_margin() && margin==640u*1024u);
    orb_memory_release(ORB_MEMORY_GLASS,1024);
    orb_memory_release(ORB_MEMORY_NETWORK,512);
    puts("heap receipt: actual cache policy and residual accounting passed");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="orb-devtools-heap-") as directory:
    test = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    test.write_text(prelude + source + tests)
    subprocess.run(
        [
            *shlex.split(os.environ.get("CC", "cc")),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-I" + str(component.parent / "orb_memory/include"),
            str(test),
            str(component.parent / "orb_memory/orb_memory.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
