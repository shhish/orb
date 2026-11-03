/* Scroll damage is one transaction, not a suppression of redraw. Compare the
 * retained framebuffer with a fresh render, including outlying sibling writes,
 * nested scrolls, deletion and a synchronous refresh inside the callback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/core/lv_obj_scroll_private.h"
#include "src/core/lv_refr_private.h"
#include "src/misc/lv_area_private.h"

#define D 64
static uint8_t s_buffer[D * D * 2], s_frame[D * D * 2], s_partial[D * D * 2];
static unsigned s_checks, s_invalidations, s_damage_pixels;
static lv_area_t s_damage;
static lv_display_t *s_display;
static lv_obj_t *s_sibling, *s_other;
static bool s_nested, s_sync;
#define CHECK(c) do { s_checks++; if (!(c)) { \
    fprintf(stderr, "scroll damage:%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint32_t tick(void) { return 100; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    for (int y = area->y1; y <= area->y2; y++) {
        size_t bytes = (size_t)lv_area_get_width(area) * 2;
        memcpy(s_frame + (y * D + area->x1) * 2, pixels, bytes);
        pixels += bytes;
    }
    lv_display_flush_ready(display);
}
static void invalidated(lv_event_t *event)
{
    s_invalidations++;
    lv_area_t area = *(lv_area_t *)lv_event_get_param(event);
    s_damage_pixels += lv_area_get_size(&area);
    if(s_invalidations == 1) s_damage = area;
    else lv_area_join(&s_damage, &s_damage, &area);
}
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}
static void oracle(void)
{
    lv_refr_now(s_display);
    memcpy(s_partial, s_frame, sizeof(s_frame));
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_display);
    CHECK(!memcmp(s_partial, s_frame, sizeof(s_frame)));
}
static void changed(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    lv_obj_t *row = lv_obj_get_child(obj, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x0091ff), 0);
    lv_obj_set_style_opa(row, LV_OPA_70, 0);
    lv_obj_set_raster_scale(row, 220);
    lv_obj_set_raster_opa(row, 210);
    lv_obj_set_pos(s_sibling, 46, 2);
    lv_obj_set_style_bg_color(s_sibling, lv_color_hex(0xff4245), 0);
    if (s_nested) {
        CHECK(lv_obj_scroll_by_raw(s_other, 0, -3) == LV_RESULT_OK);
    }
    if (s_sync) {
        lv_refr_now(s_display);
        lv_obj_set_pos(s_sibling, 48, 6);
    }
}
static void deleted(lv_event_t *event) { lv_obj_delete(lv_event_get_current_target_obj(event)); }

static void fractional_raster_bounds(lv_obj_t *screen)
{
    /* The bitmap fallback transforms inclusive last-pixel coordinates, while
     * native row spans use floor(size*scale/256). Cover both, including 10px
     * at scale200 (8 bitmap pixels versus a 7px row span) and subpixel sizes. */
    const int sizes[] = {1, 2, 3, 5, 7, 10, 13, 17};
    const int scales[] = {1, 17, 63, 127, 136, 151, 184, 199, 200, 231, 255, 256};
    lv_obj_t *viewport = box(screen, 0, 0, D, D, 0);
    lv_obj_set_scroll_damage_children(viewport, true);
    for(unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
        lv_obj_t *row = box(viewport, 18, 18, sizes[n], sizes[n] + 3, 0xffffff);
        lv_obj_set_raster_matte(row, lv_color_black());
        lv_obj_update_layout(screen);
        lv_obj_raster_prepare(row);
        oracle();
        for(unsigned s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
            lv_obj_set_raster_scale(row, scales[s]); oracle();
            lv_obj_set_raster_opa(row, 173); oracle();
            CHECK(lv_obj_scroll_by_raw(viewport, 1, -1) == LV_RESULT_OK); oracle();
            lv_obj_set_raster_opa(row, 0); oracle();
            lv_obj_set_raster_opa(row, 255); oracle();
            CHECK(lv_obj_scroll_by_raw(viewport, -1, 1) == LV_RESULT_OK); oracle();
        }
        lv_obj_delete(row);
        oracle();
    }
    lv_obj_delete(viewport);
    oracle();
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    s_display = lv_display_create(D, D);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, s_buffer, NULL, sizeof(s_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, flush);
    lv_display_add_event_cb(s_display, invalidated, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_t *list = box(screen, 0, 16, 32, 48, 0);
    lv_obj_t *row = box(list, 0, 0, 30, 32, 0xffffff);
    box(list, 0, 80, 30, 32, 0xffffff);
    lv_obj_set_raster_matte(row, lv_color_black());
    s_sibling = box(screen, 44, 0, 12, 12, 0xffffff);
    s_other = box(screen, 34, 32, 28, 32, 0);
    box(s_other, 0, 0, 24, 24, 0x30d158);
    box(s_other, 0, 64, 24, 24, 0x30d158);
    lv_obj_update_layout(screen);
    lv_obj_raster_prepare(screen);
    oracle();
    lv_obj_add_event_cb(list, changed, LV_EVENT_SCROLL, NULL);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    s_invalidations = s_damage_pixels = 0;
    CHECK(lv_obj_scroll_by_raw(list, 0, -4) == LV_RESULT_OK);
    CHECK(s_invalidations == 2);
    CHECK(s_damage.x1 == 0 && s_damage.y1 == 0 && s_damage.x2 >= 55 && s_damage.y2 == 63);
    CHECK(s_damage_pixels < lv_area_get_size(&s_damage));
    oracle();
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds > before.builds); /* batching must preserve content dirtiness */

    s_nested = true;
    s_invalidations = s_damage_pixels = 0;
    CHECK(lv_obj_scroll_by_raw(list, 0, -4) == LV_RESULT_OK);
    CHECK(s_invalidations == 3 && s_damage.x2 == 61);
    CHECK(s_damage_pixels < lv_area_get_size(&s_damage));
    oracle();
    s_nested = false;

    /* An explicit refresh publishes the first half of the callback immediately;
     * subsequent mutations must still publish when its outer scope closes. */
    s_sync = true;
    s_invalidations = 0;
    CHECK(lv_obj_scroll_by_raw(list, 0, -4) == LV_RESULT_OK);
    CHECK(s_invalidations >= 2);
    oracle();
    s_sync = false;

    lv_display_enable_invalidation(s_display, false);
    s_invalidations = 0;
    CHECK(lv_obj_scroll_by_raw(list, 0, -4) == LV_RESULT_OK);
    CHECK(s_invalidations == 0);
    lv_display_enable_invalidation(s_display, true);
    lv_obj_invalidate(screen);
    oracle();

    /* Clearing the buffer also clears pending transactional damage. */
    lv_inv_area_batch_begin(s_display);
    lv_obj_invalidate(s_sibling);
    lv_inv_area(s_display, NULL);
    s_invalidations = 0;
    lv_inv_area_batch_end(s_display);
    CHECK(s_invalidations == 0);

    lv_obj_add_event_cb(s_other, deleted, LV_EVENT_SCROLL, NULL);
    s_invalidations = 0;
    CHECK(lv_obj_scroll_by_raw(s_other, 0, -4) == LV_RESULT_INVALID);
    CHECK(s_invalidations == 1);
    oracle();

    /* Cached children keep empty viewport gaps untouched, including the old
     * and new positions of a shrunken row. No source rebuild for movement. */
    lv_obj_t *sparse = box(screen, 0, 0, D, D, 0);
    lv_obj_t *cached = box(sparse, 24, 24, 16, 16, 0xffffff);
    lv_obj_set_raster_matte(cached, lv_color_black());
    lv_obj_set_raster_scale(cached, 192);
    lv_obj_set_scroll_damage_children(sparse, true);
    lv_obj_update_layout(screen);
    lv_obj_raster_prepare(screen);
    oracle();
    lv_obj_raster_get_stats(&before);
    s_invalidations = s_damage_pixels = 0;
    CHECK(lv_obj_scroll_by_raw(sparse, 0, -3) == LV_RESULT_OK);
    CHECK(s_invalidations == 1);
    CHECK(s_damage_pixels == 12 * 15 && s_damage_pixels < D * D);
    oracle();
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds);

    lv_obj_t *cold_hidden = box(sparse, 0, 0, 12, 12, 0xff4245);
    lv_obj_set_raster_matte(cold_hidden, lv_color_black());
    lv_obj_set_raster_opa(cold_hidden, 0);
    lv_obj_update_layout(screen);
    lv_obj_raster_get_stats(&before);
    oracle();
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds);
    CHECK(s_frame[0] == 0 && s_frame[1] == 0);
    s_invalidations = s_damage_pixels = 0;
    lv_obj_set_raster_scale(cold_hidden, 192);
    CHECK(s_invalidations == 0);
    lv_obj_set_raster_opa(cold_hidden, 255);
    oracle();
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds + 1);
    lv_obj_delete(cold_hidden);
    s_invalidations = s_damage_pixels = 0;
    lv_obj_set_raster_scale(cached, 160);
    lv_obj_set_raster_opa(cached, 180);
    CHECK(s_damage.x1 >= 26 && s_damage.x2 <= 37);
    oracle();

    /* A dynamically added uncached child and visible scrollbars use the
     * generic complete viewport until the caller restores the proof. */
    lv_obj_t *uncached = box(sparse, 0, 48, 8, 8, 0x30d158);
    lv_obj_update_layout(screen); oracle();
    s_invalidations = s_damage_pixels = 0;
    CHECK(lv_obj_scroll_by_raw(sparse, 0, -2) == LV_RESULT_OK);
    CHECK(s_damage_pixels == D * D);
    oracle();
    lv_obj_delete(uncached);
    lv_obj_set_style_transform_scale_x(cached, 280, 0);
    oracle();
    s_invalidations = s_damage_pixels = 0;
    CHECK(lv_obj_scroll_by_raw(sparse, 0, -2) == LV_RESULT_OK);
    CHECK(s_damage_pixels == D * D);
    oracle();
    lv_obj_set_style_transform_scale_x(cached, 256, 0);
    lv_obj_set_scrollbar_mode(sparse, LV_SCROLLBAR_MODE_ON);
    oracle();
    s_invalidations = s_damage_pixels = 0;
    CHECK(lv_obj_scroll_by_raw(sparse, 0, -2) == LV_RESULT_OK);
    CHECK(s_damage_pixels == D * D);
    oracle();

    /* Fixed storage overflow may enlarge damage, but never drops a request. */
    lv_inv_area_batch_begin(s_display);
    for(int i = 0; i < 12; i++) {
        lv_area_t a = {i * 5, 2, i * 5, 2};
        CHECK(lv_inv_area(s_display, &a) == LV_RESULT_OK);
    }
    s_invalidations = s_damage_pixels = 0;
    lv_inv_area_batch_end(s_display);
    CHECK(s_invalidations <= 8 && s_damage.x1 == 0 && s_damage.x2 == 55);
    CHECK(s_damage_pixels >= 12);
    fractional_raster_bounds(screen);
    printf("scroll damage: %u checks passed\n", s_checks);
    return 0;
}
