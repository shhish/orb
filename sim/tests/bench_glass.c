/* Host render cost over deterministic virtual time, with the production
 * single 466x84 band and exact 192 KiB backdrop cache. No panel timing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "orb_demo.h"
#include "orb_ui.h"
#include "sim_display.h"
#include "include/lvgl/draw/lv_draw_sw_profile.h"
#include "src/draw/sw/lv_draw_sw_blur_cache_private.h"

static uint8_t s_band[466 * 84 * 2] __attribute__((aligned(64)));
static uint32_t s_now;
static double s_start, s_cpu_start, s_samples[2][1000], s_cpu_samples[2][1000];
static double s_refresh_samples[2][1000], s_refresh_cpu_samples[2][1000];
static unsigned s_count[2];
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
static lv_draw_sw_profile_snapshot_t s_before;
static uint64_t s_blur_calls[2], s_transform_calls[2];
static unsigned s_max_blur_calls[2];
static lv_draw_sw_profile_snapshot_t s_task_phases[3];
#endif

static void require(bool ok, const char *why)
{
    if (ok) return;
    fprintf(stderr, "glass timing regression: %s\n", why);
    exit(EXIT_FAILURE);
}

static uint32_t tick(void) { return s_now; }

static double seconds(clockid_t clock)
{
    struct timespec t;
    clock_gettime(clock, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void render_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
        s_start = seconds(CLOCK_MONOTONIC);
        s_cpu_start = seconds(CLOCK_PROCESS_CPUTIME_ID);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
        lv_draw_sw_profile_snapshot(&s_before);
#endif
    } else if (s_now >= 200 && s_now < 16000) {
        unsigned phase = s_now >= 8000;
        if (s_count[phase] < 1000) {
            unsigned sample = s_count[phase]++;
            s_samples[phase][sample] = (seconds(CLOCK_MONOTONIC) - s_start) * 1e6;
            s_cpu_samples[phase][sample] = (seconds(CLOCK_PROCESS_CPUTIME_ID) - s_cpu_start) * 1e6;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
            lv_draw_sw_profile_snapshot_t after;
            lv_draw_sw_profile_snapshot(&after);
            unsigned blur_calls = 0;
            for (unsigned i = 0; i < LV_DRAW_SW_PROFILE_SLOTS; i++) {
                uint32_t calls = after.rows[i].calls - s_before.rows[i].calls;
                if (!strcmp(lv_draw_sw_profile_type(i), "BLUR")) blur_calls += calls;
                if (!strcmp(lv_draw_sw_profile_type(i), "IMAGE") && lv_draw_sw_profile_transformed(i)) {
                    s_transform_calls[phase] += calls;
                }
            }
            s_blur_calls[phase] += blur_calls;
            s_max_blur_calls[phase] = LV_MAX(s_max_blur_calls[phase], blur_calls);
#if defined(CONFIG_ORB_DISPLAY_GLASS_SINGLE_PASS) && CONFIG_ORB_DISPLAY_GLASS_SINGLE_PASS
            /* Three controls, with the whole-control band guard. Reject the
             * old no-op task dispatches in bands outside those controls. */
            require(blur_calls <= 3, "more than one blur dispatch per control/frame");
#endif
#endif
        }
    }
}

static void motion(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    orb_demo_tick(0, 0, 1, 0.020f);
}

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    lv_display_set_buffers(display, s_band, NULL, sizeof(s_band), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_draw_sw_blur_cache_config_t cache = {.budget_bytes = 192 * 1024};
    if (lv_draw_sw_blur_cache_configure(&cache) != LV_RESULT_OK) return 1;
    orb_ui_init(NULL);
    orb_glass_cache_prewarm();
    /* Firmware preserves its live screen during a benchmark. Fill all three
     * leases here too, so host profiling cannot hide inactive-owner starvation. */
    lv_obj_t *saved = lv_screen_active();
    for(unsigned i = 0; i < 3; i++) {
        lv_obj_t *control = lv_button_create(saved);
        lv_obj_remove_style_all(control);
        lv_obj_set_size(control, ORB_CONTROL, ORB_CONTROL);
        lv_obj_set_pos(control, 70 + i * 130, 70);
        orb_glass_apply(control, ORB_GLASS_REGULAR, ORB_GRAY);
    }
    lv_obj_t *fixture = lv_obj_create(NULL);
    orb_demo_build(fixture);
    lv_screen_load(fixture);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    require(before.build_us > 0, "cold source raster construction time was not accounted");
#else
    require(before.build_us == 0, "source raster timing must be unavailable without profiling");
#endif
    lv_timer_create(motion, 20, NULL);
    lv_display_add_event_cb(display, render_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(display, render_event, LV_EVENT_RENDER_READY, NULL);
    lv_obj_raster_scale_cache_stats_t scale_phases[3];
    lv_draw_sw_material_stats_t material_phases[3];
    lv_obj_raster_rows_stats_t atlas_phases[3];
    orb_glass_fused_stats_t fused_phases[3];
    lv_obj_raster_scale_cache_get_stats(&scale_phases[0]);
    lv_draw_sw_material_get_stats(&material_phases[0]);
    lv_obj_raster_rows_get_stats(&atlas_phases[0]);
    orb_glass_fused_get_stats(&fused_phases[0]);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot(&s_task_phases[0]);
#endif
    bool entered_glass = false;
    while (s_now < 16000) {
        unsigned phase = s_now >= 8000;
        if (phase && !entered_glass) {
            lv_obj_raster_scale_cache_get_stats(&scale_phases[1]);
            lv_draw_sw_material_get_stats(&material_phases[1]);
            lv_obj_raster_rows_get_stats(&atlas_phases[1]);
            orb_glass_fused_get_stats(&fused_phases[1]);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
            lv_draw_sw_profile_snapshot(&s_task_phases[1]);
#endif
            entered_glass = true;
        }
        unsigned before = s_count[phase];
        double refresh_start = seconds(CLOCK_MONOTONIC);
        double refresh_cpu_start = seconds(CLOCK_PROCESS_CPUTIME_ID);
        uint32_t wait = lv_timer_handler();
        double refresh_us = (seconds(CLOCK_MONOTONIC) - refresh_start) * 1e6;
        double refresh_cpu_us = (seconds(CLOCK_PROCESS_CPUTIME_ID) - refresh_cpu_start) * 1e6;
        require(s_count[phase] <= before + 1, "one timer handler rendered multiple samples");
        if (s_count[phase] > before) {
            s_refresh_samples[phase][before] = refresh_us;
            s_refresh_cpu_samples[phase][before] = refresh_cpu_us;
        }
        s_now += wait ? LV_MIN(wait, 20) : 1;
    }
    lv_obj_raster_scale_cache_get_stats(&scale_phases[2]);
    lv_draw_sw_material_get_stats(&material_phases[2]);
    lv_obj_raster_rows_get_stats(&atlas_phases[2]);
    orb_glass_fused_get_stats(&fused_phases[2]);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot(&s_task_phases[2]);
#endif
    for (unsigned phase = 0; phase < 2; phase++) {
        unsigned n = s_count[phase];
        require(n >= 100, "phase rendered too few deterministic frames");
        qsort(s_samples[phase], n, sizeof(double), compare);
        qsort(s_cpu_samples[phase], n, sizeof(double), compare);
        qsort(s_refresh_samples[phase], n, sizeof(double), compare);
        qsort(s_refresh_cpu_samples[phase], n, sizeof(double), compare);
        printf("%s: %u virtual frames, median %.2f us, p95 %.2f us (Mac host)\n",
               phase ? "glass" : "scroll", n, s_samples[phase][n / 2], s_samples[phase][n * 95 / 100]);
        printf("%s CPU: median %.2f us, p95 %.2f us (all render threads)\n",
               phase ? "glass" : "scroll", s_cpu_samples[phase][n / 2], s_cpu_samples[phase][n * 95 / 100]);
        printf("%s whole refresh: median %.2f us, p95 %.2f us; CPU %.2f/%.2f us (includes pre-DRAW builds)\n",
               phase ? "glass" : "scroll", s_refresh_samples[phase][n / 2],
               s_refresh_samples[phase][n * 95 / 100], s_refresh_cpu_samples[phase][n / 2],
               s_refresh_cpu_samples[phase][n * 95 / 100]);
        require(s_samples[phase][n / 2] > 0 && s_cpu_samples[phase][n / 2] > 0,
                "timing clocks did not advance");
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
        printf("%s tasks: blur=%llu (max %u/frame), image_transform=%llu\n",
               phase ? "glass" : "scroll", (unsigned long long)s_blur_calls[phase],
               s_max_blur_calls[phase], (unsigned long long)s_transform_calls[phase]);
        require(s_blur_calls[phase] > 0 ||
                s_task_phases[phase + 1].stages[LV_DRAW_SW_PROFILE_GLASS_COMPOSE].calls !=
                s_task_phases[phase].stages[LV_DRAW_SW_PROFILE_GLASS_COMPOSE].calls,
                "draw profiler did not record reference or fused glass work");
        for (unsigned slot = 0; slot < LV_DRAW_SW_PROFILE_SLOTS; slot++) {
            uint32_t calls = s_task_phases[phase + 1].rows[slot].calls - s_task_phases[phase].rows[slot].calls;
            uint32_t elapsed = s_task_phases[phase + 1].rows[slot].elapsed_us - s_task_phases[phase].rows[slot].elapsed_us;
            if (!calls) continue;
            printf("DRAWTASK phase=%s type=%s%s class=%s calls=%u elapsed_us=%u us_per_frame=%.2f\n",
                   phase ? "glass" : "scroll", lv_draw_sw_profile_type(slot),
                   lv_draw_sw_profile_transformed(slot) ? "_TRANSFORM" : "",
                   lv_draw_sw_profile_class(slot), calls, elapsed, (double)elapsed / n);
        }
        /* Preparation includes time before RENDER_START. GLASS_PREPARE is
         * inclusive of its filter/downsample children; do not sum these rows. */
        for (unsigned stage = 0; stage < LV_DRAW_SW_PROFILE_STAGE_COUNT; stage++) {
            const lv_draw_sw_profile_stage_counter_t *begin = &s_task_phases[phase].stages[stage];
            const lv_draw_sw_profile_stage_counter_t *end = &s_task_phases[phase + 1].stages[stage];
            uint32_t calls = end->calls - begin->calls;
            uint32_t elapsed = end->elapsed_us - begin->elapsed_us;
            uint32_t pixels = end->pixels - begin->pixels;
            printf("HOTSPOT phase=%s stage=%s calls=%u elapsed_us=%u pixels=%u us_per_frame=%.2f\n",
                   phase ? "glass" : "scroll", lv_draw_sw_profile_stage_name(stage),
                   calls, elapsed, pixels, (double)elapsed / n);
        }
#endif
        const lv_obj_raster_rows_stats_t *atlas_start = &atlas_phases[phase];
        const lv_obj_raster_rows_stats_t *atlas_end = &atlas_phases[phase + 1];
        const orb_glass_fused_stats_t *fused_start = &fused_phases[phase];
        const orb_glass_fused_stats_t *fused_end = &fused_phases[phase + 1];
        printf("ROWATLAS phase=%s builds=%u fallbacks=%u draws=%u pixels=%u "
               "rows=%u glyphs=%u bytes=%u peak=%u budget=%u\n", phase ? "glass" : "scroll",
               atlas_end->builds - atlas_start->builds, atlas_end->fallbacks - atlas_start->fallbacks,
               atlas_end->draws - atlas_start->draws, atlas_end->pixels - atlas_start->pixels,
               atlas_end->rows, atlas_end->glyphs, atlas_end->bytes, atlas_end->peak_bytes, atlas_end->budget);
        printf("FUSEDGLASS phase=%s prepares=%u draws=%u fallbacks=%u maps=%u bytes=%zu peak=%zu budget=%zu\n",
               phase ? "glass" : "scroll", (uint32_t)(fused_end->prepares - fused_start->prepares),
               (uint32_t)(fused_end->draws - fused_start->draws),
               (uint32_t)(fused_end->fallbacks - fused_start->fallbacks), fused_end->maps,
               fused_end->bytes, fused_end->peak_bytes, fused_end->budget_bytes);
        const lv_obj_raster_scale_cache_stats_t *start = &scale_phases[phase];
        const lv_obj_raster_scale_cache_stats_t *end = &scale_phases[phase + 1];
        const lv_draw_sw_material_stats_t *material_start = &material_phases[phase];
        const lv_draw_sw_material_stats_t *material_end = &material_phases[phase + 1];
        printf("%s material: map_hits=%llu map_builds=%llu base_hits=%llu base_builds=%llu "
               "recaptures=%llu kernels=%llu bytes=%zu/%zu entries=%u\n",
               phase ? "glass" : "scroll",
               (unsigned long long)(material_end->map_hits - material_start->map_hits),
               (unsigned long long)(material_end->map_builds - material_start->map_builds),
               (unsigned long long)(material_end->base_hits - material_start->base_hits),
               (unsigned long long)(material_end->base_builds - material_start->base_builds),
               (unsigned long long)(material_end->source_recaptures - material_start->source_recaptures),
               (unsigned long long)(material_end->blur_kernels - material_start->blur_kernels),
               material_end->bytes, material_end->budget_bytes, material_end->entries);
        printf("%s scale cache: hits=%u misses=%u builds=%u evictions=%u alloc_fail=%u "
               "blits=%u build_us=%u bytes=%u peak=%u budget=%u\n",
               phase ? "glass" : "scroll", end->hits - start->hits, end->misses - start->misses,
               end->builds - start->builds, end->evictions - start->evictions,
               end->alloc_fail - start->alloc_fail, end->blits - start->blits,
               end->build_us - start->build_us, end->bytes, end->peak_bytes, end->budget);
    }
    lv_obj_raster_get_stats(&after);
    require(after.builds == before.builds, "steady scrolling rebuilt source rasters");
    require(after.build_us == before.build_us, "warm scrolling did unaccounted source preparation");
    require(after.mip_builds == before.mip_builds, "steady scrolling rebuilt material mips");
    require(after.mip_bytes <= after.mip_budget, "material mips exceeded the PSRAM budget");
    printf("material mips: cold_builds=%u warm_builds=%u bytes=%u budget=%u (included in raster bytes)\n",
           before.mip_builds, after.mip_builds - before.mip_builds, after.mip_bytes, after.mip_budget);
    lv_draw_sw_blur_cache_stats_t stats;
    lv_draw_sw_blur_cache_get_stats(&stats);
    require(stats.bytes <= stats.budget_bytes && stats.peak_bytes <= stats.budget_bytes,
            "backdrop cache exceeded its PSRAM budget");
    printf("exact cache: %zu/%zu bytes, peak %zu, hits %llu, misses %llu\n",
           stats.bytes, stats.budget_bytes, stats.peak_bytes,
           (unsigned long long)stats.hits, (unsigned long long)stats.misses);
    lv_obj_raster_scale_cache_stats_t scale;
    lv_obj_raster_scale_cache_get_stats(&scale);
    require(scale.bytes <= scale.budget && scale.peak_bytes <= scale.budget,
            "scaled row variants exceeded their PSRAM budget");
    printf("scale cache: hits=%u misses=%u builds=%u evictions=%u alloc_fail=%u blits=%u "
           "build_us=%u bytes=%u peak=%u budget=%u\n",
           scale.hits, scale.misses, scale.builds, scale.evictions, scale.alloc_fail, scale.blits,
           scale.build_us, scale.bytes, scale.peak_bytes, scale.budget);
    lv_screen_load(saved);
    lv_obj_delete(fixture);
    lv_obj_clean(saved);
    return 0;
}
