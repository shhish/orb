/* Bands touching a label's blank line box must not decode its bitmap glyphs.
 * Selection backgrounds still fill that box independently of the glyph ink. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/lv_draw_label_private.h"
#include "src/draw/sw/lv_draw_sw.h"

static unsigned s_checks, s_decodes, s_callbacks, s_releases;

#define CHECK(value) do { s_checks++; if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)

static bool glyph(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t next)
{
    (void)font;
    (void)next;
    if (letter != 'A' && letter != 'g' && letter != ' ') return false;
    dsc->adv_w = letter == ' ' ? 4 : 12;
    if (letter == ' ') return true;
    dsc->box_w = 8;
    dsc->box_h = letter == 'g' ? 8 : 4;
    dsc->ofs_y = letter == 'g' ? -3 : 0;
    dsc->format = LV_FONT_GLYPH_FORMAT_A8;
    dsc->gid.index = 1;
    return true;
}

static const void *bitmap(lv_font_glyph_dsc_t *glyph_dsc, lv_draw_buf_t *buffer)
{
    (void)glyph_dsc;
    s_decodes++;
    memset(buffer->data, 255, buffer->header.stride * buffer->header.h);
    return buffer;
}

static void release(const lv_font_t *font, lv_font_glyph_dsc_t *descriptor)
{
    (void)font;
    CHECK(descriptor->entry == &s_releases);
    s_releases++;
}

static const lv_font_t s_font = {
    .get_glyph_dsc = glyph,
    .get_glyph_bitmap = bitmap,
    .release_glyph = release,
    .line_height = 60,
    .base_line = 10,
};

static void label_bands(bool selected)
{
    lv_draw_buf_t *buffer = lv_draw_buf_create(40, 60, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    CHECK(buffer != NULL);
    memset(buffer->data, 0, buffer->header.stride * buffer->header.h);
    lv_layer_t layer = {0};
    layer.draw_buf = buffer;
    layer.buf_area = (lv_area_t){0, 0, 39, 59};
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = &s_font;
    label.text = "A";
    label.color = lv_color_white();
    label.text_size = (lv_point_t){12, 60};
    if (selected) {
        label.sel_start = 0;
        label.sel_end = 1;
        label.sel_color = lv_color_white();
        label.sel_bg_color = lv_color_hex(0xff0000);
    }
    lv_draw_task_t task = {0};
    task.draw_dsc = &label;
    task.target_layer = &layer;
    s_decodes = 0;
    for (int y = 0; y < 60; y += 10) {
        task.clip_area = (lv_area_t){0, y, 39, y + 9};
        lv_draw_sw_label(&task, &label, &layer.buf_area);
    }
    CHECK(s_decodes == 1);
    for (unsigned y = 0; y < 60; y++) {
        const uint16_t *row = (const uint16_t *)(buffer->data + y * buffer->header.stride);
        for (unsigned x = 0; x < 40; x++) {
            uint16_t expected = x < 8 && y >= 46 && y < 50 ? 0xffff :
                                selected && x < 12 ? 0xf800 : 0;
            CHECK(row[x] == expected);
        }
    }
    lv_draw_buf_destroy(buffer);
}

static void counted(lv_draw_task_t *task, lv_draw_glyph_dsc_t *draw,
                    lv_draw_fill_dsc_t *fill, const lv_area_t *area)
{
    (void)task;
    (void)draw;
    (void)fill;
    (void)area;
    s_callbacks++;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

/* Exercise the real label widget, including line wrapping and selection, with
 * each line's ink on either side of every 84-row band boundary. The descender
 * also checks that negative glyph offsets are measured from the current line. */
static void multiline_bands(bool wrapped, bool selected, int32_t line_space)
{
    enum { WIDTH = 48, HEIGHT = 224, BAND = 84 };
    int32_t pitch = s_font.line_height + line_space;
    static uint8_t pixels[WIDTH * BAND * 2];
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    CHECK(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *page = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, WIDTH, HEIGHT);
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_t *label = lv_label_create(page);
    lv_obj_remove_style_all(label);
    lv_obj_set_style_text_font(label, &s_font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(label, line_space, 0);
    lv_obj_set_size(label, 32, s_font.line_height + pitch);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(label, wrapped ? "Ag Ag" : "Ag\nAg");
    if (selected) {
        lv_label_set_text_selection_start(label, 0);
        lv_label_set_text_selection_end(label, 5);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_SELECTED);
        lv_obj_set_style_bg_color(label, lv_color_hex(0xff0000), LV_PART_SELECTED);
    }
    lv_draw_buf_t *band = lv_draw_buf_create(WIDTH, BAND, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    CHECK(band);
    for (unsigned phase = 0; phase < BAND; phase++) {
        lv_obj_set_pos(label, 4, phase);
        lv_obj_update_layout(page);
        lv_draw_buf_t *whole = lv_snapshot_take(page, LV_COLOR_FORMAT_RGB565);
        CHECK(whole);
        for (unsigned line = 0; line < 2; line++) {
            const uint16_t *row = (const uint16_t *)(whole->data +
                (phase + 46 + line * pitch) * whole->header.stride);
            CHECK(row[4] == 0xffff);
            row = (const uint16_t *)(whole->data +
                (phase + 52 + line * pitch) * whole->header.stride);
            CHECK(row[16] == 0xffff);
        }
        for (int y = 0; y < HEIGHT; y += BAND) {
            lv_area_t clip = {0, y, WIDTH - 1, LV_MIN(HEIGHT - 1, y + BAND - 1)};
            CHECK(lv_snapshot_take_area_to_draw_buf(page, LV_COLOR_FORMAT_RGB565, &clip, band) == LV_RESULT_OK);
            for (int row = 0; row <= clip.y2 - clip.y1; row++) {
                if (memcmp(whole->data + (y + row) * whole->header.stride,
                           band->data + row * band->header.stride, WIDTH * 2)) {
                    fprintf(stderr, "label bands: wrapped=%u selected=%u spacing=%d phase=%u y=%d\n",
                            wrapped, selected, (int)line_space, phase, y + row);
                }
                CHECK(!memcmp(whole->data + (y + row) * whole->header.stride,
                              band->data + row * band->header.stride, WIDTH * 2));
            }
        }
        lv_draw_buf_destroy(whole);
    }
    lv_draw_buf_destroy(band);
    lv_display_delete(display);
}

static void conservative_bounds(void)
{
    lv_draw_task_t task = {0};
    task.clip_area = (lv_area_t){0, 0, 39, 9};
    lv_area_t background = {0, 0, 11, 59};
    lv_point_t position = {0, 0};
    lv_font_glyph_dsc_t descriptor = {0};
    CHECK(lv_font_get_glyph_dsc(&s_font, &descriptor, 'A', 0));
    lv_draw_glyph_dsc_t draw;
    lv_draw_glyph_dsc_init(&draw);
    draw.g = &descriptor;
    draw.bg_coords = &background;
    descriptor.entry = &s_releases;
    lv_draw_unit_draw_letter_internal(&task, &draw, &position, &s_font, 'A', counted);
    CHECK(s_releases == 1);
    CHECK(s_callbacks == 0);
    descriptor.entry = NULL;
    s_callbacks = 0;
    draw.rotation = 100;
    lv_draw_unit_draw_letter_internal(&task, &draw, &position, &s_font, 'A', counted);
    CHECK(s_callbacks == 1);
    draw.rotation = 0;
    descriptor.is_placeholder = true;
    lv_draw_unit_draw_letter_internal(&task, &draw, &position, &s_font, 'A', counted);
    CHECK(s_callbacks == 2);
    descriptor.is_placeholder = false;
    draw.outline_stroke_width = 2;
    lv_draw_unit_draw_letter_internal(&task, &draw, &position, &s_font, 'A', counted);
    CHECK(s_callbacks == 3);
    if (draw._draw_buf) lv_draw_buf_destroy(draw._draw_buf);
}

int main(void)
{
    lv_init();
    label_bands(false);
    label_bands(true);
    conservative_bounds();
    for (unsigned wrapped = 0; wrapped < 2; wrapped++) {
        multiline_bands(wrapped, false, 4);
        multiline_bands(wrapped, true, 4);
        multiline_bands(wrapped, false, -12);
    }
    lv_deinit();
    printf("label ink clipping: %u checks; exact single/multiline plain/selected bands at all 84 phases\n", s_checks);
    return EXIT_SUCCESS;
}
