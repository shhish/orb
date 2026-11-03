/* The existing full downsample/filter/reconstruct renderer is the oracle.
 * Uniform RGB565 inputs exercise every Bayer phase, clipped rounded support,
 * tint/opacity and byte order; one changed pixel must reject the fast path. */
#define main blur_regressions_main
#include "../../firmware/components/orb_ui/tests/host/test_blur.c"
#undef main

#if defined(CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST) && CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST
static uint32_t s_random = 0x926F34A7;
static uint32_t next_random(void)
{
    s_random ^= s_random << 13;
    s_random ^= s_random >> 17;
    s_random ^= s_random << 5;
    return s_random;
}

static uint64_t test_band_regions(void)
{
    uint64_t compared = 0;
    /* A control's clipped backdrop is the proof region. A neighbour, another
     * band, padding and pixels outside the draw task's clip cannot disqualify
     * it. Conversely, every pixel and byte within the region must be checked. */
    for(unsigned swapped = 0; swapped < 2; swapped++) {
        for(int changed = -1; changed < 17 * 13; changed++) {
            fixture_t f;
            fixture_init(&f, 31, 19, swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565,
                         61, 84);
            fill(&f, 0x2B);
            for(int y = 3; y < 16; y++) memset(pixel(&f, 5, y), 0, 17 * 2);
            if(changed >= 0) pixel(&f, 5 + changed % 17, 3 + changed / 17)[changed & 1] = 0x20;
            /* Odd X and odd-width stride exercise unaligned word probes. */
            f.task.clip_area = (lv_area_t){66, 87, 82, 99};
            lv_area_t shape = {64, 65, 85, 136};
            lv_draw_blur_dsc_t dsc = descriptor(24, 11, 80);
            dsc.tint = lv_color_hex(0x1C1C1E);
            save_before(&f);
            lv_draw_sw_material_stats_t before, after;
            lv_draw_sw_material_get_stats(&before);
            lv_draw_sw_blur_set_solid_enabled(true);
            lv_draw_sw_blur(&f.task, &dsc, &shape);
            lv_draw_sw_material_get_stats(&after);
            uint8_t *fast = malloc(f.size);
            CHECK(fast != NULL);
            memcpy(fast, f.allocation, f.size);
            memcpy(f.allocation, f.before, f.size);
            lv_draw_sw_blur_set_solid_enabled(false);
            lv_draw_sw_blur(&f.task, &dsc, &shape);
            CHECK(!memcmp(fast, f.allocation, f.size));
            CHECK(after.solid_fills - before.solid_fills == (changed < 0 ? 1 : 0));
            compared += f.size;
            free(fast);
            fixture_free(&f);
        }
    }
    return compared;
}

static void test_moving_solid_reuse(void)
{
    const lv_draw_sw_blur_cache_config_t off = {0};
    const lv_draw_sw_blur_cache_config_t on = {.budget_bytes = 64 * 1024, .max_entry_bytes = 64 * 1024};
    for(unsigned swapped = 0; swapped < 2; swapped++) for(unsigned phase = 0; phase < 16; phase++) {
        fixture_t f;
        fixture_init(&f, 97, 79, swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565,
                     53 + phase % 4, 83 + phase / 4);
        fill(&f, 0);
        lv_area_t shape = f.layer.buf_area;
        if(phase & 1) { shape.x1 -= 3; shape.y2 += 4; }
        lv_draw_blur_dsc_t dsc = descriptor(24, 36, 83);
        dsc.tint = lv_color_hex(0x1C1C1E);
        dsc.backdrop_moving = true;
        save_before(&f);
        uint8_t *expected = malloc(f.size); CHECK(expected != NULL);
        CHECK(lv_draw_sw_blur_cache_configure(&off) == LV_RESULT_OK);
        lv_draw_sw_blur_set_solid_enabled(false);
        lv_draw_sw_blur(&f.task, &dsc, &shape);
        memcpy(expected, f.allocation, f.size);
        CHECK(lv_draw_sw_blur_cache_configure(&on) == LV_RESULT_OK);
        lv_draw_sw_blur_set_solid_enabled(true);
        for(unsigned repeat = 0; repeat < 3; repeat++) {
            memcpy(f.allocation, f.before, f.size);
            lv_draw_sw_blur(&f.task, &dsc, &shape);
            CHECK(!memcmp(expected, f.allocation, f.size));
            lv_draw_sw_blur_cache_stats_t stats;
            lv_draw_sw_blur_cache_get_stats(&stats);
            CHECK(stats.misses == 1 && stats.hits == repeat && stats.leased_entries == 0);
            CHECK(stats.bytes <= on.budget_bytes);
        }
        /* A single moving glyph pixel must leave the solid path. An existing
         * warm black entry must never hide that newly revealed content. */
        memcpy(f.allocation, f.before, f.size);
        pixel(&f, 11, 23)[0] ^= 0x20;
        memcpy(f.before, f.allocation, f.size);
        lv_draw_sw_blur(&f.task, &dsc, &shape);
        memcpy(expected, f.allocation, f.size);
        lv_draw_sw_blur_cache_stats_t stats;
        lv_draw_sw_blur_cache_get_stats(&stats);
        CHECK(stats.hits == 2 && stats.leased_entries == 0);
        CHECK(lv_draw_sw_blur_cache_configure(&off) == LV_RESULT_OK);
        memcpy(f.allocation, f.before, f.size);
        lv_draw_sw_blur_set_solid_enabled(false);
        lv_draw_sw_blur(&f.task, &dsc, &shape);
        CHECK(!memcmp(expected, f.allocation, f.size));
        free(expected); fixture_free(&f);
    }
    lv_draw_sw_blur_set_solid_enabled(true);
}

int main(void)
{
    lv_init();
    test_moving_solid_reuse();
    uint64_t compared = test_band_regions();
    unsigned uniform = 0, nonuniform = 0;
    for(unsigned i = 0; i < 1536; i++) {
        fixture_t f;
        int width = 1 + next_random() % 127;
        int height = 1 + next_random() % 101;
        bool swapped = i & 1;
        fixture_init(&f, width, height, swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565,
                     (int)(i % 17) - 8, (int)(i % 19) - 9);
        uint16_t value = i % 3 ? next_random() : 0;
        if(swapped) value = lv_color_swap_16(value);
        for(int y = 0; y < height; y++) {
            for(int x = 0; x < width; x++) memcpy(pixel(&f, x, y), &value, 2);
        }
        lv_area_t shape = f.layer.buf_area;
        if(i % 4 == 0) lv_area_increase(&shape, 7, 5);
        if(i % 7 == 0 && width > 5 && height > 5) {
            f.task.clip_area.x1++;
            f.task.clip_area.y1++;
            f.task.clip_area.x2 -= 2;
            f.task.clip_area.y2 -= 2;
        }
        bool changed = i % 5 == 0 && lv_area_get_size(&f.task.clip_area) > 1;
        if(changed) pixel(&f, f.task.clip_area.x2 - f.layer.buf_area.x1,
                         f.task.clip_area.y2 - f.layer.buf_area.y1)[0] ^= 0x20;
        lv_draw_blur_dsc_t dsc = descriptor(1 + i % 43, i % 3 ? LV_RADIUS_CIRCLE : 0, i % 256);
        dsc.quality = i % 3;
        dsc.tint = lv_color_hex(next_random());
        dsc.downsample_factor = i % 4 ? 0 : 4;
        save_before(&f);
        lv_draw_sw_material_stats_t before, after;
        lv_draw_sw_material_get_stats(&before);
        lv_draw_sw_blur_set_solid_enabled(true);
        lv_draw_sw_blur(&f.task, &dsc, &shape);
        lv_draw_sw_material_get_stats(&after);
        uint8_t * fast = malloc(f.size);
        CHECK(fast != NULL);
        memcpy(fast, f.allocation, f.size);
        memcpy(f.allocation, f.before, f.size);
        lv_draw_sw_blur_set_solid_enabled(false);
        lv_draw_sw_blur(&f.task, &dsc, &shape);
        if(memcmp(fast, f.allocation, f.size)) {
            fprintf(stderr, "solid mismatch fixture %u, %dx%d, value %u\n", i, width, height, value);
        }
        CHECK(!memcmp(fast, f.allocation, f.size));
        CHECK(after.solid_fills - before.solid_fills == (changed ? 0 : 1));
        uniform += !changed;
        nonuniform += changed;
        compared += f.size;
        free(fast);
        fixture_free(&f);
    }
    lv_draw_sw_blur_set_solid_enabled(true);
    lv_deinit();
    printf("solid blur: 444 band-region + %u uniform + %u single-pixel rejection fixtures, %llu exact bytes, %d checks, %d failures\n",
           uniform, nonuniform, (unsigned long long)compared, s_checks, s_failed);
    return s_failed ? 1 : 0;
}
#else
int main(void)
{
    puts("solid blur: option disabled");
    return 0;
}
#endif
