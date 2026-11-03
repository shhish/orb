/* Exercise split-arena eligibility and buffer switching through real LVGL.
 * Immediate host flushes prove pixels/ownership, not device DMA overlap. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_display_buffers.h"
#include "orb_display_geom.h"
#include "orb_ui.h"
#include "src/libs/lodepng/lodepng.h"

#define D 466
#define FULL_BYTES (D * 84 * 2)
#define HALF_BYTES (D * 42 * 2)
#define HALF_STRIDE ((HALF_BYTES + 63u) & ~63u)
#define ARENA_BYTES (HALF_STRIDE * 2)

typedef struct {
    uint8_t arena[ARENA_BYTES] __attribute__((aligned(64)));
    uint8_t pixels[D * D * 2];
    uint8_t frozen[D * D * 2];
    lv_image_dsc_t frozen_image;
    lv_display_t *display;
    lv_obj_t *glass, *content, *snapshot;
    unsigned single_frames, double_frames, flushes;
    bool adaptive, doubled, snapshot_transition, live_overlap, independent;
} fixture_t;

static fixture_t s_control, s_candidate;
static int16_t s_half_chords[D];
static uint32_t s_now;
static unsigned s_checks;

#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static uint32_t tick(void) { return s_now; }

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    fixture_t *f = lv_display_get_user_data(display);
    int32_t width = lv_area_get_width(area), height = lv_area_get_height(area);
    CHECK(area->x1 >= 0 && area->x2 < D && area->y1 >= 0 && area->y2 < D);
    CHECK((size_t)width * height * 2 <= (f->doubled ? HALF_BYTES : FULL_BYTES));
    CHECK(pixels == f->arena || (f->doubled && pixels == f->arena + HALF_STRIDE));
    for (int32_t y = area->y1; y <= area->y2; y++) {
        memcpy(f->pixels + (y * D + area->x1) * 2, pixels, (size_t)width * 2);
        pixels += width * 2;
    }
    f->flushes++;
    lv_display_flush_ready(display);
}

static void band(lv_display_t *display, lv_area_t *area, int32_t y_last, uint32_t pixels)
{
    fixture_t *f = lv_display_get_user_data(display);
    orb_geom_band(s_half_chords, D, area->y1, y_last, area->x1, area->x2, pixels,
                  f->independent ? NULL : orb_glass_band_guard,
                  &area->y2, &area->x1, &area->x2);
}

static void round_area(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void render_start(lv_event_t *event)
{
    lv_display_t *display = lv_event_get_target(event);
    fixture_t *f = lv_display_get_user_data(display);
    CHECK(!display->flushing);
    f->independent = f->snapshot_transition ||
        (f->live_overlap && orb_display_live_bands_independent(display));
    bool doubled = f->adaptive &&
        orb_display_half_buffers_fit_scene(display, HALF_BYTES, f->independent);
    if (doubled != f->doubled) {
        lv_display_set_buffers(display, f->arena, doubled ? f->arena + HALF_STRIDE : NULL,
                               doubled ? HALF_BYTES : FULL_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
        f->doubled = doubled;
    }
    if (doubled) f->double_frames++;
    else f->single_frames++;
}

static void create_fixture(fixture_t *f, bool adaptive)
{
    f->adaptive = adaptive;
    f->display = lv_display_create(D, D);
    CHECK(f->display != NULL);
    lv_display_set_default(f->display);
    lv_display_set_user_data(f->display, f);
    lv_display_set_color_format(f->display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(f->display, f->arena, NULL, FULL_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(f->display, flush);
    lv_display_set_band_cb(f->display, band);
    lv_display_set_tile_cnt(f->display, 1);
    lv_display_add_event_cb(f->display, round_area, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(f->display, render_start, LV_EVENT_RENDER_START, NULL);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    for (unsigned i = 0; i < 7; i++) {
        lv_obj_t *row = lv_obj_create(screen);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 60 + i % 2 * 12, 40 + i * 54);
        lv_obj_set_size(row, 332, 40);
        lv_obj_set_style_bg_color(row, lv_color_hex(i % 2 ? 0x30303A : 0x183248), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        if (i == 1) f->content = row;
    }
    f->glass = lv_obj_create(screen);
    lv_obj_remove_style_all(f->glass);
    lv_obj_set_size(f->glass, ORB_CONTROL, ORB_CONTROL);
    lv_obj_set_pos(f->glass, 101 - ORB_CONTROL / 2, 101 - ORB_CONTROL / 2);
    orb_glass_apply(f->glass, ORB_GLASS_REGULAR, ORB_GRAY);
}

static void compare_frames(void)
{
    lv_refr_now(s_control.display);
    lv_refr_now(s_candidate.display);
    if (memcmp(s_control.pixels, s_candidate.pixels, sizeof(s_control.pixels))) {
        bool different = false;
        for (unsigned i = 0; i < sizeof(s_control.pixels); i++) {
            if (s_control.pixels[i] == s_candidate.pixels[i]) continue;
            unsigned x = i / 2 % D, y = i / 2 / D;
            /* Different band heights send different rectangle overscan in
             * the square panel RAM's physically invisible corners. Compare
             * every visible circle column, including its conservative rim. */
            if ((s_control.live_overlap || s_candidate.live_overlap) &&
                ((int)x < D / 2 - s_half_chords[y] || (int)x > D / 2 + s_half_chords[y])) continue;
            fprintf(stderr, "buffer mismatch frame=%u candidate_half=%u independent=%u pixel=%u,%u control=%u candidate=%u\n",
                s_candidate.single_frames + s_candidate.double_frames, s_candidate.doubled,
                s_candidate.independent, x, y, s_control.pixels[i], s_candidate.pixels[i]);
            different = true;
            break;
        }
        CHECK(!different);
    }
    CHECK(!s_control.doubled);
}

/* Capture the bytes actually flushed with adaptive buffers; a snapshot would
 * redraw into a different buffer and could hide an admission/rendering bug. */
static void save_frame(const fixture_t *f, const char *prefix, const char *name)
{
    static uint8_t rgb[D * D * 3];
    for (unsigned i = 0; i < D * D; i++) {
        uint16_t pixel = (uint16_t)f->pixels[i * 2] << 8 | f->pixels[i * 2 + 1];
        rgb[i * 3] = (uint8_t)(((pixel >> 11) & 31) * 255 / 31);
        rgb[i * 3 + 1] = (uint8_t)(((pixel >> 5) & 63) * 255 / 63);
        rgb[i * 3 + 2] = (uint8_t)((pixel & 31) * 255 / 31);
    }
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s-%s.png", prefix, name);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    unsigned char *png = NULL;
    size_t bytes = 0;
    CHECK(lodepng_encode24(&png, &bytes, rgb, D, D) == 0);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(png, 1, bytes, file) == bytes);
    CHECK(fclose(file) == 0);
    lv_free(png);
}

static void eligibility(void)
{
    lv_display_t d = { .render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL,
                       .color_format = LV_COLOR_FORMAT_RGB565_SWAPPED };
    CHECK(!orb_display_half_buffers_fit(NULL, HALF_BYTES));
    CHECK(!orb_display_half_buffers_fit(&d, HALF_BYTES));
    d.inv_p = 1;
    d.inv_areas[0] = (lv_area_t){0, 0, D - 1, 41};
    CHECK(orb_display_half_buffers_fit(&d, HALF_BYTES));
    d.inv_areas[0].y2++;
    CHECK(!orb_display_half_buffers_fit(&d, HALF_BYTES));
    CHECK(orb_display_half_buffers_fit_scene(&d, HALF_BYTES, true));
    d.inv_areas[0] = (lv_area_t){0, 0, 99, 99};
    CHECK(orb_display_half_buffers_fit(&d, HALF_BYTES));
    d.inv_p = 2;
    d.inv_areas[1] = (lv_area_t){0, 0, D - 1, D - 1};
    CHECK(!orb_display_half_buffers_fit(&d, HALF_BYTES));
    CHECK(orb_display_half_buffers_fit_scene(&d, HALF_BYTES, true));
    CHECK(!orb_display_half_buffers_fit_scene(&d, D * 2 - 1, true));
    d.inv_area_joined[1] = 1;
    CHECK(orb_display_half_buffers_fit(&d, HALF_BYTES));
    d.inv_area_joined[0] = 1;
    CHECK(!orb_display_half_buffers_fit(&d, HALF_BYTES));
    CHECK(!orb_display_half_buffers_fit_scene(&d, HALF_BYTES, true));
    d.inv_area_joined[0] = 0;
    d.rotation = LV_DISPLAY_ROTATION_90;
    CHECK(!orb_display_half_buffers_fit(&d, HALF_BYTES));
    CHECK(!orb_display_half_buffers_fit_scene(&d, HALF_BYTES, true));
    d.rotation = LV_DISPLAY_ROTATION_0;
    d.render_mode = LV_DISPLAY_RENDER_MODE_FULL;
    CHECK(!orb_display_half_buffers_fit(&d, HALF_BYTES));
    CHECK(!orb_display_half_buffers_fit_scene(&d, HALF_BYTES, true));
}

static void begin_snapshot(fixture_t *f)
{
    memcpy(f->frozen, f->pixels, sizeof(f->frozen));
    f->frozen_image = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565_SWAPPED,
                   .w = D, .h = D, .stride = D * 2},
        .data_size = sizeof(f->frozen), .data = f->frozen,
    };
    /* Like the shell compositor, the opaque snapshot covers the live glass
     * tree. Its captured controls must not invoke the live blur band guard. */
    f->snapshot = lv_image_create(lv_display_get_screen_active(f->display));
    lv_image_set_src(f->snapshot, &f->frozen_image);
    f->snapshot_transition = true;
}

static void invalidate_live_scene(void)
{
    lv_obj_invalidate(lv_display_get_screen_active(s_control.display));
    lv_obj_invalidate(lv_display_get_screen_active(s_candidate.display));
    compare_frames();
}

static void live_scene_eligibility(void)
{
    CHECK(!orb_display_live_bands_independent(NULL));
    CHECK(!orb_display_live_bands_independent(s_candidate.display));
    s_control.live_overlap = s_candidate.live_overlap = true;
    lv_obj_set_hidden(s_control.glass, true);
    lv_obj_set_hidden(s_candidate.glass, true);
    invalidate_live_scene();
    CHECK(s_candidate.independent && s_candidate.doubled);
    for (unsigned layer = 0; layer < 3; layer++) {
        lv_obj_t *overlay[2];
        fixture_t *fixtures[] = {&s_control, &s_candidate};
        for (unsigned i = 0; i < 2; i++) {
            lv_display_t *display = fixtures[i]->display;
            lv_obj_t *parent = layer == 0 ? lv_display_get_layer_bottom(display) :
                layer == 1 ? lv_display_get_layer_top(display) : lv_display_get_layer_sys(display);
            overlay[i] = lv_obj_create(parent);
            lv_obj_remove_style_all(overlay[i]);
            lv_obj_set_size(overlay[i], ORB_CONTROL, ORB_CONTROL);
            lv_obj_set_pos(overlay[i], 200, 100);
            orb_glass_apply(overlay[i], ORB_GLASS_REGULAR, ORB_GRAY);
        }
        invalidate_live_scene();
        CHECK(!s_candidate.independent && !s_candidate.doubled);
        lv_obj_set_hidden(overlay[0], true);
        lv_obj_set_hidden(overlay[1], true);
        invalidate_live_scene();
        CHECK(s_candidate.independent && s_candidate.doubled);
        lv_obj_set_hidden(overlay[0], false);
        lv_obj_set_hidden(overlay[1], false);
        invalidate_live_scene();
        CHECK(!s_candidate.independent && !s_candidate.doubled);
        lv_obj_delete(overlay[0]); lv_obj_delete(overlay[1]);
        invalidate_live_scene();
        CHECK(s_candidate.independent && s_candidate.doubled);
    }
    /* An animated outgoing screen remains a live source too. Borrow the
     * normal previous-screen slot just for the predicate, then restore it
     * before rendering/deleting; all objects and blur styles are real LVGL. */
    lv_display_set_default(s_candidate.display);
    lv_obj_t *overflow = lv_obj_create(lv_display_get_layer_top(s_candidate.display));
    lv_obj_remove_style_all(overflow);
    lv_obj_set_size(overflow, 8, 8);
    lv_obj_set_pos(overflow, -100, -100);
    lv_obj_set_overflow_visible(overflow, true);
    lv_obj_t *overflow_glass = lv_obj_create(overflow);
    lv_obj_set_size(overflow_glass, ORB_CONTROL, ORB_CONTROL);
    lv_obj_set_pos(overflow_glass, 300, 200);
    orb_glass_apply(overflow_glass, ORB_GLASS_REGULAR, ORB_GRAY);
    lv_obj_update_layout(lv_display_get_layer_top(s_candidate.display));
    CHECK(lv_obj_is_overflow_visible(overflow) && !lv_obj_is_hidden(overflow_glass));
    CHECK(!orb_display_live_bands_independent(s_candidate.display));
    lv_obj_set_hidden(overflow, true);
    CHECK(orb_display_live_bands_independent(s_candidate.display));
    lv_obj_set_hidden(overflow, false);
    CHECK(!orb_display_live_bands_independent(s_candidate.display));
    lv_obj_delete(overflow);
    lv_obj_t *previous = lv_obj_create(NULL);
    lv_obj_t *glass = lv_obj_create(previous);
    lv_obj_set_size(glass, ORB_CONTROL, ORB_CONTROL);
    orb_glass_apply(glass, ORB_GLASS_REGULAR, ORB_GRAY);
    CHECK(s_candidate.display->prev_scr == NULL);
    s_candidate.display->prev_scr = previous;
    CHECK(!orb_display_live_bands_independent(s_candidate.display));
    lv_obj_set_hidden(glass, true);
    CHECK(orb_display_live_bands_independent(s_candidate.display));
    s_candidate.display->prev_scr = NULL;
    lv_obj_delete(previous);
    CHECK(sizeof(s_candidate.arena) == ARENA_BYTES && ARENA_BYTES - FULL_BYTES < 128);
}

int main(int argc, char **argv)
{
    lv_init();
    lv_tick_set_cb(tick);
    orb_ui_init(NULL);
    orb_geom_half_chords(s_half_chords, D);
    eligibility();
    create_fixture(&s_control, false);
    create_fixture(&s_candidate, true);
    compare_frames();
    CHECK(s_candidate.single_frames == 1 && s_candidate.double_frames == 0);

    /* Lighting changes invalidate just the glass, which must remain whole. */
    orb_glass_set_light(90, 0.8f);
    compare_frames();
    CHECK(s_candidate.doubled && s_candidate.double_frames == 1);

    lv_obj_send_event(s_control.glass, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(s_candidate.glass, LV_EVENT_PRESSED, NULL);
    for (unsigned i = 0; i < 8; i++) {
        s_now += 20;
        lv_timer_handler();
        compare_frames();
    }
    CHECK(s_candidate.double_frames > 1);
    if (argc > 1) {
        save_frame(&s_control, argv[1], "control");
        save_frame(&s_candidate, argv[1], "candidate");
    }

    /* A later full-screen refresh must rejoin the arena before drawing. */
    lv_obj_invalidate(lv_display_get_screen_active(s_control.display));
    lv_obj_invalidate(lv_display_get_screen_active(s_candidate.display));
    compare_frames();
    CHECK(!s_candidate.doubled && s_candidate.single_frames >= 2);
    lv_obj_set_style_bg_color(s_control.content, lv_color_hex(0x24405C), 0);
    lv_obj_set_style_bg_color(s_candidate.content, lv_color_hex(0x24405C), 0);
    compare_frames();
    CHECK(s_candidate.doubled);

    /* Cached full-frame motion may split the same arena without changing any
     * pixel. Repeated updates alternate halves; returning to live full damage
     * restores the whole band and its blur guard. */
    begin_snapshot(&s_control);
    begin_snapshot(&s_candidate);
    compare_frames();
    CHECK(!memcmp(s_control.pixels, s_control.frozen, sizeof(s_control.pixels)));
    CHECK(!memcmp(s_candidate.pixels, s_candidate.frozen, sizeof(s_candidate.pixels)));
    CHECK(s_candidate.doubled);
    unsigned doubles = s_candidate.double_frames;
    for (unsigned i = 0; i < 4; i++) {
        lv_obj_invalidate(s_control.snapshot);
        lv_obj_invalidate(s_candidate.snapshot);
        compare_frames();
        CHECK(s_candidate.doubled);
    }
    CHECK(s_candidate.double_frames == doubles + 4);
    s_control.snapshot_transition = s_candidate.snapshot_transition = false;
    lv_obj_delete(s_control.snapshot);
    lv_obj_delete(s_candidate.snapshot);
    lv_obj_invalidate(lv_display_get_screen_active(s_control.display));
    lv_obj_invalidate(lv_display_get_screen_active(s_candidate.display));
    compare_frames();
    CHECK(!s_candidate.doubled);
    live_scene_eligibility();
    printf("adaptive display buffers: %u checks; %u single/%u double frames, identical visible pixels\n",
           s_checks, s_candidate.single_frames, s_candidate.double_frames);
    lv_display_delete(s_control.display);
    lv_display_delete(s_candidate.display);
    lv_deinit();
    return 0;
}
