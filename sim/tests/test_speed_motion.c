/* Motion-quality lifetime and wraparound checks against the real LVGL timer.
 * Timing assertions use virtual ticks; host scheduler speed is irrelevant. */
#include <stdio.h>
#include <stdlib.h>

#include "lvgl.h"

#if (defined(CONFIG_ORB_DISPLAY_GLASS_MOTION_QUALITY) && CONFIG_ORB_DISPLAY_GLASS_MOTION_QUALITY) || \
    (defined(CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) && CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) || \
    (defined(CONFIG_ORB_DISPLAY_TE_PACING) && CONFIG_ORB_DISPLAY_TE_PACING)
static uint32_t s_now;
static unsigned s_checks;
static uint8_t s_buffer[64 * 64 * 2];

static uint32_t tick(void) { return s_now; }

static void require(bool ok, const char *why)
{
    s_checks++;
    if (ok) return;
    fprintf(stderr, "motion quality regression: %s\n", why);
    exit(EXIT_FAILURE);
}

static unsigned timer_count(void)
{
    unsigned count = 0;
    lv_timer_t *timer = NULL;
    while ((timer = lv_timer_get_next(timer))) count++;
    return count;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    LV_UNUSED(area);
    LV_UNUSED(pixels);
    lv_display_flush_ready(display);
}

static lv_display_t *create_display(void)
{
    lv_display_t *display = lv_display_create(64, 64);
    require(display != NULL, "display allocation failed");
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_buffer, NULL, sizeof(s_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    return display;
}

static void advance(uint32_t elapsed)
{
    s_now += elapsed;
    lv_timer_handler();
}

int main(void)
{
    /* Repeated init/deinit checks that deleted timer pointers cannot survive
     * a simulator reset or a new display at a recycled allocation address. */
    for (unsigned cycle = 0; cycle < 3; cycle++) {
        lv_init();
        s_now = UINT32_MAX - 60;
        lv_tick_set_cb(tick);
        unsigned initial_timers = timer_count();
        lv_display_t *display = create_display();
        require(!lv_draw_sw_blur_motion_active(display), "new display inherited motion");
        unsigned before_motion = timer_count();
        lv_draw_sw_blur_note_motion(NULL);
        require(timer_count() == before_motion, "null motion allocated a timer");
        lv_draw_sw_blur_note_motion(display);
        require(lv_draw_sw_blur_motion_active(display), "motion was not activated");
        require(timer_count() == before_motion + 1, "motion timer was not uniquely owned");

        advance(119); /* Cross UINT32_MAX; 119 ms is still moving. */
        require(lv_draw_sw_blur_motion_active(display), "motion settled before 120 ms across wrap");
        advance(41);  /* The 40 ms settle timer is now due. */
        require(!lv_draw_sw_blur_motion_active(display), "motion did not settle across tick wrap");

        lv_draw_sw_blur_note_motion(display);
        advance(80);
        lv_draw_sw_blur_note_motion(display);
        require(timer_count() == before_motion + 1, "repeat movement duplicated the timer");
        advance(119);
        require(lv_draw_sw_blur_motion_active(display), "repeat movement did not extend settling");
        advance(41);
        require(!lv_draw_sw_blur_motion_active(display), "repeat movement did not settle");

        lv_draw_sw_blur_note_motion(display);
        lv_display_delete(display);
        require(timer_count() == initial_timers, "display deletion leaked its motion timer");
        display = create_display();
        require(!lv_draw_sw_blur_motion_active(display), "recreated display inherited deleted motion");
        lv_draw_sw_blur_note_motion(display);
        advance(160);
        require(!lv_draw_sw_blur_motion_active(display), "recreated display did not settle");
        lv_display_delete(display);
        require(timer_count() == initial_timers, "recreated display leaked its motion timer");
        lv_deinit();
    }
    printf("motion quality: %u virtual-tick/lifetime checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
#else
int main(void)
{
    puts("motion quality: option disabled (no experimental state)");
    return EXIT_SUCCESS;
}
#endif
