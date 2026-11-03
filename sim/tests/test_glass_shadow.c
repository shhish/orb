/* Black A8 glass masks must not turn neutral RGB565 backdrops green.
 * Exercise the real image path, including clipping, stride and both byte orders. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw.h"

static unsigned s_checks;
#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

enum { W = 64, H = 256, PAD = 16, GUARD = 32, STRIDE = W * 2 + PAD };
static uint8_t s_pixels[GUARD + H * STRIDE + GUARD];
static uint8_t s_mask[H * (W + PAD)];

static uint16_t load_pixel(int x, int y, bool swapped)
{
    uint16_t p;
    memcpy(&p, s_pixels + GUARD + y * STRIDE + x * 2, 2);
    return swapped ? lv_color_swap_16(p) : p;
}

static void store_pixel(int x, int y, bool swapped, uint16_t p)
{
    if (swapped) p = lv_color_swap_16(p);
    memcpy(s_pixels + GUARD + y * STRIDE + x * 2, &p, 2);
}

static void draw_image(bool swapped, bool neutral, uint8_t opa, bool clipped,
                        lv_color_t color, const uint16_t *spans)
{
    lv_draw_buf_t dest, mask;
    lv_color_format_t cf = swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
    CHECK(lv_draw_buf_init(&dest, W, H, cf, STRIDE, s_pixels + GUARD, H * STRIDE) == LV_RESULT_OK);
    CHECK(lv_draw_buf_init(&mask, W, H, LV_COLOR_FORMAT_A8, W + PAD, s_mask, sizeof(s_mask)) == LV_RESULT_OK);
    /* Use exactly the allocated A8 image route that orb_glass uses. */
    mask.header.flags |= LV_IMAGE_FLAGS_ALLOCATED;
    lv_layer_t layer = { .draw_buf = &dest, .color_format = cf, .buf_area = {13, 27, 13 + W - 1, 27 + H - 1} };
    lv_draw_task_t task = { .target_layer = &layer, .clip_area = layer.buf_area };
    if (clipped) task.clip_area = (lv_area_t){16, 29, 13 + W - 4, 27 + H - 3};
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = &mask;
    dsc.recolor = color;
#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
    dsc.a8_spans = spans;
#else
    LV_UNUSED(spans);
#endif
    dsc.rgb565_neutral_shadow = neutral;
    dsc.opa = opa;
    dsc.image_area = layer.buf_area;
    lv_draw_sw_image(&task, &dsc, &layer.buf_area);
}

static void draw(bool swapped, bool neutral, uint8_t opa, bool clipped)
{
    draw_image(swapped, neutral, opa, clipped, lv_color_black(), NULL);
}

static uint16_t grey(int index)
{
    unsigned level = index / 2;
    return (uint16_t)((level << 11) | ((2 * level + (index & 1)) << 5) | level);
}

static uint16_t expected(uint16_t original, uint8_t alpha, uint8_t opa, bool neutral)
{
    if (opa <= LV_OPA_MIN) return original;
    if (opa < LV_OPA_MAX) alpha = LV_OPA_MIX2(alpha, opa);
    uint16_t result = lv_color_16_16_mix(0, original, alpha);
    unsigned r = original >> 11, g = (original >> 5) & 63, b = original & 31;
    if (neutral && result != original && r == b && (g == 2 * r || g == 2 * r + 1)) {
        unsigned level = result & 31;
        result = (uint16_t)((level << 11) | (((level << 1) | (level >> 4)) << 5) | level);
    }
    return result;
}

static void check_guards(void)
{
    for (int i = 0; i < GUARD; i++) {
        CHECK(s_pixels[i] == 0xa5);
        CHECK(s_pixels[sizeof(s_pixels) - 1 - i] == 0xa5);
    }
    for (int y = 0; y < H; y++) {
        for (int x = W * 2; x < STRIDE; x++) CHECK(s_pixels[GUARD + y * STRIDE + x] == 0xa5);
    }
}

static void test_greys(void)
{
    const uint8_t opacities[] = {0, 2, 3, 77, 128, 252, 253, 255};
    unsigned corrected = 0;
    for (int swapped = 0; swapped <= 1; swapped++) {
        for (unsigned o = 0; o < sizeof(opacities); o++) {
            for (int clipped = 0; clipped <= 1; clipped++) {
                memset(s_pixels, 0xa5, sizeof(s_pixels));
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        store_pixel(x, y, swapped, grey(x));
                        s_mask[y * (W + PAD) + x] = (uint8_t)y;
                    }
                }
                draw(swapped, true, opacities[o], clipped);
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        uint16_t original = grey(x);
                        bool inside = !clipped || (x >= 3 && x < W - 3 && y >= 2 && y < H - 2);
                        uint16_t want = inside ? expected(original, y, opacities[o], true) : original;
                        uint16_t got = load_pixel(x, y, swapped);
                        CHECK(got == want);
                        CHECK((got >> 11) == (got & 31));
                        CHECK((got >> 11) <= (original >> 11));
                        CHECK(((got >> 5) & 63) <= ((original >> 5) & 63));
                        if (inside && got != expected(original, y, opacities[o], false)) corrected++;
                    }
                }
                check_guards();
            }
        }
    }
    CHECK(corrected > 1000); /* The corpus must exercise the original green bias. */
    printf("shadow: %u neutral corrections across the alpha/grey corpus\n", corrected);
}

static void test_ordinary_path(void)
{
    for (int swapped = 0; swapped <= 1; swapped++) {
        memset(s_pixels, 0xa5, sizeof(s_pixels));
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                store_pixel(x, y, swapped, grey(x));
                s_mask[y * (W + PAD) + x] = y;
            }
        }
        draw(swapped, false, 255, false);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                CHECK(load_pixel(x, y, swapped) == lv_color_16_16_mix(0, grey(x), y));
            }
        }
        check_guards();
    }
}

static void test_colors(void)
{
    /* Every RGB565 color, at several alpha values, must retain its ordinary
     * blend unless it is a neutral input covered by the documented opt-in. */
    const uint8_t alphas[] = {0, 3, 31, 46, 77, 128, 252, 255};
    for (int swapped = 0; swapped <= 1; swapped++) {
        for (unsigned a = 0; a < sizeof(alphas); a++) {
            for (unsigned chunk = 0; chunk < 4; chunk++) {
                memset(s_pixels, 0xa5, sizeof(s_pixels));
                memset(s_mask, alphas[a], sizeof(s_mask));
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) store_pixel(x, y, swapped, chunk * W * H + y * W + x);
                }
                draw(swapped, true, 255, false);
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        uint16_t original = chunk * W * H + y * W + x;
                        CHECK(load_pixel(x, y, swapped) == expected(original, alphas[a], 255, true));
                    }
                }
                check_guards();
            }
        }
    }
}

static void test_black_fills(void)
{
    const int widths[] = {1, 3, 31, 64};
    const int heights[] = {1, 5, 84};
    const uint8_t opacities[] = {0, 3, 128, 252, 253, 255};
    for (int swapped = 0; swapped <= 1; swapped++) {
        for (unsigned wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
            for (unsigned hi = 0; hi < sizeof(heights) / sizeof(heights[0]); hi++) {
                for (int padded = 0; padded <= 1; padded++) {
                    int width = widths[wi], height = heights[hi];
                    int stride = width * 2 + (padded ? PAD : 0);
                    /* Shift padded buffers by one pixel to exercise word-unaligned
                     * destinations as well as the contiguous whole-band clear. */
                    int prefix = GUARD + (padded ? 2 : 0);
                    size_t size = prefix + height * stride + GUARD;
                    uint8_t *memory = malloc(size);
                    CHECK(memory != NULL);
                    for (unsigned oi = 0; oi < sizeof(opacities); oi++) {
                        for (int clipped = 0; clipped <= 1; clipped++) {
                            memset(memory, 0xa5, size);
                            for (int y = 0; y < height; y++) {
                                for (int x = 0; x < width; x++) {
                                    uint16_t original = (uint16_t)(x * 977 + y * 37 + 1);
                                    uint16_t raw = swapped ? lv_color_swap_16(original) : original;
                                    memcpy(memory + prefix + y * stride + x * 2, &raw, 2);
                                }
                            }
                            lv_color_format_t cf = swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
                            lv_draw_buf_t buffer;
                            CHECK(lv_draw_buf_init(&buffer, width, height, cf, stride,
                                                  memory + prefix, height * stride) == LV_RESULT_OK);
                            lv_area_t area = {13, 27, 13 + width - 1, 27 + height - 1};
                            lv_layer_t layer = {.draw_buf = &buffer, .color_format = cf, .buf_area = area};
                            lv_draw_task_t task = {.target_layer = &layer, .clip_area = area};
                            if (clipped && width > 2) { task.clip_area.x1++; task.clip_area.x2--; }
                            if (clipped && height > 2) { task.clip_area.y1++; task.clip_area.y2--; }
                            lv_draw_fill_dsc_t dsc;
                            lv_draw_fill_dsc_init(&dsc);
                            dsc.color = lv_color_black();
                            dsc.opa = opacities[oi];
                            lv_draw_sw_fill(&task, &dsc, &area);
                            for (int y = 0; y < height; y++) {
                                for (int x = 0; x < width; x++) {
                                    uint16_t raw;
                                    memcpy(&raw, memory + prefix + y * stride + x * 2, 2);
                                    uint16_t got = swapped ? lv_color_swap_16(raw) : raw;
                                    uint16_t original = (uint16_t)(x * 977 + y * 37 + 1);
                                    bool inside = x + 13 >= task.clip_area.x1 && x + 13 <= task.clip_area.x2 &&
                                                  y + 27 >= task.clip_area.y1 && y + 27 <= task.clip_area.y2;
                                    uint16_t want = original;
                                    if (inside && dsc.opa > LV_OPA_MIN) {
                                        want = dsc.opa >= LV_OPA_MAX ? 0 : lv_color_16_16_mix(0, original, dsc.opa);
                                    }
                                    CHECK(got == want);
                                }
                                for (int x = width * 2; x < stride; x++) {
                                    CHECK(memory[prefix + y * stride + x] == 0xa5);
                                }
                            }
                            for (int i = 0; i < prefix; i++) CHECK(memory[i] == 0xa5);
                            for (int i = 0; i < GUARD; i++) CHECK(memory[size - 1 - i] == 0xa5);
                        }
                    }
                    free(memory);
                }
            }
        }
    }
}

static void test_other_primitive_metadata(void)
{
    /* A generic primitive must never inherit an A8 span pointer or neutral
     * shadow flag from another draw's stack descriptor. */
    for (int swapped = 0; swapped <= 1; swapped++) {
        memset(s_pixels, 0xa5, sizeof(s_pixels));
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) store_pixel(x, y, swapped, 0);
        lv_color_format_t cf = swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
        lv_draw_buf_t dest;
        CHECK(lv_draw_buf_init(&dest, W, H, cf, STRIDE, s_pixels + GUARD, H * STRIDE) == LV_RESULT_OK);
        lv_layer_t layer = {.draw_buf = &dest, .color_format = cf, .buf_area = {13, 27, 13 + W - 1, 27 + H - 1}};
        lv_draw_task_t task = {.target_layer = &layer, .clip_area = layer.buf_area};
        lv_draw_triangle_dsc_t dsc;
        lv_draw_triangle_dsc_init(&dsc);
        dsc.p[0] = (lv_point_precise_t){23, 37};
        dsc.p[1] = (lv_point_precise_t){23, 77};
        dsc.p[2] = (lv_point_precise_t){63, 57};
        dsc.color = lv_color_hex(0x0091ff);
        lv_draw_sw_triangle(&task, &dsc);
        CHECK(load_pixel(30, 30, swapped) == lv_color_to_u16(dsc.color));
        CHECK(load_pixel(0, 0, swapped) == 0);
        check_guards();
    }
}

#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
static void test_sparse_exact(void)
{
    uint8_t original[sizeof(s_pixels)], reference[sizeof(s_pixels)];
    uint16_t spans[H * 4];
    const uint8_t opacities[] = {0, 2, 3, 17, 128, 252, 253, 255};
    for (int swapped = 0; swapped <= 1; swapped++) {
        memset(s_pixels, 0xa5, sizeof(s_pixels));
        memset(s_mask, 0, sizeof(s_mask));
        for (int y = 0; y < H; y++) {
            /* Empty rows, single pixels, two spans and moving AA coverage:
             * stride padding remains untouched in both mask and destination. */
            unsigned x1 = y % 19, x2 = x1 + (y % 9), x3 = 37 + y % 7, x4 = x3 + y % 17;
            if (y % 13 == 0) x1 = x2 = x3 = x4 = 0;
            spans[4 * y] = x1; spans[4 * y + 1] = x2;
            spans[4 * y + 2] = x3; spans[4 * y + 3] = x4;
            for (int x = 0; x < W; x++) {
                uint16_t value = y & 1 ? grey(x) : (uint16_t)(y * 257 + x * 977);
                store_pixel(x, y, swapped, value);
                if ((x >= x1 && x < x2) || (x >= x3 && x < x4))
                    s_mask[y * (W + PAD) + x] = (uint8_t)(y + x * 13);
            }
        }
        memcpy(original, s_pixels, sizeof(original));
        for (unsigned oi = 0; oi < sizeof(opacities); oi++) {
            for (int color = 0; color < 3; color++) {
                lv_color_t c = color == 0 ? lv_color_black() : color == 1 ? lv_color_white() : lv_color_hex(0x0091ff);
                for (int clipped = 0; clipped <= 1; clipped++) {
                    memcpy(s_pixels, original, sizeof(original));
                    draw_image(swapped, color == 0, opacities[oi], clipped, c, NULL);
                    memcpy(reference, s_pixels, sizeof(reference));
                    memcpy(s_pixels, original, sizeof(original));
                    draw_image(swapped, color == 0, opacities[oi], clipped, c, spans);
                    CHECK(memcmp(reference, s_pixels, sizeof(reference)) == 0);
                    check_guards();
                }
            }
        }
    }
}
#endif

int main(void)
{
    lv_init();
    test_greys();
    test_ordinary_path();
    test_colors();
    test_black_fills();
    test_other_primitive_metadata();
#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
    test_sparse_exact();
#endif
    lv_deinit();
    printf("glass shadow: %u checks passed\n", s_checks);
    return 0;
}
