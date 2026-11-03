/* Pixel and cache-lifetime regressions through the real LVGL renderer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "orb_demo.h"
#include "orb_ui.h"
#include "sim_display.h"

#define D 466
static uint32_t s_now;
static unsigned char s_rgb[D * D * 3], s_still[D * D * 3];

static uint32_t tick(void) { return s_now; }
static void require(bool ok, const char *why)
{
    if (!ok) {
        fprintf(stderr, "scene regression: %s\n", why);
        exit(1);
    }
}

static int white_pixels(int x1, int y1, int x2, int y2)
{
    int n = 0;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            unsigned char *p = &s_rgb[(y * D + x) * 3];
            n += p[0] > 200 && p[1] > 200 && p[2] > 200;
        }
    }
    return n;
}

static void motion(lv_timer_t *t)
{
    LV_UNUSED(t);
    orb_demo_tick(0, 0, 1, 0.020f);
}

static void check_scale(lv_display_t *d)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 66, 180);
    lv_obj_set_size(row, 333, 85);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_raster_matte(row, lv_color_black());
    lv_obj_raster_prepare(screen);
    lv_refr_now(d);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    lv_obj_set_raster_scale(row, LV_SCALE_NONE / 2);
    lv_refr_now(d);
    sim_display_rgb(s_rgb);
    require(white_pixels(66, 180, 148, 264) == 0, "scaled row escaped its left bound");
    require(white_pixels(200, 210, 220, 230) == 441, "scaled row lost its center");
    lv_obj_set_raster_scale(row, 0);
    lv_refr_now(d);
    sim_display_rgb(s_rgb);
    require(white_pixels(66, 180, 398, 264) == 0, "zero scale did not hide the row");
    lv_obj_set_raster_scale(row, LV_SCALE_NONE);
    lv_refr_now(d);
    lv_obj_raster_get_stats(&after);
    require(after.builds == before.builds, "scaling rebuilt the source cache");
}

int main(int argc, char **argv)
{
    bool reduced = argc > 1 && !strcmp(argv[1], "--reduce-motion");
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = sim_display_create();
    lv_lock();
    orb_ui_init(NULL);
    if (argc > 1 && !strcmp(argv[1], "--scale")) {
        check_scale(d);
        lv_unlock();
        puts("raster scale: pixels and cache reuse passed");
        return 0;
    }
    orb_ui_set_reduce_motion(reduced);
    orb_demo_build(lv_screen_active());
    lv_timer_create(motion, 20, NULL);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    bool captured_still = false;
    /* Exercise the normal timer path without forcing intermediate refreshes. */
    while (s_now < 8100) {
        uint32_t wait = lv_timer_handler();
        if (reduced && s_now >= 2000 && !captured_still) {
            sim_display_rgb(s_still);
            captured_still = true;
        }
        s_now += wait ? LV_MIN(wait, 1000) : 1;
        s_now = LV_MIN(s_now, 8100);
    }
    lv_timer_handler();
    lv_refr_now(d);
    sim_display_rgb(s_rgb);
    require(white_pixels(191, 46, 275, 89) > 200, "clock disappeared after partial redraws");
    require(white_pixels(348, 84, 384, 118) > 60, "toolbar plus disappeared after partial redraws");
    lv_obj_raster_get_stats(&after);
    require(after.builds == before.builds, "scroll scale/fade rebuilt row caches");
    if (reduced) require(captured_still && !memcmp(s_rgb, s_still, sizeof(s_rgb)), "Reduce Motion still animates");
    lv_unlock();
    puts("demo scene: chrome pixels, row cache reuse and motion checks passed");
    return 0;
}
