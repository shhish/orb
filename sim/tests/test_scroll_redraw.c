/* Compare the pixels retained by partial refresh with a fresh rendering of the
 * same frozen scene. The framebuffer stands in for panel RAM, so any mismatch
 * here is a software redraw artifact rather than camera/panel persistence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "orb_demo.h"
#include "orb_ui.h"
#include "sim_display.h"

#define BAND_ROWS 84
#define FRAME_BYTES (SIM_D * SIM_D * 3)

static uint32_t s_now;
static uint8_t s_band[SIM_D * BAND_ROWS * 2] __attribute__((aligned(64)));
static uint8_t s_partial[FRAME_BYTES], s_full[FRAME_BYTES];
static unsigned s_frames, s_failures;
static const char *s_capture_prefix;

static void require(bool ok, const char *why)
{
    if (ok) return;
    fprintf(stderr, "raster coverage regression: %s\n", why);
    exit(EXIT_FAILURE);
}

static void capture(const char *phase)
{
    if (!s_capture_prefix || (s_now != 5100 && s_now != 8600)) return;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s-%u-%s.png", s_capture_prefix, s_now, phase);
    require(n >= 0 && (size_t)n < sizeof(path), "capture path is too long");
    require(sim_display_save_png(path), "could not save capture");
}

static uint32_t tick(void)
{
    return s_now;
}

static void motion(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    /* Move the content and chrome across even-aligned windows while the list
     * is scrolling; level the scene again during the press/release phase. */
    float x = s_now >= 1800 && s_now < 3400 ? 0.40f : 0.0f;
    float y = s_now >= 4200 && s_now < 5800 ? -0.35f : 0.0f;
    orb_demo_tick(x, y, 1.0f, 0.020f);
}

static void check_frame(lv_display_t *display)
{
    lv_refr_now(display);
    sim_display_rgb(s_partial);
    capture("partial");
    /* A full invalidation must not advance animations or change the scene. */
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    sim_display_rgb(s_full);
    capture("full");
    s_frames++;
    if (!memcmp(s_partial, s_full, sizeof(s_full))) return;

    unsigned different = 0;
    int x1 = SIM_D, y1 = SIM_D, x2 = -1, y2 = -1;
    for (int y = 0; y < SIM_D; y++) {
        for (int x = 0; x < SIM_D; x++) {
            size_t offset = (size_t)(y * SIM_D + x) * 3;
            if (!memcmp(s_partial + offset, s_full + offset, 3)) continue;
            different++;
            x1 = LV_MIN(x1, x);
            y1 = LV_MIN(y1, y);
            x2 = LV_MAX(x2, x);
            y2 = LV_MAX(y2, y);
        }
    }
    fprintf(stderr, "partial redraw differs at %u ms (%s): %u pixels, [%d,%d..%d,%d]\n",
            s_now, orb_demo_phase(), different, x1, y1, x2, y2);
    s_failures++;
}

static void check_raster_children(lv_display_t *display)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 133, 160);
    lv_obj_set_size(row, 200, 120);
    lv_obj_t *child = lv_obj_create(row);
    lv_obj_remove_style_all(child);
    lv_obj_set_size(child, 200, 120);
    lv_obj_set_style_bg_color(child, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(child, LV_OPA_COVER, 0);
    lv_obj_set_raster_matte(row, lv_color_black());
    lv_obj_raster_prepare(screen);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    check_frame(display);

    /* The child is opaque, but is part of its parent's bitmap. Cover search
     * must not start drawing at that child and bypass the bitmap's opacity. */
    lv_obj_set_raster_opa(row, LV_OPA_50);
    check_frame(display);
    size_t center = (220 * SIM_D + 233) * 3;
    require(s_full[center] > 64 && s_full[center] < 192, "opaque child bypassed cached parent fade");

    lv_obj_set_raster_opa(row, LV_OPA_COVER);
    lv_obj_set_raster_scale(row, LV_SCALE_NONE / 2);
    check_frame(display);
    size_t left = (220 * SIM_D + 145) * 3;
    require(s_full[left] == 0 && s_full[left + 1] == 0 && s_full[left + 2] == 0,
            "shrunk cache skipped clearing its uncovered backdrop");
    lv_obj_set_raster_scale(row, 0);
    check_frame(display);
    require(s_full[center] == 0, "zero-scale cache left stale pixels");
    lv_obj_set_raster_scale(row, LV_SCALE_NONE);
    lv_obj_set_raster_opa(row, LV_OPA_TRANSP);
    check_frame(display);
    require(s_full[center] == 0, "zero-opacity cache left stale pixels");
    lv_obj_set_raster_opa(row, LV_OPA_COVER);
    check_frame(display);
    require(s_full[center] > 240, "restored cache lost its opaque child");
    lv_obj_raster_get_stats(&after);
    require(after.builds == before.builds, "coverage checks rebuilt the source bitmap");
    lv_obj_delete(row);
}

int main(int argc, char **argv)
{
    bool demo_only = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--demo-only")) demo_only = true;
        else if (!s_capture_prefix) s_capture_prefix = argv[i];
        else {
            fprintf(stderr, "usage: %s [capture-prefix] [--demo-only]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    /* Exercise the accepted firmware memory layout rather than relying on
     * overlap provided by the simulator's optional second buffer. */
    lv_display_set_buffers(display, s_band, NULL, sizeof(s_band), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_lock();
    orb_ui_init(NULL);
    if (!demo_only) check_raster_children(display);
    orb_demo_build(lv_screen_active());
    lv_timer_create(motion, 20, NULL);
    check_frame(display);
    for (s_now = 20; s_now <= 16000; s_now += 20) {
        lv_timer_handler();
        if (s_now % 100 == 0) check_frame(display);
    }
    lv_unlock();
    printf("scroll redraw: %u frozen frames, %u failures (single 466x84 band)\n", s_frames, s_failures);
    return s_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
