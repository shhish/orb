"""Compile bounded profiling counters with host LVGL types, without ESP-IDF."""

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
LVGL = ROOT / "firmware/third_party/lvgl__lvgl"
SOURCE = LVGL / "src/draw/sw/lv_draw_sw_profile.c"


def benchmark_fixture():
    source = (ROOT / "firmware/components/orb_devtools/orb_devtools.c").read_text()
    block = source.split(
        "#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE", 1
    )[1].split("#endif", 1)[0]
    return (
        '#include <stdlib.h>\n#include "orb_ui.h"\n#define main counter_main\n#include "'
        + str(Path(__file__).with_suffix(".c"))
        + '"\n#undef main\n'
        + """
typedef struct { uint64_t draw_us, frames; } orb_display_counters_t;
static struct { orb_display_counters_t display; } s_snapshots, *s_bench_snapshots = &s_snapshots;
static const char *s_bench_phase;
static lv_obj_raster_stats_t s_fake_raster;
static lv_obj_raster_rows_stats_t s_fake_atlas;
static orb_glass_fused_stats_t s_fake_fused;
void lv_obj_raster_get_stats(lv_obj_raster_stats_t *out) { *out = s_fake_raster; }
void lv_obj_raster_rows_get_stats(lv_obj_raster_rows_stats_t *out) { *out = s_fake_atlas; }
void orb_glass_fused_get_stats(orb_glass_fused_stats_t *out) { *out = s_fake_fused; }
#define heap_caps_free free
"""
        + block
        + """
int main(void) {
    counter_main();
    s_bench_profile = calloc(1, sizeof(*s_bench_profile));
    assert(s_bench_profile);
    lv_draw_sw_profile_snapshot(&s_bench_profile->previous);
    s_bench_profile->raster.builds = 5;
    s_bench_profile->raster.mip_builds = 5;
    s_bench_profile->row_atlas = (lv_obj_raster_rows_stats_t){.builds=3, .draws=9, .pixels=400};
    s_bench_profile->fused.prepares = UINT32_MAX - 1;
    lv_draw_dsc_base_t base = {0};
    lv_draw_task_t task = {.type = LV_DRAW_TASK_TYPE_FILL, .draw_dsc = &base};
    s_bench_phase = "scroll";
    s_bench_snapshots->display = (orb_display_counters_t){.draw_us=1000, .frames=10};
    orb_display_counters_t now = {.draw_us=5000, .frames=12};
    s_fake_raster = (lv_obj_raster_stats_t){.builds=9, .count=3, .bytes=800,
        .mip_builds=9, .mip_bytes=300, .mip_budget=512};
    s_fake_atlas = (lv_obj_raster_rows_stats_t){.rows=2, .glyphs=10, .bytes=300, .peak_bytes=500,
        .budget=1024, .builds=5, .fallbacks=1, .draws=19, .pixels=900};
    s_fake_fused = (orb_glass_fused_stats_t){.prepares=2, .draws=10, .fallbacks=1,
        .bytes=600, .peak_bytes=900, .budget_bytes=12288, .maps=3};
    lv_draw_sw_profile_record(&task, 3000);
    lv_draw_sw_profile_stage_record(LV_DRAW_SW_PROFILE_ROWS_DRAW, 1500, 120);
    lv_draw_sw_profile_stage_record(LV_DRAW_SW_PROFILE_GLASS_COMPOSE, 1100, 80);
    benchmark_profile_sample(&now);
    s_bench_snapshots->display = now;
    now.draw_us += 6000;
    now.frames += 3;
    s_bench_phase = "glass";
    s_fake_raster.builds = 12;
    s_fake_atlas.fallbacks++;
    s_fake_atlas.draws += 3;
    s_fake_atlas.pixels += 120;
    s_fake_fused.draws += 3;
    s_fake_fused.fallbacks += 2;
    lv_draw_sw_profile_record(&task, 2000);
    lv_draw_sw_profile_record(&task, 2500);
    lv_draw_sw_profile_stage_record(LV_DRAW_SW_PROFILE_GLASS_COMPOSE, 900, 60);
    benchmark_profile_sample(&now);
    assert(s_bench_profile->count == 2);
    assert(s_bench_profile->phases[0].raster.builds == 4);
    assert(s_bench_profile->phases[1].raster.builds == 3);
    s_bench_profile->count = BENCH_PROFILE_PHASES;
    benchmark_profile_sample(&now);
    assert(s_bench_profile->overflow);
    s_bench_profile->count = 2;
    puts("before deferred report");
    benchmark_profile_report();
    assert(!s_bench_profile);
    return 0;
}
"""
    )


def event_fixture():
    source = (LVGL / "src/core/lv_obj_event.c").read_text()
    body = source.split("lv_result_t lv_obj_send_event(", 1)[1].split(
        "\nlv_result_t lv_obj_event_base(", 1
    )[0]
    return (
        '#define main counter_main\n#include "'
        + str(Path(__file__).with_suffix(".c"))
        + '"\n#undef main\n#include "src/misc/lv_event_private.h"\n'
        + """
#undef LV_CHECK_OBJ
#define LV_CHECK_OBJ(obj, cls, action) do { if (!(obj)) { action; } } while (0)
static unsigned s_depth, s_delivered;
void lv_event_push(lv_event_t *e) { (void)e; s_depth++; }
void lv_event_pop(lv_event_t *e) { (void)e; assert(s_depth); s_depth--; }
static lv_result_t event_send_core(lv_event_t *e) {
    s_delivered++;
    if (e->code == LV_EVENT_DRAW_MAIN && e->param) {
        assert(lv_obj_send_event(e->current_target, LV_EVENT_DRAW_POST, NULL) == LV_RESULT_INVALID);
    }
    return LV_RESULT_INVALID;
}
lv_result_t lv_obj_send_event("""
        + body
        + """
int main(void) {
    lv_obj_t obj = {.class_p = &lv_obj_class};
    lv_draw_sw_profile_snapshot_t before, after;
    lv_draw_sw_profile_snapshot(&before);
    assert(lv_obj_send_event(&obj, LV_EVENT_CLICKED, NULL) == LV_RESULT_INVALID);
    assert(lv_obj_send_event(&obj, LV_EVENT_DRAW_MAIN_BEGIN, NULL) == LV_RESULT_INVALID);
    lv_draw_sw_profile_snapshot(&after);
    assert(after.stages[LV_DRAW_SW_PROFILE_DRAW_EVENTS].calls ==
           before.stages[LV_DRAW_SW_PROFILE_DRAW_EVENTS].calls);
    assert(lv_obj_send_event(&obj, LV_EVENT_DRAW_MAIN, &obj) == LV_RESULT_INVALID);
    assert(!s_depth && s_delivered == 4);
    lv_draw_sw_profile_snapshot(&after);
    assert(after.stages[LV_DRAW_SW_PROFILE_DRAW_EVENTS].calls -
           before.stages[LV_DRAW_SW_PROFILE_DRAW_EVENTS].calls == 2);
    assert(!after.stages[LV_DRAW_SW_PROFILE_DRAW_EVENTS].pixels);
    puts("draw profile: production event delivery preserves nesting/result; only MAIN/POST count");
    return 0;
}
"""
    )


def main():
    cc = os.environ.get("CC", "cc")
    common = [
        cc,
        "-std=gnu11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O1",
        "-g",
        "-DLV_CONF_INCLUDE_SIMPLE",
        "-DLV_KCONFIG_IGNORE",
        f"-I{ROOT / 'sim'}",
        f"-I{LVGL}",
        f"-I{LVGL / 'include'}",
        f"-I{ROOT / 'firmware/components/orb_ui/include'}",
        f"-I{ROOT / 'firmware/components/orb_tokens/include'}",
        f"-I{ROOT / 'firmware/components/orb_fonts/include'}",
    ]
    with tempfile.TemporaryDirectory(prefix="orb-draw-profile-") as temporary:
        directory = Path(temporary)
        binary = directory / "profile"
        subprocess.run(
            [
                *common,
                "-DCONFIG_ORB_DISPLAY_DRAW_PROFILE=1",
                "-fsanitize=address,undefined",
                str(SOURCE),
                str(Path(__file__).with_suffix(".c")),
                "-pthread",
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
        events = directory / "events.c"
        events.write_text(event_fixture())
        subprocess.run(
            [
                *common,
                "-DCONFIG_ORB_DISPLAY_DRAW_PROFILE=1",
                "-fsanitize=address,undefined",
                str(SOURCE),
                str(events),
                "-pthread",
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
        phases = directory / "phases.c"
        phases.write_text(benchmark_fixture())
        subprocess.run(
            [
                *common,
                "-DCONFIG_ORB_DISPLAY_DRAW_PROFILE=1",
                "-fsanitize=address,undefined",
                str(SOURCE),
                str(phases),
                "-pthread",
                "-o",
                str(binary),
            ],
            check=True,
        )
        report = subprocess.check_output([str(binary)], text=True)
        before, after = report.split("before deferred report\n")
        assert "@@ORBDEV" not in before
        for expected in (
            "overflow=1",
            "DRAWPROFILE phase=scroll draw_us=4000 frames=2",
            "DRAWPROFILE phase=glass draw_us=6000 frames=3",
            "DRAWTASK phase=scroll type=FILL class=none calls=1 elapsed_us=3000",
            "DRAWTASK phase=glass type=FILL class=none calls=2 elapsed_us=4500",
            "RASTER phase=scroll builds=4 count=3 bytes=800 mip_builds=4 mip_bytes=300 mip_budget=512",
            "RASTER phase=glass builds=3 count=3 bytes=800 mip_builds=0 mip_bytes=300 mip_budget=512",
            "HOTSPOT phase=scroll stage=ROWS_DRAW calls=1 elapsed_us=1500 pixels=120",
            "HOTSPOT phase=scroll stage=GLASS_COMPOSE calls=1 elapsed_us=1100 pixels=80",
            "HOTSPOT phase=glass stage=ROWS_DRAW calls=0 elapsed_us=0 pixels=0",
            "HOTSPOT phase=glass stage=GLASS_COMPOSE calls=1 elapsed_us=900 pixels=60",
            "ROWATLAS phase=scroll builds=2 fallbacks=1 draws=10 pixels=500 rows=2 glyphs=10 bytes=300 peak=500 budget=1024",
            "ROWATLAS phase=glass builds=0 fallbacks=1 draws=3 pixels=120 rows=2 glyphs=10 bytes=300 peak=500 budget=1024",
            "FUSEDGLASS phase=scroll prepares=4 draws=10 fallbacks=1 maps=3 bytes=600 peak=900 budget=12288",
            "FUSEDGLASS phase=glass prepares=0 draws=3 fallbacks=2 maps=3 bytes=600 peak=900 budget=12288",
        ):
            assert expected in after
        print("draw profile: deferred phase reports, exact deltas, overflow and cleanup passed")
        disabled = directory / "disabled.o"
        subprocess.run([*common, "-c", str(SOURCE), "-o", str(disabled)], check=True)
        symbols = subprocess.check_output(["nm", str(disabled)], text=True)
        assert "lv_draw_sw_profile" not in symbols
        disabled_calls = directory / "disabled.c"
        disabled_calls.write_text(
            '#include "include/lvgl/draw/lv_draw_sw_profile.h"\n'
            "void test(void) {\n"
            "    uint64_t begin = lv_draw_sw_profile_stage_begin();\n"
            "    lv_draw_sw_profile_stage_end(LV_DRAW_SW_PROFILE_ROWS_DRAW, begin, 42);\n"
            "    lv_draw_sw_profile_stage_record(LV_DRAW_SW_PROFILE_GLASS_FILTER, 1, 2);\n"
            "}\n"
        )
        subprocess.run([*common, "-c", str(disabled_calls), "-o", str(disabled)], check=True)
        symbols = subprocess.check_output(["nm", str(disabled)], text=True)
        assert "lv_draw_sw_profile" not in symbols
        print("draw profile: disabled object has no profiling symbols")


if __name__ == "__main__":
    main()
