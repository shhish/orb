#include <assert.h>
#include <stdio.h>
#include "lvgl.h"
#include "orb_display_power.h"

static uint32_t s_now;
static unsigned s_flushes, s_renders, s_updates, s_draws;
static uint32_t s_pacing_delay;
static bool s_panel_on, s_ready;
static uint8_t s_pixels[32 * 32 * 2];

static uint32_t tick(void) { return s_now; }
static uint32_t gate(lv_display_t *display)
{
    (void)display;
    uint32_t power = orb_display_power_refresh_delay(s_panel_on, s_ready);
    return power ? power : s_pacing_delay;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    s_flushes++;
    lv_display_flush_ready(display);
}
static void render(lv_event_t *event) { (void)event; s_renders++; }
static void draw(lv_event_t *event) { (void)event; s_draws++; }
static void snapshots(bool allowed)
{
    lv_draw_buf_t *buffer = lv_draw_buf_create(32, 32, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    assert(buffer);
    unsigned draws = s_draws;
    lv_result_t result = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, buffer);
    assert(result == (allowed ? LV_RESULT_OK : LV_RESULT_INVALID));
    lv_area_t area = {0, 0, 31, 15};
    result = lv_snapshot_take_area_to_draw_buf(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888, &area, buffer);
    assert(result == (allowed ? LV_RESULT_OK : LV_RESULT_INVALID));
    assert(allowed ? s_draws > draws : s_draws == draws);
    lv_draw_buf_destroy(buffer);
}
static void retained_animation(lv_timer_t *timer)
{
    (void)timer;
    s_updates++;
    lv_obj_invalidate(lv_screen_active());
}
static void advance(uint32_t duration)
{
    uint32_t end = s_now + duration;
    while (s_now < end) { s_now++; lv_timer_handler(); }
}
int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = lv_display_create(32, 32);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_pixels, NULL, sizeof(s_pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_refresh_gate_cb(display, gate);
    lv_display_add_event_cb(display, render, LV_EVENT_RENDER_START, NULL);
    lv_obj_add_event_cb(lv_screen_active(), draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(lv_layer_top(), draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_timer_create(retained_animation, 20, NULL);
    advance(2000);
    assert(s_updates == 100 && !s_renders && !s_flushes);
    lv_refr_now(display); /* Devtools captures obey the hard power gate too. */
    assert(!s_renders && !s_flushes);
    snapshots(false);
    s_panel_on = true; /* SLPOUT has not yet settled. */
    advance(1000);
    assert(!s_renders && !s_flushes);
    lv_refr_now(display);
    assert(!s_renders && !s_flushes);
    snapshots(false);
    s_ready = true;
    advance(100);
    assert(s_renders && s_flushes);
    s_pacing_delay = 25; /* Synchronous snapshots still bypass ordinary pacing. */
    snapshots(true);
    s_pacing_delay = 0;
    unsigned renders = s_renders, flushes = s_flushes;
    s_panel_on = false;
    advance(1000);
    assert(s_renders == renders && s_flushes == flushes);
    snapshots(false);
    assert(orb_display_te_fresh(100000, 99999, 16800, 800));
    assert(!orb_display_te_fresh(100000, 100001, 16800, 800));
    assert(!orb_display_te_fresh(167200, 100000, 16800, 800));
    assert(!orb_display_te_fresh(1000000, 999999, 900000, 800));
    assert(!orb_display_te_fresh(100000, 99999, 16800, 0));
    puts("dark refresh: LVGL timers, forced refresh and snapshots obey power gate; wake and silent TE passed");
    lv_display_delete(display);
}
