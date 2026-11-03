/* Exercise the enabled path and its cold/warm/lifetime contracts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_demo.h"
#include "orb_ui.h"
#include "sim_display.h"
#include "src/core/lv_obj_rows_private.h"
#include "src/core/lv_obj_rows_average.h"
#include "src/core/lv_obj_rows_blend.h"
#include "src/draw/lv_draw_private.h"

static uint32_t s_now;
static unsigned s_checks;
static uint8_t s_partial[466 * 466 * 3], s_full[466 * 466 * 3];
static unsigned s_allocations, s_remaining;
static uint32_t tick(void) { return s_now; }
#define CHECK(c) do { s_checks++; if(!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while(0)

static void * limited_alloc(size_t bytes, lv_color_format_t cf)
{
    LV_UNUSED(cf);
    if(!s_remaining) return NULL;
    s_remaining--;
    void * p = malloc(bytes);
    if(p) s_allocations++;
    return p;
}
static void limited_free(void * p) { if(p) { s_allocations--; free(p); } }
static void custom_draw(lv_event_t * event) { LV_UNUSED(event); }

static void check_exact_area_average(void)
{
    for(unsigned value = 0; value <= 65152; value++)
        CHECK(lv_obj_rows_divide_255(value) == value / 255);
    for(unsigned a = 0; a <= 255; a++) for(unsigned b = 0; b <= 255; b++)
        CHECK(lv_obj_rows_mul255(a, b) == (a * b + 127) / 255);
    for(unsigned dst = 0; dst < 64; dst++) for(unsigned src = 0; src < 64; src++)
        for(unsigned a = 0; a <= 255; a++) {
            uint16_t d = ((dst & 31) << 11) | (dst << 5) | (dst & 31);
            uint16_t s = ((src & 31) << 11) | (src << 5) | (src & 31);
            unsigned z = 255 - a;
            uint16_t expected = (((s >> 11) * a + (d >> 11) * z + 127) / 255 << 11) |
                (((((s >> 5) & 63) * a + ((d >> 5) & 63) * z + 127) / 255) << 5) |
                (((s & 31) * a + (d & 31) * z + 127) / 255);
            CHECK(lv_obj_rows_blend565(d, s, a) == expected);
        }
    static uint8_t source[128 * 128];
    const unsigned dimensions[] = {1, 2, 3, 5, 17, 64, 97};
    /* Verify the native-width bound even for tiny and maximum font metrics,
     * then compare the production filter to independent 64-bit integration. */
    for(unsigned width = 1; width <= UINT16_MAX; width++) {
        unsigned scaled = LV_MAX(1u, (width * 136u + 128) / 256);
        CHECK((width * 256u + scaled - 1) / scaled <= 512);
    }
    for(unsigned pattern = 0; pattern < 4; pattern++) {
        for(unsigned y = 0; y < 128; y++) for(unsigned x = 0; x < 128; x++)
            source[y * 128 + x] = pattern == 0 ? 0 : pattern == 1 ? 255 :
                pattern == 2 ? ((x + y) & 1) * 255 : (x * 37 + y * 83 + x * y * 11) & 255;
        for(unsigned wi = 0; wi < sizeof(dimensions) / sizeof(dimensions[0]); wi++)
            for(unsigned hi = 0; hi < sizeof(dimensions) / sizeof(dimensions[0]); hi++)
                for(unsigned scale = 136; scale <= 256; scale += 8) {
                    unsigned width = dimensions[wi], height = dimensions[hi];
                    unsigned scaled_w = LV_MAX(1u, (width * scale + 128) / 256);
                    unsigned scaled_h = LV_MAX(1u, (height * scale + 128) / 256);
                    for(unsigned y = 0; y < scaled_h; y++) for(unsigned x = 0; x < scaled_w; x++) {
                        unsigned x0 = x * width * 256u / scaled_w;
                        unsigned x1 = (x + 1) * width * 256u / scaled_w;
                        unsigned y0 = y * height * 256u / scaled_h;
                        unsigned y1 = (y + 1) * height * 256u / scaled_h;
                        uint64_t reference = 0;
                        /* Integrate by source cells, independent of the
                         * optimized destination pixel traversal. */
                        for(unsigned sy = y0 / 256; sy < height && sy * 256 < y1; sy++)
                            for(unsigned sx = x0 / 256; sx < width && sx * 256 < x1; sx++) {
                                unsigned left = LV_MAX(x0, sx * 256), right = LV_MIN(x1, (sx + 1) * 256);
                                unsigned top = LV_MAX(y0, sy * 256), bottom = LV_MIN(y1, (sy + 1) * 256);
                                reference += (uint64_t)source[sy * 128 + sx] * (right - left) * (bottom - top);
                            }
                        unsigned area = (x1 - x0) * (y1 - y0);
                        CHECK(lv_obj_rows_average_a8(source, 128, x0, x1, y0, y1) ==
                              (reference + area / 2) / area);
                    }
                }
    }
}

static void check_sampler(lv_obj_t * row)
{
    lv_obj_set_raster_scale(row, 184);
    lv_obj_set_raster_opa(row, 173);
    CHECK(lv_obj_raster_sample_ready(row));
    lv_area_t logical;
    lv_obj_get_coords(row, &logical);
    int x0 = logical.x1 + lv_area_get_width(&logical) * (256 - 184) / 512;
    int y0 = logical.y1 + lv_area_get_height(&logical) * (256 - 184) / 512;
    for(int y = y0 + 8; y < y0 + 40; y += 8) for(int x = x0 + 40; x < x0 + 160; x += 8) {
        lv_area_t cell = {x, y, x + 7, y + 7};
        lv_color32_t area;
        CHECK(lv_obj_raster_sample_area(row, &cell, &area));
        unsigned red = 0, green = 0, blue = 0, alpha = 0;
        for(int py = y; py < y + 8; py++) for(int px = x; px < x + 8; px++) {
            lv_color32_t c;
            CHECK(lv_obj_raster_sample_pixel(row, px, py, &c));
            red += c.red; green += c.green; blue += c.blue; alpha += c.alpha;
        }
        CHECK(abs((int)area.red - (int)((red + 32) / 64)) <= 1);
        CHECK(abs((int)area.green - (int)((green + 32) / 64)) <= 1);
        CHECK(abs((int)area.blue - (int)((blue + 32) / 64)) <= 1);
        CHECK(area.alpha == (alpha + 32) / 64);
    }
}

static void check_band_pixels(lv_obj_t *screen)
{
    enum { W = 192, H = 88, STRIDE = W * 2 + 16, GUARD = 32 };
    static uint8_t pixels[GUARD + H * STRIDE + GUARD];
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 17, 19);
    lv_obj_set_size(row, W, H);
    lv_obj_set_style_radius(row, 35, 0);
    lv_obj_set_style_bg_opa(row, 255, 0);
    for(unsigned i = 0; i < 2; i++) {
        lv_obj_t *label = lv_label_create(row);
        orb_text_apply(label, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
        lv_label_set_text(label, i ? "VAV" : "AV ffi");
        lv_obj_set_pos(label, i ? 29 : 17, 21);
        lv_obj_set_style_text_color(label, lv_color_hex(i ? 0xFF4245 : 0xE1E8F4), 0);
        lv_obj_set_style_text_opa(label, i ? 179 : 255, 0);
    }
    lv_obj_update_layout(screen);
    lv_area_t area; lv_obj_get_coords(row, &area);
    for(unsigned background = 0; background < 2; background++) {
        lv_obj_set_style_bg_color(row, lv_color_hex(background ? 0x123456 : 0), 0);
        s_remaining = 10000;
        lv_obj_rows_t *rows = lv_obj_rows_create(row, lv_color_black(), limited_alloc, limited_free, NULL, 0);
        CHECK(rows != NULL);
        for(unsigned scale = 136; scale < 256; scale += 7) for(unsigned swapped = 0; swapped < 2; swapped++) {
            memset(pixels, 0xA7, sizeof(pixels));
            lv_color_format_t cf = swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
            lv_draw_buf_t buffer;
            CHECK(lv_draw_buf_init(&buffer, W, H, cf, STRIDE,
                                  pixels + GUARD, H * STRIDE) == LV_RESULT_OK);
            lv_area_t clip = {area.x1 + 7, area.y1 + 3, area.x2 - 11, area.y2 - 5};
            lv_layer_t layer = {.draw_buf = &buffer, .color_format = cf,
                                .buf_area = area, ._clip_area = clip};
            CHECK(lv_obj_rows_draw(rows, &layer, &area, scale, 255, lv_color_black()));
            /* The independent scalar sampler still computes shape coverage and
             * painter-order blending for every pixel. Cover black/colour,
             * clipped bands, overlapping semiopaque glyphs and both byte orders. */
            for(int y = area.y1; y <= area.y2; y++) for(int x = area.x1; x <= area.x2; x++) {
                lv_color32_t reference;
                uint16_t expected = 0xA7A7, actual;
                bool inside = x >= clip.x1 && x <= clip.x2 && y >= clip.y1 && y <= clip.y2;
                if(inside && lv_obj_rows_sample(rows, &area, scale, x, y, &reference)) {
                    expected = lv_color_to_u16(lv_color_make(reference.red, reference.green, reference.blue));
                    if(swapped) expected = lv_color_swap_16(expected);
                }
                memcpy(&actual, pixels + GUARD + (y - area.y1) * STRIDE + (x - area.x1) * 2, 2);
                CHECK(actual == expected);
            }
            for(unsigned i = 0; i < GUARD; i++) {
                CHECK(pixels[i] == 0xA7);
                CHECK(pixels[sizeof(pixels) - i - 1] == 0xA7);
            }
            for(unsigned y = 0; y < H; y++) for(unsigned x = W * 2; x < STRIDE; x++)
                CHECK(pixels[GUARD + y * STRIDE + x] == 0xA7);
        }
        lv_obj_rows_destroy(rows);
        lv_obj_raster_rows_drop_unused();
        CHECK(s_allocations == 0);
    }
    lv_obj_delete(row);
}

static void check_transparent_button(lv_obj_t * screen, lv_display_t * display)
{
    lv_obj_t * row = lv_button_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 320, 100);
    lv_obj_set_pos(row, 73, 183);
    lv_obj_set_style_radius(row, 50, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xFF4245), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_t * label = lv_label_create(row);
    orb_text_apply(label, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(label, "Transparent content");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_pos(label, 20, 20);
    lv_obj_t * glyph = lv_label_create(row);
    orb_icon_apply(glyph, ORB_TEXT_TITLE3, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(glyph, ORB_ICON_TERMINAL);
    lv_obj_set_style_text_color(glyph, lv_color_white(), 0);
    lv_obj_set_pos(glyph, 270, 20);
    lv_obj_set_raster_matte(row, lv_color_black());
    lv_obj_set_raster_rows(row, true);
    lv_obj_set_raster_scale(row, 184);
    lv_obj_raster_prepare(screen);
    lv_obj_raster_rows_stats_t stats;
    lv_obj_raster_rows_get_stats(&stats);
    CHECK(stats.rows == 1);
    lv_refr_now(display);
    sim_display_rgb(s_partial);

    /* The unused red background above must not leak into the transparent
     * model. Its pixels equal the established opaque-black matte row. */
    lv_obj_set_style_bg_color(row, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_raster_prepare(screen);
    lv_refr_now(display);
    sim_display_rgb(s_full);
    CHECK(!memcmp(s_partial, s_full, sizeof(s_full)));
    lv_obj_raster_rows_get_stats(&stats);
    CHECK(stats.rows == 1);

    /* Neither a partial fill nor a nonblack matte proves the simple model's
     * compositing contract. Unsupported styles retain the reference raster. */
    lv_obj_set_style_bg_opa(row, LV_OPA_50, 0);
    lv_obj_raster_prepare(screen);
    lv_obj_raster_rows_get_stats(&stats);
    CHECK(stats.rows == 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_raster_matte(row, lv_color_hex(0x1C1C1E));
    lv_obj_raster_prepare(screen);
    lv_obj_raster_rows_get_stats(&stats);
    CHECK(stats.rows == 0);
    lv_obj_set_raster_matte(row, lv_color_black());
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_raster_prepare(screen);
    lv_obj_raster_rows_get_stats(&stats);
    CHECK(stats.rows == 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_raster_prepare(screen);
    lv_obj_raster_rows_get_stats(&stats);
    CHECK(stats.rows == 1);
    check_sampler(row);
    lv_obj_delete(row);
    lv_obj_raster_rows_get_stats(&stats);
    CHECK(stats.rows == 0);
}

int main(void)
{
    check_exact_area_average();
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t * display = sim_display_create();
    orb_ui_init(NULL);
    lv_obj_t * saved = lv_screen_active();
    for(unsigned mode = 0; mode < 2; mode++) {
        CHECK(orb_fonts_set_content_size(mode ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        const lv_font_t * face = orb_font(ORB_TEXT_HEADLINE);
        orb_fonts_load_stats_t before_lookup, after_lookup;
        orb_fonts_load_stats(&before_lookup);
        CHECK(orb_fonts_nominal_px(face) == orb_text_style_info(ORB_TEXT_HEADLINE)->size_px);
        lv_font_t unknown = {0};
        CHECK(orb_fonts_nominal_px(&unknown) == 0);
        orb_fonts_load_stats(&after_lookup);
        CHECK(after_lookup.loads == before_lookup.loads);
        lv_obj_t * scene = lv_obj_create(NULL);
        orb_demo_build(scene);
        lv_screen_load(scene);
        lv_obj_update_layout(scene);
        lv_obj_t * list = lv_obj_get_child(scene, 0);
        for(unsigned i = 1; i <= 10; i++) {
            lv_obj_t * row = lv_obj_get_child(list, i);
            lv_obj_t * text = lv_obj_get_child(row, 1);
            lv_area_t bounds, labels;
            lv_obj_get_coords(row, &bounds);
            for(unsigned line = 0; line < 2; line++) {
                lv_obj_get_coords(lv_obj_get_child(text, line), &labels);
                CHECK(labels.y1 >= bounds.y1 + 4 && labels.y2 <= bounds.y2 - 4);
                CHECK(labels.x1 >= bounds.x1 && labels.x2 <= bounds.x2);
            }
        }
        lv_obj_raster_rows_stats_t cold, warm;
        lv_obj_raster_rows_get_stats(&cold);
        CHECK(cold.rows == 10);
        CHECK(cold.glyphs > 10);
        CHECK(cold.bytes <= cold.budget);
        CHECK(cold.size_builds > 0 && cold.size_reuses > 0);
        for(unsigned frame = 0; frame < 200; frame++) {
            s_now += 20;
            orb_demo_tick(0, 0, 1, 0.020f);
            lv_timer_handler();
            if(frame % 20) continue;
            lv_refr_now(display);
            sim_display_rgb(s_partial);
            lv_obj_invalidate(scene);
            lv_refr_now(display);
            sim_display_rgb(s_full);
            CHECK(!memcmp(s_partial, s_full, sizeof(s_full)));
        }
        lv_obj_raster_rows_get_stats(&warm);
        CHECK(warm.draws > cold.draws);
        CHECK(warm.bytes == cold.bytes);
        CHECK(warm.builds == cold.builds);
        CHECK(warm.size_builds == cold.size_builds && warm.size_reuses == cold.size_reuses);
        check_sampler(lv_obj_get_child(lv_obj_get_child(scene, 0), 1));
        lv_screen_load(saved);
        lv_obj_delete(scene);
        lv_obj_raster_rows_get_stats(&warm);
        CHECK(warm.rows == 0);
        CHECK(warm.glyphs > 0); /* bounded unpinned glyphs survive warm opens */
        CHECK(warm.bytes <= warm.budget);
    }
    check_transparent_button(saved, display);
    check_band_pixels(saved);
    lv_obj_t * row = lv_obj_create(saved);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 320, 100);
    lv_obj_set_pos(row, 80, 180);
    lv_obj_set_style_bg_opa(row, 255, 0);
    lv_obj_t * label = lv_label_create(row);
    orb_text_apply(label, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(label, "AV ffi caf\xc3\xa9");
    lv_obj_set_pos(label, 20, 20);
    lv_obj_t * overlap = lv_label_create(row);
    orb_text_apply(overlap, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(overlap, "VAV");
    lv_obj_set_pos(overlap, 30, 20);
    lv_obj_update_layout(saved);
    s_remaining = 1; /* row metadata succeeds; first new glyph fails */
    lv_obj_rows_t * failed = lv_obj_rows_create(row, lv_color_black(), limited_alloc, limited_free, NULL, 0);
    CHECK(failed == NULL);
    CHECK(s_allocations == 0);
    lv_obj_set_raster_matte(row, lv_color_black());
    lv_obj_set_raster_rows(row, true);
    lv_obj_raster_prepare(saved);
    lv_obj_raster_rows_stats_t valid;
    lv_obj_raster_rows_get_stats(&valid);
    CHECK(valid.rows == 1);
    check_sampler(row); /* UTF-8, kerning and overlapping glyph painter order */
    lv_obj_add_event_cb(row, custom_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_invalidate_raster(row);
    lv_obj_raster_prepare(saved);
    lv_obj_raster_rows_stats_t rejected;
    lv_obj_raster_rows_get_stats(&rejected);
    CHECK(rejected.rows == 0);
    lv_obj_delete(row);
    lv_obj_raster_rows_drop_unused();
    lv_obj_raster_rows_stats_t released;
    lv_obj_raster_rows_get_stats(&released);
    CHECK(released.rows == 0 && released.glyphs == 0 && released.bytes == 0);
    printf("rows at size: %u checks passed\n", s_checks);
    return 0;
}
