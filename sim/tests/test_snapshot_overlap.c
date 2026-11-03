/* Real LVGL, two aliased DMA halves, and delayed SPI retirement. Compare the
 * pre-fence renderer with the former FLUSH_START compositor byte for byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "orb_nav_compositor.h"
#include "orb_display_geom.h"

#define D 466
#define HALF_BYTES (D * 42 * 2)
#define HALF_STRIDE ((HALF_BYTES + 63u) & ~63u)

typedef struct {
    lv_display_t *display;
    orb_nav_compositor_t *compositor;
    uint8_t arena[HALF_STRIDE * 2] __attribute__((aligned(64)));
    uint8_t panel[D * D * 2], pending[HALF_BYTES];
    const uint8_t *inflight;
    lv_area_t area;
    size_t bytes;
    unsigned overlaps, renders, flushes, waits;
    bool candidate;
} fixture_t;

static fixture_t s_reference, s_candidate;
static int16_t s_chords[D];
static unsigned s_checks;
#define CHECK(condition) do { s_checks++; if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static void retire(lv_display_t *display)
{
    fixture_t *f = lv_display_get_user_data(display);
    CHECK(f->inflight != NULL);
    /* Queueing a different band never grants permission to overwrite the
     * first. A premature reuse is detected even before the fake DMA copies. */
    CHECK(!memcmp(f->pending, f->inflight, f->bytes));
    int width = lv_area_get_width(&f->area);
    for (int row = f->area.y1; row <= f->area.y2; row++)
        memcpy(f->panel + (row * D + f->area.x1) * 2,
            f->inflight + (row - f->area.y1) * width * 2, width * 2);
    f->inflight = NULL;
    f->waits++;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    fixture_t *f = lv_display_get_user_data(display);
    CHECK(!f->inflight);
    f->inflight = pixels; f->area = *area;
    f->bytes = (size_t)lv_area_get_size(area) * 2;
    CHECK(f->bytes <= HALF_BYTES);
    CHECK(pixels == f->arena || pixels == f->arena + HALF_STRIDE);
    memcpy(f->pending, pixels, f->bytes);
    f->flushes++;
    /* Deliberately do not call flush_ready: real ownership ends at retire. */
}

static void compose(lv_event_t *event)
{
    fixture_t *f = lv_event_get_user_data(event);
    const lv_area_t *area;
    lv_draw_buf_t *draw;
    lv_display_render_band_t *band = NULL;
    if (f->candidate) {
        band = lv_event_get_param(event);
        area = band->area; draw = band->draw_buf;
    } else {
        area = lv_event_get_param(event);
        draw = lv_display_get_buf_active(f->display);
        CHECK(!f->inflight); /* The old event only runs after the DMA fence. */
    }
    if (f->inflight) {
        CHECK(f->inflight != draw->data);
        CHECK(!memcmp(f->pending, f->inflight, f->bytes));
        f->overlaps++;
    }
    CHECK(orb_nav_compositor_render_band(f->compositor, area->x1, area->y1,
        lv_area_get_width(area), lv_area_get_height(area), draw->data,
        draw->header.stride, true));
    if (band) band->handled = true;
    f->renders++;
}

static void band_shape(lv_display_t *display, lv_area_t *area, int32_t last, uint32_t pixels)
{
    (void)display;
    orb_geom_band(s_chords, D, area->y1, last, area->x1, area->x2, pixels, NULL,
        &area->y2, &area->x1, &area->x2);
}

static void create(fixture_t *f, bool candidate)
{
    f->candidate = candidate;
    f->compositor = orb_nav_compositor_create(D, D);
    CHECK(f->compositor != NULL);
    for (unsigned slot = 0; slot < 2; slot++) {
        CHECK(orb_nav_compositor_capture_begin(f->compositor, slot, 1));
        uint16_t *pixels = orb_nav_compositor_pixels(f->compositor, slot);
        for (unsigned i = 0; i < D * D; i++) pixels[i] = (uint16_t)(i * 317u + slot * 939u);
        CHECK(orb_nav_compositor_capture_complete(f->compositor, slot));
    }
    f->display = lv_display_create(D, D);
    lv_display_set_user_data(f->display, f);
    lv_display_set_color_format(f->display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(f->display, f->arena, f->arena + HALF_STRIDE,
        HALF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(f->display, flush);
    lv_display_set_flush_wait_cb(f->display, retire);
    lv_display_set_band_cb(f->display, band_shape);
    lv_display_set_tile_cnt(f->display, 1);
    lv_display_add_event_cb(f->display, compose,
        candidate ? LV_EVENT_RENDER_BAND : LV_EVENT_FLUSH_START, f);
    lv_obj_set_style_bg_color(lv_display_get_screen_active(f->display), lv_color_hex(0xFF00FF), 0);
}

static void refresh(fixture_t *f)
{
    lv_obj_invalidate(lv_display_get_screen_active(f->display));
    lv_refr_now(f->display);
    if (f->inflight) {
        retire(f->display);
        lv_display_flush_ready(f->display);
    }
}

int main(void)
{
    lv_init(); orb_geom_half_chords(s_chords, D);
    create(&s_reference, false); create(&s_candidate, true);
    orb_nav_token_rect_t token = {60, 90, 72, 72};
    for (unsigned kind = ORB_NAV_TRANSITION_SLIDE; kind <= ORB_NAV_TRANSITION_BOUNDARY; kind++) {
        for (unsigned direction = ORB_NAV_DIRECTION_LEFT; direction <= ORB_NAV_DIRECTION_DOWN; direction++) {
            for (unsigned reduced = 0; reduced < 2; reduced++) {
                unsigned to = kind == ORB_NAV_TRANSITION_BOUNDARY ? 0 : 1;
                CHECK(orb_nav_compositor_begin(s_reference.compositor, kind, 0, to,
                    direction, &token, direction & 1, reduced));
                CHECK(orb_nav_compositor_begin(s_candidate.compositor, kind, 0, to,
                    direction, &token, direction & 1, reduced));
                for (int frame = -1; frame <= 11; frame++) {
                    orb_nav_compositor_drag(s_reference.compositor, frame / 10.f);
                    orb_nav_compositor_drag(s_candidate.compositor, frame / 10.f);
                    refresh(&s_reference); refresh(&s_candidate);
                    CHECK(!memcmp(s_reference.panel, s_candidate.panel, sizeof(s_reference.panel)));
                }
                orb_nav_compositor_cancel(s_reference.compositor);
                orb_nav_compositor_cancel(s_candidate.compositor);
            }
        }
    }
    CHECK(s_candidate.overlaps > 0 && s_reference.overlaps == 0);
    CHECK(s_candidate.flushes == s_reference.flushes);
    CHECK(s_candidate.renders == s_candidate.flushes);
    CHECK(s_candidate.waits == s_candidate.flushes);
    orb_nav_compositor_destroy(s_reference.compositor);
    orb_nav_compositor_destroy(s_candidate.compositor);
    printf("snapshot overlap: %u checks, %u compositions during outstanding DMA; exact pixels\n",
        s_checks, s_candidate.overlaps);
    return 0;
}
