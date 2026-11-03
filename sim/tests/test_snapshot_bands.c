#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "orb_display_snapshot.h"
#include "src/display/lv_display_private.h"

#define WIDTH 466
#define HEIGHT 466
static unsigned s_checks, s_flushes, s_guard_calls;
static uint8_t s_scratch[WIDTH * 84 * 2] __attribute__((aligned(64)));
static uint8_t s_destination[WIDTH * HEIGHT * 2] __attribute__((aligned(64)));
#define CHECK(test) do { s_checks++; if (!(test)) { \
    fprintf(stderr, "snapshot bands:%d: %s\n", __LINE__, #test); exit(1); \
} } while (0)

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    LV_UNUSED(area); LV_UNUSED(pixels);
    s_flushes++;
    lv_display_flush_ready(display);
}

static int32_t guard(int32_t y, int32_t last, int32_t rows)
{
    LV_UNUSED(last);
    s_guard_calls++;
    int32_t end = y + rows - 1;
    if (y < 96 && end >= 96 && end < 178) return 96 - y;
    return rows;
}

static int32_t invalid_guard(int32_t y, int32_t last, int32_t rows)
{
    LV_UNUSED(y); LV_UNUSED(last); LV_UNUSED(rows);
    return 0;
}

static lv_obj_t *plain(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t color)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    return object;
}

static void compare(lv_obj_t *page, lv_draw_buf_t *destination)
{
    lv_draw_buf_t *reference = lv_snapshot_take(page, LV_COLOR_FORMAT_RGB565);
    CHECK(reference);
    CHECK(orb_display_snapshot_bands(page, destination, s_scratch, sizeof(s_scratch), guard));
    for (int row = 0; row < HEIGHT; row++) {
        CHECK(!memcmp(reference->data + (size_t)row * reference->header.stride,
            destination->data + (size_t)row * destination->header.stride, WIDTH * 2));
    }
    lv_draw_buf_destroy(reference);
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    CHECK(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_scratch, NULL, sizeof(s_scratch), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *page = plain(lv_screen_active(), 0, 0, WIDTH, HEIGHT, lv_color_black());
    for (unsigned i = 0; i < 7; i++) {
        lv_obj_t *row = plain(page, 42, 45 + (int)i * 54, 340, 42,
            lv_color_hex(i % 2 ? 0x0091ff : 0x303030));
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, "Cached pixels: whole regions");
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    }
    lv_draw_buf_t destination;
    CHECK(lv_draw_buf_init(&destination, WIDTH, HEIGHT, LV_COLOR_FORMAT_RGB565, WIDTH * 2,
        s_destination, sizeof(s_destination)) == LV_RESULT_OK);
    lv_layer_t *old_head = display->layer_head;
    compare(page, &destination);
    CHECK(display->layer_head == old_head && s_flushes == 0);
    lv_obj_t *glass = plain(page, 101, 101, 72, 72, lv_color_hex(0x252525));
    lv_obj_set_style_radius(glass, 36, 0);
    lv_obj_set_style_bg_opa(glass, 90, 0);
    lv_obj_set_style_blur_backdrop(glass, true, 0);
    lv_obj_set_style_blur_radius(glass, 16, 0);
    lv_obj_set_style_blur_quality(glass, LV_BLUR_QUALITY_SPEED, 0);
    compare(page, &destination);
    CHECK(s_guard_calls > 12 && display->layer_head == old_head && s_flushes == 0);

    lv_area_t outside = {-1, 0, 10, 10};
    lv_draw_buf_t band;
    CHECK(lv_draw_buf_init(&band, WIDTH, 84, LV_COLOR_FORMAT_RGB565, WIDTH * 2,
        s_scratch, sizeof(s_scratch)) == LV_RESULT_OK);
    lv_draw_buf_t *reference = lv_snapshot_take(page, LV_COLOR_FORMAT_RGB565);
    CHECK(reference);
    lv_area_t portion = {77, 210, 276, 230};
    CHECK(lv_snapshot_take_area_to_draw_buf(page, LV_COLOR_FORMAT_RGB565, &portion, &band) == LV_RESULT_OK);
    for (int row = 0; row < 21; row++) {
        CHECK(!memcmp(band.data + (size_t)row * band.header.stride,
            reference->data + (size_t)(row + 210) * reference->header.stride + 77u * 2u, 200u * 2u));
    }
    lv_draw_buf_destroy(reference);
    CHECK(lv_snapshot_take_area_to_draw_buf(page, LV_COLOR_FORMAT_RGB565, &outside, &band) == LV_RESULT_INVALID);
    CHECK(!orb_display_snapshot_bands(page, &destination, s_scratch, WIDTH * 2 - 1, guard));
    CHECK(!orb_display_snapshot_bands(page, &destination, s_scratch, sizeof(s_scratch), invalid_guard));
    CHECK(display->layer_head == old_head && s_flushes == 0);
    lv_display_delete(display);
    lv_deinit();
    printf("snapshot bands: %u checks; opaque/text and whole-glass regions byte-exact\n", s_checks);
    return 0;
}
