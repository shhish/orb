/* Exercise the real observer seams, including callbacks that retire their
 * timer/display. Observer identities must never be read from freed objects. */
#include <stdio.h>
#include <stdlib.h>
#include "lvgl.h"

static uint32_t s_now;
static unsigned s_checks, s_count, s_timer_begin, s_timer_end;
static lv_display_refresh_phase_t s_phases[32];
static uint8_t s_pixels[32 * 32 * 2];
#define CHECK(c) do { s_checks++; if (!(c)) { \
    fprintf(stderr, "refresh phases:%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static uint32_t tick(void) { return s_now; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    LV_UNUSED(area); LV_UNUSED(pixels);
    lv_display_flush_ready(display);
}
static void phase(lv_display_t *display, lv_display_refresh_phase_t value)
{
    LV_UNUSED(display);
    CHECK(value < LV_DISPLAY_REFRESH_PHASE_COUNT && s_count < 32);
    s_phases[s_count++] = value;
}
static lv_display_t *display_create(void)
{
    lv_display_t *display = lv_display_create(32, 32);
    CHECK(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_pixels, NULL, sizeof(s_pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_refresh_phase_cb(display, phase);
    s_count = 0;
    return display;
}
static void delete_display(lv_event_t *event) { lv_display_delete(lv_event_get_current_target(event)); }
static void retiring_timer(lv_timer_t *timer) { lv_timer_delete(timer); }
static void timer_observer(lv_timer_cb_t callback, bool entering)
{
    if (callback != retiring_timer) return;
    if (entering) s_timer_begin++;
    else s_timer_end++;
}
int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = display_create();
    lv_refr_now(display);
    CHECK(s_count == LV_DISPLAY_REFRESH_PHASE_COUNT);
    for (unsigned i = 0; i < s_count; i++) CHECK(s_phases[i] == (i + 1) % LV_DISPLAY_REFRESH_PHASE_COUNT);
    lv_display_set_refresh_phase_cb(display, NULL);
    s_count = 0;
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(display);
    CHECK(!s_count);
    lv_display_delete(display);

    display = display_create();
    lv_display_add_event_cb(display, delete_display, LV_EVENT_REFR_READY, NULL);
    lv_refr_now(display);
    CHECK(s_count && s_phases[s_count - 1] == LV_DISPLAY_REFRESH_PHASE_IDLE);

    display = display_create();
    lv_display_add_event_cb(display, delete_display, LV_EVENT_REFR_START, NULL);
    lv_refr_now(display);
    CHECK(s_count == 2 && s_phases[0] == LV_DISPLAY_REFRESH_PHASE_PREPARE &&
          s_phases[1] == LV_DISPLAY_REFRESH_PHASE_IDLE);

    lv_timer_set_observer_cb(timer_observer);
    lv_timer_create(retiring_timer, 1, NULL);
    s_now = 10; lv_timer_handler();
    CHECK(s_timer_begin == 1 && s_timer_end == 1);
    lv_timer_set_observer_cb(NULL);
    lv_timer_create(retiring_timer, 1, NULL);
    s_now = 20; lv_timer_handler();
    CHECK(s_timer_begin == 1 && s_timer_end == 1);
    printf("refresh/timer observers: %u checks passed\n", s_checks);
}
