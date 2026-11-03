/*
 * orb_fonts test app: links every Orb font (for idf.py size-components) and, when
 * flashed, runs the self-test and times LVGL rendering a screen of each style into
 * 104-row bands in internal RAM with two draw threads, as orb_display does. Nothing is
 * sent to the panel. Normal faces require the signed pack in assets. Sign this
 * application and the envelope with the same throwaway key before any lead-run test.
 */

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "orb_fonts_selftest.h"
#include "orb_assets.h"

#define HOR 466
#define BAND_ROWS 104

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint64_t now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static void flush_done(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

void app_main(void)
{
    (void)orb_assets_init();
    lv_init();
    lv_tick_set_cb(tick_ms);

    lv_display_t *disp = lv_display_create(HOR, HOR);
    size_t bytes = HOR * BAND_ROWS * 2;
    void *buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_done);

    orb_fonts_selftest(disp, now_us, false);
}
