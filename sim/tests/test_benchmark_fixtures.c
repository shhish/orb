/* Deterministic fixtures establish a real idle/transition baseline without
 * coupling their cadence to the host clock or modifying the original demo. */
#include <stdio.h>
#include <stdlib.h>

#include "orb_demo_fixture.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks, s_frames;

#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static uint32_t tick(void) { return s_now; }

static void rendered(lv_event_t *event)
{
    LV_UNUSED(event);
    s_frames++;
}

static void advance(unsigned duration)
{
    for (unsigned elapsed = 0; elapsed < duration; elapsed += 20) {
        s_now += 20;
        lv_timer_handler();
    }
}

static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static void save(const char *directory, const char *fixture, unsigned mode)
{
    if (!directory) return;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/fixture-%s-%u.png", directory, fixture, mode);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    CHECK(sim_display_save_png(path));
}

int main(int argc, char **argv)
{
    CHECK(argc <= 2);
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    CHECK(display != NULL);
    lv_display_add_event_cb(display, rendered, LV_EVENT_RENDER_READY, NULL);
    orb_ui_init(NULL);
    lv_obj_t *saved = lv_screen_active();
    lv_obj_set_style_bg_color(saved, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(saved, LV_OPA_COVER, 0);
    advance(100);
    unsigned base_timers = timers();
    unsigned base_animations = lv_anim_count_running();

    for (unsigned mode = 0; mode < 4; mode++) {
        CHECK(orb_fonts_set_content_size(mode & 1 ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency((mode & 2) != 0);
        lv_obj_t *scene = lv_obj_create(NULL);
        CHECK(scene != NULL);
        orb_demo_fixture_build(scene, ORB_DEMO_FIXTURE_QUIET);
        lv_screen_load(scene);
        advance(100);
        save(argc == 2 ? argv[1] : NULL, "quiet", mode);
        unsigned frames = s_frames;
        lv_obj_raster_stats_t before, after;
        lv_obj_raster_get_stats(&before);
        sim_display_take_changed();
        advance(25000);
        lv_obj_raster_get_stats(&after);
        CHECK(s_frames == frames);
        CHECK(!sim_display_take_changed());
        CHECK(after.builds == before.builds);
        CHECK(timers() == base_timers);
        CHECK(lv_anim_count_running() == base_animations);
        lv_screen_load(saved);
        lv_obj_delete(scene);
        advance(100);

        scene = lv_obj_create(NULL);
        CHECK(scene != NULL);
        orb_demo_fixture_build(scene, ORB_DEMO_FIXTURE_TRANSITION);
        lv_screen_load(scene);
        advance(100);
        frames = s_frames;
        lv_obj_raster_get_stats(&before);
        advance(2000);
        lv_obj_raster_get_stats(&after);
        CHECK(s_frames >= frames + 50);
        /* Stage 0 measures the existing transition rather than asserting its
         * future snapshot-stage source-build budget. Keep that work visible. */
        printf("transition mode %u: %u frames, %u source builds\n", mode,
               s_frames - frames, after.builds - before.builds);
        CHECK(lv_anim_count_running() == base_animations + 1);
        save(argc == 2 ? argv[1] : NULL, "transition", mode);
        lv_screen_load(saved);
        lv_obj_delete(scene);
        advance(100);
        frames = s_frames;
        sim_display_take_changed();
        advance(2000);
        CHECK(s_frames == frames);
        CHECK(!sim_display_take_changed());
        CHECK(timers() == base_timers);
        CHECK(lv_anim_count_running() == base_animations);
    }
    printf("benchmark fixtures: %u checks passed (four 25s quiet windows and transition lifetimes)\n", s_checks);
    return EXIT_SUCCESS;
}
