/* Repeated developer benchmarks must leave no scene timers or light animation
 * behind. Exercise both phases and destruction during a glass press. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_demo.h"
#include "orb_ui.h"
#include "sim_display.h"

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

static unsigned timer_count(void)
{
    unsigned count = 0;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) count++;
    return count;
}

static void advance(unsigned ms, bool demo)
{
    uint32_t end = s_now + ms;
    while (s_now < end) {
        s_now += 20;
        if (demo) orb_demo_tick(0, 0, 1, 0.020f);
        lv_timer_handler();
    }
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    CHECK(display != NULL);
    orb_ui_init(NULL);
    lv_obj_t *saved = lv_screen_active();
    lv_obj_set_style_bg_color(saved, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(saved, LV_OPA_COVER, 0);
    advance(100, false);
    unsigned timers = timer_count();
    unsigned animations = lv_anim_count_running();

    const unsigned durations[] = {1000, 8500, 8500, 1000};
    for (unsigned pass = 0; pass < sizeof(durations) / sizeof(durations[0]); pass++) {
        bool reduced = pass == 3;
        orb_ui_set_reduce_motion(reduced);
        lv_obj_t *scene = lv_obj_create(NULL);
        CHECK(scene != NULL);
        orb_demo_build(scene);
        lv_screen_load(scene);
        CHECK(!strcmp(orb_demo_phase(), "scroll"));
        advance(durations[pass], true);
        CHECK(!strcmp(orb_demo_phase(), durations[pass] > 8000 ? "glass" : "scroll"));
        CHECK(sim_display_take_changed());

        lv_screen_load(saved);
        lv_obj_delete(scene);
        CHECK(lv_screen_active() == saved);
        CHECK(timer_count() == timers);
        CHECK(lv_anim_count_running() == animations);
        lv_refr_now(display);
        sim_display_take_changed();
        /* Longer than either phase timer. A leaked callback used to operate
         * on freed scene pointers, or drive global light on a later screen. */
        advance(9000, false);
        CHECK(timer_count() == timers);
        CHECK(lv_anim_count_running() == animations);
        CHECK(!sim_display_take_changed());
    }
    orb_ui_set_reduce_motion(false);
    printf("demo lifecycle: %u checks passed across four create/destroy cycles\n", s_checks);
    return 0;
}
