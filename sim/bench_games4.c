/* Same 60 Hz production input/retained drawing trace before and after Games 4.
 * draw_ms times only LVGL refresh, separately from fixed-step game physics. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "orb_game_logic.h"
#include "orb_game_view.h"
#include "orb_fonts.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static uint64_t s_draw_begin, s_draw_us, s_cpu_begin, s_cpu_us;
static uint32_t tick(void) { return s_now; }
static uint64_t micros(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
static uint64_t cpu_micros(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void draw_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        s_draw_begin = micros();
        s_cpu_begin = cpu_micros();
    } else {
        s_draw_us += micros() - s_draw_begin;
        s_cpu_us += cpu_micros() - s_cpu_begin;
    }
}
static void action(int id, float x, float y, void *context)
{
    (void)id; (void)x; (void)y; (void)context;
}
int main(void)
{
    enum { WARM = 60, FRAMES = 360 };
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    /* Count every render, including one requested by LVGL's timer handler. */
    lv_display_add_event_cb(display, draw_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(display, draw_event, LV_EVENT_RENDER_READY, NULL);
    orb_ui_init(NULL);
    if (orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) != ORB_FONTS_OK) return 1;
    for (orb_game_kind_t kind = ORB_GAME_MARBLE; kind < ORB_GAME_COUNT; ++kind) {
        orb_game_state_t state;
        orb_game_init(&state, kind, 0x4f5242u);
        if (kind == ORB_GAME_BEADS) orb_game_beads_set_time(&state, 10, 9);
        orb_game_view_t *view = orb_game_view_create(lv_screen_active(), kind, action, NULL);
        if (!view) return 2;
        uint64_t draws[FRAMES], cpu[FRAMES], logic[FRAMES], sum_draw = 0, sum_cpu = 0, sum_logic = 0;
        for (unsigned frame = 0; frame < WARM + FRAMES; ++frame) {
            /* Continuous tilted pours, normal puzzle motion and endless rounds.
             * Restart losses through the actual action, never by editing score. */
            if (state.game_over) orb_game_action(&state, ORB_GAME_RESTART, 0, 0);
            if (kind == ORB_GAME_ORBIT && frame % 47u == 0)
                orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
            if (kind == ORB_GAME_POP && frame % 30u == 0)
                orb_game_action(&state, ORB_GAME_PRIMARY, 0, 0);
            float t = (float)frame / 60.0f;
            uint64_t begin = micros();
            orb_game_step(&state, 0.30f * sinf(t * 1.4f), 0.14f * cosf(t), true, false);
            uint64_t logic_us = micros() - begin;
            s_now += frame % 3u == 0 ? 16u : 17u;
            s_draw_us = s_cpu_us = 0;
            orb_game_view_update(view, &state, 120, false, false);
            lv_timer_handler();
            lv_refr_now(display);
            uint64_t draw_us = s_draw_us;
            if (frame >= WARM) {
                draws[frame - WARM] = draw_us;
                cpu[frame - WARM] = s_cpu_us;
                logic[frame - WARM] = logic_us;
                sum_draw += draw_us;
                sum_cpu += s_cpu_us;
                sum_logic += logic_us;
            }
        }
        qsort(draws, FRAMES, sizeof(*draws), compare);
        qsort(cpu, FRAMES, sizeof(*cpu), compare);
        qsort(logic, FRAMES, sizeof(*logic), compare);
        printf("%s draw_ms mean=%.3f p95=%.3f max=%.3f draw_cpu_ms mean=%.3f p95=%.3f max=%.3f logic_ms mean=%.3f p95=%.3f frames=%u\n",
               orb_game_id(kind), (double)sum_draw / FRAMES / 1000.0,
               (double)draws[FRAMES * 95 / 100] / 1000.0, (double)draws[FRAMES-1] / 1000.0,
               (double)sum_cpu / FRAMES / 1000.0, (double)cpu[FRAMES * 95 / 100] / 1000.0,
               (double)cpu[FRAMES-1] / 1000.0,
               (double)sum_logic / FRAMES / 1000.0, (double)logic[FRAMES * 95 / 100] / 1000.0, FRAMES);
        orb_game_view_destroy(view);
        lv_refr_now(display);
    }
    return 0;
}
