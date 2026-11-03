"""Compile production Stage 0 deferred receipts without ESP-IDF or a board."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
LVGL = ROOT / "firmware/third_party/lvgl__lvgl"


def main():
    source = (ROOT / "firmware/components/orb_devtools/orb_devtools.c").read_text()
    block = source.split(
        "#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING\n#define BENCH_ACCOUNT_PHASES", 1
    )[1]
    block = "#define BENCH_ACCOUNT_PHASES" + block.split("#endif", 1)[0]
    fixture = r"""
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "lvgl.h"
#include "orb_display_accounting.h"
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_SPIRAM 4
#define heap_caps_free free
static const char *s_bench_phase;
static orb_display_accounting_t s_fake;
static lv_obj_raster_stats_t s_raster;
static lv_draw_sw_material_stats_t s_material;
void orb_display_accounting(orb_display_accounting_t *out) { *out = s_fake; }
static bool orb_display_accounting_active(void) { return true; }
void lv_obj_raster_get_stats(lv_obj_raster_stats_t *out) { *out = s_raster; }
void lv_draw_sw_material_get_stats(lv_draw_sw_material_stats_t *out) { *out = s_material; }
void lv_draw_scratch_get_stats(lv_draw_scratch_stats_t *out) {
    *out = (lv_draw_scratch_stats_t){ .peak_internal=4096 };
}
static size_t heap_caps_get_largest_free_block(int caps) { return caps * 8192; }
"""
    fixture += (
        block
        + r"""
int main(void) {
    assert(sizeof(bench_account_t) < 20000);
    s_bench_account = calloc(1, sizeof(*s_bench_account));
    s_bench_account->cold_prepare_us = 12345;
    s_bench_account->cold_mip_builds = TEST_MIPS ? 7 : 0;
    s_bench_account->raster.mip_builds = TEST_MIPS ? 1 : 0;
    s_bench_phase = "scroll";
    s_fake.scheduled = s_fake.completed = 3;
    s_fake.preparation_us = 9000;
    s_fake.scene_prepare_us = 1000;
    s_fake.whole_update_us = 150000;
    s_fake.completion_interval[50] = 2;
    s_fake.whole_update[50] = 3;
    s_raster = (lv_obj_raster_stats_t){ .builds=5, .count=10, .bytes=500000,
        .mip_builds=TEST_MIPS ? 3 : 0, .mip_bytes=TEST_MIPS ? 800 : 0,
        .mip_budget=TEST_MIPS ? 196608 : 0 };
    s_material = (lv_draw_sw_material_stats_t){ .blur_kernels=9, .preparation_us=12000 };
    for (unsigned i=0; i<20; i++) orb_accounting_band(&s_fake,
        (orb_display_band_trace_t){ .draw_start_us=i*100, .queued_us=i*100+10,
          .sender_us=i*100+20, .dma_start_us=i*100+30, .dma_end_us=i*100+40,
          .bytes=1000, .y1=0, .y2=83, .ok=true });
    benchmark_account_sample();
    assert(s_bench_account->phases[0].material.preparation_us == 12000);
    assert(s_bench_account->phases[0].raster.mip_builds == (TEST_MIPS ? 2 : 0));
    s_bench_phase = "glass";
    if(TEST_MIPS) {
        s_bench_account->raster.mip_builds = UINT32_MAX - 2;
        s_raster.mip_builds = 4;
    }
    s_bench_account->material.preparation_us = UINT32_MAX - 9;
    s_material.preparation_us = 12;
    benchmark_account_sample();
    assert(s_bench_account->phases[1].material.preparation_us == 22);
    assert(s_bench_account->phases[1].raster.mip_builds == (TEST_MIPS ? 7 : 0));
    s_bench_phase = "quiet";
    benchmark_account_sample();
    assert(s_bench_account->phases[2].counters.completed == 0);
    assert(s_bench_account->phases[2].raster.builds == 0);
    assert(s_bench_account->phases[2].raster.mip_builds == 0);
    assert(s_bench_account->phases[2].material.blur_kernels == 0);
    s_bench_account->count = BENCH_ACCOUNT_PHASES;
    benchmark_account_sample();
    assert(s_bench_account->overflow);
    s_bench_account->count = 3;
    puts("before deferred report");
    benchmark_account_report();
    assert(!s_bench_account);
    return 0;
}
"""
    )
    with tempfile.TemporaryDirectory(prefix="orb-accounting-") as temporary:
        directory = Path(temporary)
        path = directory / "receipts.c"
        path.write_text(fixture)
        binary = directory / "receipts"
        for enabled in (0, 1):
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CC", "cc")),
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-O1",
                    "-g",
                    "-fsanitize=address,undefined",
                    "-DLV_CONF_INCLUDE_SIMPLE",
                    "-DLV_KCONFIG_IGNORE",
                    f"-DTEST_MIPS={enabled}",
                    f"-I{ROOT / 'sim'}",
                    f"-I{LVGL}",
                    f"-I{LVGL / 'include'}",
                    f"-I{ROOT / 'firmware/components/orb_display/include'}",
                    str(path),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            output = subprocess.check_output([str(binary)], text=True)
            before, after = output.split("before deferred report\n")
            assert "@@ORBDEV" not in before
            for expected in (
                "overflow=1 cold_prepare_us=12345",
                f"cold_mip_builds={7 if enabled else 0}",
                "ACCOUNT phase=scroll scheduled=3 completed=3 dropped=0 preparation_us=9000 scene_prepare_us=1000 whole_update_us=150000 source_builds=5",
                f"mip_builds={2 if enabled else 0} mip_bytes={800 if enabled else 0} mip_budget={196608 if enabled else 0}",
                "scratch_peak=4096 trace_omitted=12",
                "CADENCE phase=scroll bucket=50 intervals=2 whole_updates=3",
                "ACCOUNT phase=quiet scheduled=0 completed=0 dropped=0 preparation_us=0",
                "MATERIAL phase=quiet map_hits=0 map_builds=0 base_hits=0 base_builds=0 recaptures=0 kernels=0 fallbacks=0 preparation_us=0",
            ):
                assert expected in after, expected
            assert after.count("@@ORBDEV BAND phase=scroll ") == 8
            assert "@@ORBDEV BAND phase=quiet " not in after
        print(
            "update accounting: production deferred receipt deltas, cold prep, cadence, bounded trace, quiet and overflow passed"
        )


if __name__ == "__main__":
    main()
