/* Host microbenchmark: restored input for each blur, warm caches, median/p95
 * over 501 calls. Times include allocation, masks, filtering and compositing;
 * restoring the input is outside the measurement. These are not device FPS.
 * Link to the simulator's liblvgl; a preserved implementation can be selected
 * with -DBLUR_BENCH_FUNCTION=<symbol> for identical before/after workloads. */
#define main blur_regressions_main
#include "test_blur.c"
#undef main
#include <time.h>

#ifndef BLUR_BENCH_FUNCTION
#define BLUR_BENCH_FUNCTION lv_draw_sw_blur
#endif
void BLUR_BENCH_FUNCTION(lv_draw_task_t *, const lv_draw_blur_dsc_t *, const lv_area_t *);

static double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int compare_time(const void * a, const void * b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    const int sizes[][2] = {{250, 72}, {72, 72}, {466, 104}};
    const lv_color_format_t formats[] = {LV_COLOR_FORMAT_RGB565, LV_COLOR_FORMAT_RGB565_SWAPPED};
    enum { SAMPLES = 501 };
    lv_init();
    for(unsigned format = 0; format < 2; format++) {
        for(unsigned size = 0; size < 3; size++) {
          for(unsigned motion = 0; motion < 2; motion++) {
            fixture_t f;
            fixture_init(&f, sizes[size][0], sizes[size][1], formats[format], 7, 31);
            for(int y = 0; y < f.height; y++) {
                for(int x = 0; x < f.width; x++) {
                    uint16_t value = x * 123 + y * 47;
                    memcpy(pixel(&f, x, y), &value, sizeof(value));
                }
            }
            save_before(&f);
            lv_draw_blur_dsc_t dsc = descriptor(26, LV_RADIUS_CIRCLE, 40);
            dsc.downsample_factor = motion ? 4 : 0;
            double samples[SAMPLES];
            for(int i = -16; i < SAMPLES; i++) {
                memcpy(f.allocation, f.before, f.size);
                double start = now_seconds();
                BLUR_BENCH_FUNCTION(&f.task, &dsc, &f.layer.buf_area);
                double elapsed = now_seconds() - start;
                if(i >= 0) samples[i] = elapsed;
            }
            qsort(samples, SAMPLES, sizeof(samples[0]), compare_time);
            printf("RGB565%s %dx%d factor%u: median %.2f us, p95 %.2f us (Mac host)\n",
                   format ? " swapped" : "", f.width, f.height, motion ? 4 : 3,
                   samples[SAMPLES / 2] * 1e6, samples[SAMPLES * 95 / 100] * 1e6);
            fixture_free(&f);
          }
        }
    }
    lv_deinit();
    return s_failed ? 1 : 0;
}
