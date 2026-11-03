/* Pixel-exact guard for the optimized blur. The golden hashes were captured
 * from the smooth-AA implementation at 2e4d8e6, before optimization, on the
 * little-endian RGB565 layout used by the ESP32 and Mac simulator.
 * Reuse the guarded fixtures and keep the original regression entry point. */
#define main blur_regressions_main
#include "test_blur.c"
#undef main

static uint32_t s_random = 0x12345678;

static uint32_t next_random(void)
{
    s_random ^= s_random << 13;
    s_random ^= s_random >> 17;
    s_random ^= s_random << 5;
    return s_random;
}

int main(void)
{
    const lv_color_format_t formats[] = {LV_COLOR_FORMAT_A8, LV_COLOR_FORMAT_RGB565,
        LV_COLOR_FORMAT_RGB565_SWAPPED, LV_COLOR_FORMAT_RGB888, LV_COLOR_FORMAT_ARGB8888};
    const uint64_t expected[] = {
        UINT64_C(0x8743eb61c5ae0f8e), UINT64_C(0xd0dc0be672ec6b32), UINT64_C(0x3b9c6200d7d08c83),
        UINT64_C(0x840b55f8ce8feee0), UINT64_C(0x40982154d1b183cb), UINT64_C(0x355b5d5249452f0d),
        UINT64_C(0x11c1a0a70d9e7176), UINT64_C(0x11260fb44301d338), UINT64_C(0x417a2b5c3a2dd8db),
        UINT64_C(0xbc9657f32c19f825), UINT64_C(0x25dbedffcab9edbc), UINT64_C(0x7ff3022941ca9717),
        UINT64_C(0x605f160666f00ee8), UINT64_C(0x9c82f8e825268894), UINT64_C(0x7c750e6af439546d)
    };
    uint64_t hashes[15];
    for(int i = 0; i < 15; i++) hashes[i] = UINT64_C(14695981039346656037);
    size_t bytes = 0;
    lv_init();
    for(int i = 0; i < 800; i++) {
        fixture_t f;
        int width = 1 + next_random() % 127;
        int height = 1 + next_random() % 101;
        int ox = (int)(next_random() % 47) - 23;
        int oy = (int)(next_random() % 47) - 23;
        fixture_init(&f, width, height, formats[i % 5], ox, oy);
        for(int y = 0; y < height; y++) {
            for(int x = 0; x < width * f.bytes; x++) pixel(&f, 0, y)[x] = next_random();
        }
        lv_area_t shape = f.layer.buf_area;
        if(i % 3 == 0) {
            shape.x1 -= 3;
            shape.y1 -= 2;
            shape.x2 += 4;
            shape.y2 += 5;
        }
        if(i % 7 == 0 && width > 4 && height > 4) {
            f.task.clip_area.x1++;
            f.task.clip_area.y1++;
            f.task.clip_area.x2 -= 2;
            f.task.clip_area.y2 -= 2;
        }
        /* Sequence random draws explicitly: C argument order is unspecified. */
        int radius = 1 + next_random() % 45;
        int corner = i % 4 == 0 ? 0 : (i % 4 == 1 ? LV_RADIUS_CIRCLE : 1 + next_random() % 31);
        uint8_t tint = next_random();
        lv_draw_blur_dsc_t dsc = descriptor(radius, corner, tint);
        dsc.tint = lv_color_hex(next_random());
        dsc.quality = (lv_blur_quality_t)(i % 3);
        lv_draw_sw_blur(&f.task, &dsc, &shape);
        unsigned bucket = (i % 5) * 3 + i % 3;
        for(size_t j = 0; j < f.size; j++) {
            hashes[bucket] ^= f.allocation[j];
            hashes[bucket] *= UINT64_C(1099511628211);
        }
        bytes += f.size;
        fixture_free(&f);
    }
    for(int i = 0; i < 15; i++) {
        if(hashes[i] != expected[i]) fprintf(stderr, "blur changed: format %d, quality %d\n", formats[i / 3], i % 3);
        CHECK(hashes[i] == expected[i]);
    }
    printf("blur exact: 800 fixtures, %zu bytes, %d checks, %d failures\n", bytes, s_checks, s_failed);
    lv_deinit();
    return s_failed ? 1 : 0;
}
