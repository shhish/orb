/* Preserve the generic RGB565 filter bit for bit while specializing only its
 *opaque scale-only interior; checked AA edges remain the original path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "src/draw/lv_draw_image_private.h"
#include "src/draw/sw/lv_draw_sw.h"

#define W 332
#define H 85
#define CAPACITY (W * H * 3 + 32)
static uint8_t s_source[W * H * 3], s_control[CAPACITY], s_fast[CAPACITY];
static unsigned s_checks;
#define CHECK(c) do { s_checks++; if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec * 1e-6;
}

static void compare(lv_area_t area, lv_draw_image_dsc_t *dsc, lv_color_format_t cf)
{
    memset(s_control, 0xA5, sizeof(s_control));
    memset(s_fast, 0xA5, sizeof(s_fast));
    lv_draw_sw_transform_set_scale_fast(false);
    lv_draw_sw_transform(&area, s_source, W, H, W * 2, dsc, NULL, cf, s_control + 16);
    lv_draw_sw_transform_set_scale_fast(true);
    lv_draw_sw_transform(&area, s_source, W, H, W * 2, dsc, NULL, cf, s_fast + 16);
    CHECK(!memcmp(s_control, s_fast, sizeof(s_control)));
}

int main(void)
{
    lv_init();
    uint32_t random = 0x3154F18Au;
    for (size_t i = 0; i < sizeof(s_source); i++) {
        random = random * 1664525u + 1013904223u;
        s_source[i] = random >> 24;
    }
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    for (int format = 0; format < 3; format++) {
        lv_color_format_t cf = format == 0 ? LV_COLOR_FORMAT_RGB565 :
                               format == 1 ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565A8;
        for (unsigned scale = 1; scale <= LV_SCALE_NONE; scale++) {
            dsc.scale_x = scale;
            dsc.scale_y = LV_MAX(1, scale * 3 / 4);
            lv_area_t full;
            lv_image_buf_get_transformed_area(&full, W, H, 0, dsc.scale_x, dsc.scale_y, &dsc.pivot);
            compare(full, &dsc, cf);
            if (full.x2 > 8 && full.y2 > 4) {
                lv_area_t clipped = full;
                clipped.x1 += 3;
                clipped.x2 -= 2;
                clipped.y1 += 1;
                clipped.y2 -= 2;
                compare(clipped, &dsc, cf);
            }
        }
        dsc.scale_x = dsc.scale_y = 193;
        dsc.rotation = 170;
        lv_area_t area = {20, 5, 100, 35};
        compare(area, &dsc, cf);
        /*Half/full turns also have a constant source Y per output row. The
         *same integer accumulator must preserve negative X steps/positions.*/
        for (unsigned rotation = 1800; rotation <= 3600; rotation += 1800) {
            dsc.rotation = rotation;
            lv_area_t rotated;
            lv_image_buf_get_transformed_area(&rotated, W, H, rotation,
                                              dsc.scale_x, dsc.scale_y, &dsc.pivot);
            compare(rotated, &dsc, cf);
        }
        dsc.rotation = 0;
        dsc.antialias = false;
        compare(area, &dsc, cf);
        dsc.antialias = true;
    }
    dsc.scale_x = dsc.scale_y = 227;
    lv_area_t area;
    lv_image_buf_get_transformed_area(&area, W, H, 0, 227, 227, &dsc.pivot);
    const unsigned iterations = 300;
    double timings[2];
    for (int fast = 0; fast < 2; fast++) {
        lv_draw_sw_transform_set_scale_fast(fast);
        double start = now_ms();
        for (unsigned i = 0; i < iterations; i++) {
            lv_draw_sw_transform(&area, s_source, W, H, W * 2, &dsc, NULL,
                                 LV_COLOR_FORMAT_RGB565_SWAPPED, s_fast);
        }
        timings[fast] = now_ms() - start;
    }
    printf("RGB565 scale timing: generic %.3f us optimized %.3f us ratio %.3f (Mac host only)\n",
           timings[0] * 1000 / iterations, timings[1] * 1000 / iterations, timings[1] / timings[0]);
    lv_deinit();
    printf("RGB565 scale: %u exact full/clipped/fallback comparisons passed\n", s_checks);
    return 0;
}
