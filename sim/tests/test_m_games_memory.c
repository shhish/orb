/* Comparable retained LVGL payload, before/after Design 2.0. Native view
 * calloc storage, font files and allocator headers are deliberately excluded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_game_play.h"
#include "orb_game_view.h"
#include "orb_kit.h"
#include "sim_display.h"

static uint32_t s_now;
static uint32_t tick(void) { return s_now; }
static size_t bytes(void)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    return monitor.total_size;
}
static void settle(void)
{
    for (unsigned i = 0; i < 70; ++i) {
        s_now += 16;
        lv_timer_handler();
    }
    lv_refr_now(lv_display_get_default());
}
static void report(const char *mode, const char *name, size_t before,
                   const orb_glass_build_stats_t *glass_before)
{
    orb_glass_build_stats_t glass;
    orb_glass_build_stats(&glass);
    printf("GAMES-MEMORY %s %s retained=%lld shared-glass=%lld private=%lld\n",
        mode, name, (long long)bytes() - (long long)before,
        (long long)glass.bytes - (long long)glass_before->bytes,
        (long long)bytes() - (long long)before -
        ((long long)glass.bytes - (long long)glass_before->bytes));
}
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    bool ax = strcmp(mode, "default") != 0, bold = !strcmp(mode, "ax3-bold-rt");
    lv_init();
    lv_tick_set_cb(tick);
    if (!sim_display_create()) return EXIT_FAILURE;
    orb_ui_init(NULL);
    if (orb_fonts_set_content_size(ax ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) != ORB_FONTS_OK ||
        orb_fonts_set_bold_text(bold) != ORB_FONTS_OK) return EXIT_FAILURE;
    orb_ui_set_reduce_transparency(bold);
    orb_glass_cache_prewarm();
    settle();
    lv_obj_t *screen = lv_screen_active();
    static const char *const progress[ORB_GAME_COUNT] = {
        "Stage 2 of 3 · 7 keepsakes", "Settle into the time", "Best 40", "Best 24", "Best 128",
    };
    size_t before = bytes();
    orb_glass_build_stats_t glass;
    orb_glass_build_stats(&glass);
    orb_game_play_view_t *picker = orb_game_play_view_create(screen, NULL, NULL, progress);
    if (!picker) return EXIT_FAILURE;
    orb_game_play_view_select(picker, ORB_GAME_ORBIT);
    settle();
    report(mode, "picker", before, &glass);
    orb_game_play_view_destroy(picker);
    settle();
    for (orb_game_kind_t kind = 0; kind < ORB_GAME_COUNT; ++kind) {
        before = bytes();
        orb_glass_build_stats(&glass);
        orb_game_state_t state;
        orb_game_init(&state, kind, 0x4f5242u);
        state.started = true;
        state.score = 10;
        orb_game_view_t *view = orb_game_view_create(screen, kind, NULL, NULL);
        if (!view) return EXIT_FAILURE;
        orb_game_view_update(view, &state, 40, false, false);
        settle();
        report(mode, orb_game_id(kind), before, &glass);
        orb_game_view_destroy(view);
        settle();
    }
    return EXIT_SUCCESS;
}
