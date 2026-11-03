/* Regression tests for the real LVGL backdrop blur, linked to the simulator's
 * liblvgl.a. No display or draw thread scheduling is needed: each fixture is a
 * padded, guarded partial RGB565/RGB888/A8 layer. Optional argv[1] writes a PPM
 * image for review; argv[2] selects "image-only" or "resampling" for comparison
 * against the old renderer without running its known small-buffer edge cases.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw.h"

static int s_failed;
static int s_checks;
#define CHECK(cond) do { s_checks++; if(!(cond)) { \
    if(s_failed < 30) fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
    s_failed++; } } while(0)

enum { GUARD = 32 };
typedef struct {
    lv_draw_buf_t buf;
    lv_layer_t layer;
    lv_draw_task_t task;
    uint8_t * allocation;
    uint8_t * before;
    size_t size;
    int width, height, bytes, stride;
} fixture_t;

static void fixture_init(fixture_t * f, int width, int height, lv_color_format_t cf, int ox, int oy)
{
    memset(f, 0, sizeof(*f));
    f->width = width;
    f->height = height;
    f->bytes = lv_color_format_get_size(cf);
    f->stride = width * f->bytes + 16;
    f->size = (size_t)f->stride * height + 2 * GUARD;
    f->allocation = malloc(f->size);
    f->before = malloc(f->size);
    CHECK(f->allocation && f->before);
    if(!f->allocation || !f->before) exit(2);
    memset(f->allocation, 0xA5, f->size);
    CHECK(lv_draw_buf_init(&f->buf, width, height, cf, f->stride, f->allocation + GUARD,
                          f->stride * height) == LV_RESULT_OK);
    f->layer.draw_buf = &f->buf;
    f->layer.buf_area = (lv_area_t){ox, oy, ox + width - 1, oy + height - 1};
    f->task.target_layer = &f->layer;
    f->task.clip_area = f->layer.buf_area;
}

static uint8_t * pixel(fixture_t * f, int x, int y)
{
    return f->buf.data + y * f->stride + x * f->bytes;
}

static void fill(fixture_t * f, uint8_t v)
{
    for(int y = 0; y < f->height; y++) memset(pixel(f, 0, y), v, f->width * f->bytes);
}

static void save_before(fixture_t * f)
{
    memcpy(f->before, f->allocation, f->size);
}

static void check_guards(fixture_t * f)
{
    for(int i = 0; i < GUARD; i++) {
        CHECK(f->allocation[i] == 0xA5);
        CHECK(f->allocation[f->size - 1 - i] == 0xA5);
    }
    for(int y = 0; y < f->height; y++) {
        for(int i = f->width * f->bytes; i < f->stride; i++) CHECK(pixel(f, 0, y)[i] == 0xA5);
    }
}

static void fixture_free(fixture_t * f)
{
    check_guards(f);
    free(f->before);
    free(f->allocation);
}

static lv_draw_blur_dsc_t descriptor(int radius, int corner, uint8_t tint)
{
    lv_draw_blur_dsc_t dsc;
    lv_draw_blur_dsc_init(&dsc);
    dsc.blur_radius = radius;
    dsc.corner_radius = corner;
    dsc.quality = LV_BLUR_QUALITY_SPEED;
    dsc.tint = lv_color_white();
    dsc.tint_opa = tint;
    return dsc;
}

static void test_clipped_bounds(void)
{
    const lv_color_format_t formats[] = {LV_COLOR_FORMAT_A8, LV_COLOR_FORMAT_RGB565,
        LV_COLOR_FORMAT_RGB565_SWAPPED, LV_COLOR_FORMAT_RGB888, LV_COLOR_FORMAT_ARGB8888};
    const int sizes[] = {1, 2, 3, 7, 23};
    for(unsigned fmt = 0; fmt < sizeof(formats) / sizeof(formats[0]); fmt++) {
        for(unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            fixture_t f;
            fixture_init(&f, sizes[i], sizes[i] + 2, formats[fmt], 13, 27);
            fill(&f, formats[fmt] == LV_COLOR_FORMAT_A8 ? 255 : 0);
            save_before(&f);
            /* Both the shape and task clip may extend beyond the available band. */
            lv_area_t coords = {10, 22, 50, 60};
            f.task.clip_area = (lv_area_t){12, 28, 60, 29 + sizes[i]};
            lv_draw_blur_dsc_t dsc = descriptor(24, 0, 255);
            lv_draw_sw_blur(&f.task, &dsc, &coords);
            for(int y = 0; y < f.height; y++) {
                for(int x = 0; x < f.width; x++) {
                    for(int c = 0; c < f.bytes; c++) {
                        uint8_t original = f.before[GUARD + y * f.stride + x * f.bytes + c];
                        CHECK(pixel(&f, x, y)[c] == (y == 0 || c == 3 ? original : 255));
                    }
                }
            }
            fixture_free(&f);
        }
    }
}

static void test_averaging(void)
{
    for(int factor = 2; factor <= 4; factor++) {
        for(int phase = 0; phase < factor; phase++) {
            fixture_t f;
            fixture_init(&f, factor * 20, factor * 15, LV_COLOR_FORMAT_RGB888, 0, 0);
            for(int y = 0; y < f.height; y++) {
                for(int x = 0; x < f.width; x++) memset(pixel(&f, x, y), x % factor == phase ? 255 : 0, 3);
            }
            lv_draw_blur_dsc_t dsc = descriptor(factor == 2 ? 16 : 24, 0, 0);
            dsc.downsample_factor = factor;
            lv_draw_sw_blur(&f.task, &dsc, &f.layer.buf_area);
            for(int y = 0; y < f.height; y++) {
                for(int x = 0; x < f.width; x++) {
                    for(int c = 0; c < 3; c++) CHECK(pixel(&f, x, y)[c] == (factor == 2 ? 128 : factor == 3 ? 85 : 64));
                }
            }
            fixture_free(&f);
        }
    }
}

/* Independent 4x4 packed-cell expectation: high-frequency input must be
 * averaged rather than skipped. Check both byte orders and every Bayer phase. */
static void test_factor4_packed_average(void)
{
    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };
    for(unsigned swapped = 0; swapped < 2; swapped++) {
        for(int phase = 0; phase < 4; phase++) {
            fixture_t f;
            fixture_init(&f, 80, 60, swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565, 7, 13);
            for(int y = 0; y < f.height; y++) {
                for(int x = 0; x < f.width; x++) {
                    uint16_t value = x % 4 == phase ? 0xFFFF : 0;
                    memcpy(pixel(&f, x, y), &value, 2);
                }
            }
            lv_draw_blur_dsc_t dsc = descriptor(26, 0, 0);
            dsc.downsample_factor = 4;
            lv_draw_sw_blur(&f.task, &dsc, &f.layer.buf_area);
            for(int y = 0; y < f.height; y++) {
                for(int x = 0; x < f.width; x++) {
                    uint32_t threshold = bayer[(y + 13) & 3][(x + 7) & 3] * 16 + 8;
                    uint16_t expected = ((64 * 31 + threshold) / 255 << 11) |
                                        ((64 * 63 + threshold) / 255 << 5) |
                                        ((64 * 31 + threshold) / 255);
                    if(swapped) expected = (expected >> 8) | (expected << 8);
                    uint16_t actual;
                    memcpy(&actual, pixel(&f, x, y), 2);
                    CHECK(actual == expected);
                }
            }
            fixture_free(&f);
        }
    }
}

static void test_factor4_packed_reference(void)
{
    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };
    const int sizes[] = {1, 2, 3, 4, 7, 19, 37};
    for(unsigned swapped = 0; swapped < 2; swapped++) {
        for(unsigned size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
            fixture_t packed, reference;
            fixture_init(&packed, sizes[size], sizes[size] + 2,
                         swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565, -3, 5);
            fixture_init(&reference, packed.width, packed.height, LV_COLOR_FORMAT_RGB888, -3, 5);
            for(int y = 0; y < packed.height; y++) {
                for(int x = 0; x < packed.width; x++) {
                    uint16_t value = (uint16_t)(x * 5137 + y * 7919 + 0x6AD5);
                    uint8_t * rgb = pixel(&reference, x, y);
                    unsigned b = value & 31, g = (value >> 5) & 63, r = value >> 11;
                    rgb[0] = (b << 3) | (b >> 2);
                    rgb[1] = (g << 2) | (g >> 4);
                    rgb[2] = (r << 3) | (r >> 2);
                    if(swapped) value = (value >> 8) | (value << 8);
                    memcpy(pixel(&packed, x, y), &value, 2);
                }
            }
            lv_draw_blur_dsc_t dsc = descriptor(26, 0, 37);
            dsc.tint = lv_color_hex(0x426AB1);
            dsc.downsample_factor = 4;
            lv_draw_sw_blur(&reference.task, &dsc, &reference.layer.buf_area);
            lv_draw_sw_blur(&packed.task, &dsc, &packed.layer.buf_area);
            for(int y = 0; y < packed.height; y++) {
                for(int x = 0; x < packed.width; x++) {
                    const uint8_t * rgb = pixel(&reference, x, y);
                    uint32_t threshold = bayer[(y + 5) & 3][(x - 3) & 3] * 16 + 8;
                    uint16_t expected = ((rgb[2] * 31 + threshold) / 255 << 11) |
                                        ((rgb[1] * 63 + threshold) / 255 << 5) |
                                        ((rgb[0] * 31 + threshold) / 255);
                    if(swapped) expected = (expected >> 8) | (expected << 8);
                    uint16_t actual;
                    memcpy(&actual, pixel(&packed, x, y), 2);
                    CHECK(actual == expected);
                }
            }
            fixture_free(&packed);
            fixture_free(&reference);
        }
    }
}

static void test_interpolation(void)
{
    fixture_t f;
    fixture_init(&f, 90, 45, LV_COLOR_FORMAT_RGB888, 0, 0);
    for(int y = 0; y < f.height; y++) {
        for(int x = 0; x < f.width; x++) memset(pixel(&f, x, y), 2 * x, 3);
    }
    lv_draw_blur_dsc_t dsc = descriptor(24, 0, 0);
    lv_draw_sw_blur(&f.task, &dsc, &f.layer.buf_area);
    for(int x = 25; x < 65; x++) {
        int step = pixel(&f, x + 1, 22)[0] - pixel(&f, x, 22)[0];
        CHECK(step >= 1 && step <= 3);
    }
    fixture_free(&f);
}

static void test_a8_and_precise(void)
{
    const lv_blur_quality_t qualities[] = {LV_BLUR_QUALITY_AUTO, LV_BLUR_QUALITY_PRECISION,
                                         LV_BLUR_QUALITY_SPEED};
    for(unsigned i = 0; i < sizeof(qualities) / sizeof(qualities[0]); i++) {
        fixture_t a8, rgb;
        fixture_init(&a8, 31, 17, LV_COLOR_FORMAT_A8, 13, 27);
        fixture_init(&rgb, 31, 17, LV_COLOR_FORMAT_RGB888, 13, 27);
        for(int y = 0; y < a8.height; y++) {
            for(int x = 0; x < a8.width; x++) {
                uint8_t v = (x * 23 + y * 31) & 255;
                *pixel(&a8, x, y) = v;
                memset(pixel(&rgb, x, y), v, 3);
            }
        }
        lv_draw_blur_dsc_t dsc = descriptor(11, 8, 0);
        dsc.quality = qualities[i];
        lv_draw_sw_blur(&a8.task, &dsc, &a8.layer.buf_area);
        lv_draw_sw_blur(&rgb.task, &dsc, &rgb.layer.buf_area);
        for(int y = 0; y < a8.height; y++) {
            for(int x = 0; x < a8.width; x++) {
                for(int c = 0; c < 3; c++) CHECK(*pixel(&a8, x, y) == pixel(&rgb, x, y)[c]);
            }
        }
        save_before(&rgb);
        dsc.blur_radius = 0;
        dsc.tint_opa = 255;
        lv_draw_sw_blur(&rgb.task, &dsc, &rgb.layer.buf_area);
        CHECK(memcmp(rgb.before, rgb.allocation, rgb.size) == 0);
        fixture_free(&a8);
        fixture_free(&rgb);
    }
}

static void test_round_mask(void)
{
    fixture_t f;
    fixture_init(&f, 37, 21, LV_COLOR_FORMAT_RGB888, 13, 27);
    fill(&f, 0);
    lv_draw_blur_dsc_t dsc = descriptor(24, LV_RADIUS_CIRCLE, 255);
    lv_draw_sw_blur(&f.task, &dsc, &f.layer.buf_area);
    int partial = 0;
    for(int y = 0; y < f.height; y++) {
        for(int x = 0; x < f.width; x++) {
            int v = pixel(&f, x, y)[0];
            partial += v > 0 && v < 255;
            CHECK(v == pixel(&f, f.width - 1 - x, y)[0]);
            CHECK(v == pixel(&f, x, f.height - 1 - y)[0]);
            CHECK(v == pixel(&f, x, y)[1] && v == pixel(&f, x, y)[2]);
        }
    }
    CHECK(partial >= 16);
    CHECK(pixel(&f, 0, 0)[0] == 0);
    CHECK(pixel(&f, 18, 10)[0] == 255);
    fixture_free(&f);
}

static void test_format_equivalence(void)
{
    fixture_t native, swapped, argb;
    fixture_init(&native, 39, 25, LV_COLOR_FORMAT_RGB565, 11, 21);
    fixture_init(&swapped, 39, 25, LV_COLOR_FORMAT_RGB565_SWAPPED, 11, 21);
    fixture_init(&argb, 39, 25, LV_COLOR_FORMAT_ARGB8888, 11, 21);
    for(int y = 0; y < native.height; y++) {
        for(int x = 0; x < native.width; x++) {
            uint16_t rgb = (x * 129 + y * 253) & 0xFFFF;
            uint16_t reversed = (rgb >> 8) | (rgb << 8);
            memcpy(pixel(&native, x, y), &rgb, 2);
            memcpy(pixel(&swapped, x, y), &reversed, 2);
            uint8_t * p = pixel(&argb, x, y);
            p[0] = x * 3; p[1] = y * 5; p[2] = (x + y) * 2; p[3] = (x * 7 + y * 17) & 255;
        }
    }
    save_before(&argb);
    lv_draw_blur_dsc_t dsc = descriptor(25, 9, 67);
    lv_draw_sw_blur(&native.task, &dsc, &native.layer.buf_area);
    lv_draw_sw_blur(&swapped.task, &dsc, &swapped.layer.buf_area);
    lv_draw_sw_blur(&argb.task, &dsc, &argb.layer.buf_area);
    for(int y = 0; y < native.height; y++) {
        for(int x = 0; x < native.width; x++) {
            CHECK(pixel(&native, x, y)[0] == pixel(&swapped, x, y)[1]);
            CHECK(pixel(&native, x, y)[1] == pixel(&swapped, x, y)[0]);
            CHECK(pixel(&argb, x, y)[3] == argb.before[GUARD + y * argb.stride + x * 4 + 3]);
        }
    }
    fixture_free(&native);
    fixture_free(&swapped);
    fixture_free(&argb);
}

static void test_dither_band_origin(void)
{
    fixture_t full, band;
    fixture_init(&full, 31, 21, LV_COLOR_FORMAT_RGB565, 0, 0);
    fixture_init(&band, 31, 9, LV_COLOR_FORMAT_RGB565, 0, 7);
    fill(&full, 0);
    fill(&band, 0);
    lv_draw_blur_dsc_t dsc = descriptor(24, 0, 255);
    dsc.tint = lv_color_hex(0x121212);
    lv_draw_sw_blur(&full.task, &dsc, &full.layer.buf_area);
    lv_draw_sw_blur(&band.task, &dsc, &full.layer.buf_area);
    int different = 0;
    for(int y = 0; y < band.height; y++) {
        for(int x = 0; x < band.width; x++) {
            CHECK(memcmp(pixel(&full, x, y + 7), pixel(&band, x, y), 2) == 0);
            different += memcmp(pixel(&band, x, y), pixel(&band, 0, 0), 2) != 0;
        }
    }
    CHECK(different > 0);
    fixture_free(&full);
    fixture_free(&band);
}

static void write_preview(const char * path)
{
    fixture_t f;
    fixture_init(&f, 240, 120, LV_COLOR_FORMAT_RGB565, 0, 0);
    for(int y = 0; y < f.height; y++) {
        for(int x = 0; x < f.width; x++) {
            int grid = (x / 2 + y / 2) & 1;
            uint16_t rgb = ((x * 31 / 239) << 11) | ((y * 63 / 119) << 5) | (grid ? 31 : 0);
            memcpy(pixel(&f, x, y), &rgb, 2);
        }
    }
    lv_draw_blur_dsc_t dsc = descriptor(24, LV_RADIUS_CIRCLE, 32);
    const lv_area_t coords = {21, 19, 218, 100};
    lv_draw_sw_blur(&f.task, &dsc, &coords);
    FILE * out = fopen(path, "wb");
    CHECK(out != NULL);
    if(out) {
        fprintf(out, "P6\n%d %d\n255\n", f.width, f.height);
        for(int y = 0; y < f.height; y++) {
            for(int x = 0; x < f.width; x++) {
                uint16_t rgb;
                memcpy(&rgb, pixel(&f, x, y), 2);
                uint8_t color[3] = {(rgb >> 11) * 255 / 31, ((rgb >> 5) & 63) * 255 / 63, (rgb & 31) * 255 / 31};
                fwrite(color, 1, 3, out);
            }
        }
        fclose(out);
    }
    fixture_free(&f);
}

int main(int argc, char ** argv)
{
    lv_init();
    if(argc < 3) {
        test_clipped_bounds();
        test_averaging();
        test_factor4_packed_average();
        test_factor4_packed_reference();
        test_interpolation();
        test_a8_and_precise();
        test_round_mask();
        test_format_equivalence();
        test_dither_band_origin();
    }
    else if(strcmp(argv[2], "resampling") == 0) {
        test_averaging();
        test_factor4_packed_average();
        test_factor4_packed_reference();
        test_interpolation();
        test_round_mask();
    }
    if(argc > 1) write_preview(argv[1]);
    printf("blur: %d checks, %d failures\n", s_checks, s_failed);
    lv_deinit();
    return s_failed ? 1 : 0;
}
