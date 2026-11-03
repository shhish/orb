/* More than LV_INV_BUF_SIZE moving regions must preserve every changed pixel
 * without upgrading sparse damage to a full-screen refresh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"
#include "src/misc/lv_area_private.h"

#define D 64
static uint8_t s_buffer[D * D * 2], s_frame[D * D * 2], s_partial[D * D * 2];
static uint32_t s_colors[64];
static unsigned s_checks, s_pixels;
#define CHECK(c) do { s_checks++; if (!(c)) { \
    fprintf(stderr, "damage overflow:%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint32_t tick(void) { return 100; }
static lv_area_t cell(unsigned i)
{
    return (lv_area_t){(i % 8) * 8 + 2, (i / 8) * 8 + 2,
                       (i % 8) * 8 + 3, (i / 8) * 8 + 3};
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    size_t bytes = (size_t)lv_area_get_width(area) * 2;
    s_pixels += lv_area_get_size(area);
    for (int y = area->y1; y <= area->y2; y++, pixels += bytes)
        memcpy(s_frame + (y * D + area->x1) * 2, pixels, bytes);
    lv_display_flush_ready(display);
}
static void draw(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    for (unsigned i = 0; i < 64; i++) {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_opa = LV_OPA_COVER;
        dsc.bg_color = lv_color_hex(s_colors[i]);
        lv_area_t area = cell(i);
        lv_draw_rect(layer, &dsc, &area);
    }
}
int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = lv_display_create(D, D);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_buffer, NULL, sizeof(s_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(screen, draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_refr_now(display);
    for (unsigned iteration = 0; iteration < 20; iteration++) {
        for (unsigned i = 0; i < 64; i++) {
            s_colors[i] = iteration & 1 ? 0x30d158 : 0xff375f;
            lv_area_t area = cell(i);
            CHECK(lv_inv_area(display, &area) == LV_RESULT_OK);
        }
        CHECK(display->inv_p == LV_INV_BUF_SIZE);
        for (unsigned i = 0; i < 64; i++) {
            lv_area_t requested = cell(i);
            bool covered = false;
            for (unsigned j = 0; j < display->inv_p; j++)
                covered |= lv_area_is_in(&requested, &display->inv_areas[j], 0);
            CHECK(covered);
        }
        s_pixels = 0;
        lv_refr_now(display);
        CHECK(s_pixels < D * D / 2);
        memcpy(s_partial, s_frame, sizeof(s_frame));
        lv_obj_invalidate(screen);
        lv_refr_now(display);
        CHECK(!memcmp(s_partial, s_frame, sizeof(s_frame)));
    }
    lv_display_delete(display);
    lv_deinit();
    printf("damage overflow: %u bounded coverage and partial/full pixel checks passed\n", s_checks);
    return 0;
}
