/* Headless Play picker contact-sheet render: the production orb_game_play_view
 * over the five games, at a chosen index and accessibility variant. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "orb_fonts.h"
#include "orb_game_logic.h"
#include "orb_game_play.h"
#include "orb_ui.h"
#include "sim_board.h"
#include "sim_display.h"

static uint32_t s_now;
static uint32_t tick(void) { return s_now; }

int main(int argc, char **argv)
{
    const char *shot_path = "play.png";
    unsigned index = 0;
    orb_content_size_t size = ORB_CONTENT_SIZE_DEFAULT;
    bool bold = false, rt = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--ax3")) size = ORB_CONTENT_SIZE_AX3;
        else if (!strcmp(argv[i], "--bold")) bold = true;
        else if (!strcmp(argv[i], "--rt")) rt = true;
        else if (i + 1 < argc && !strcmp(argv[i], "--index")) {
            index = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (i + 1 < argc && !strcmp(argv[i], "--shot")) shot_path = argv[++i];
        else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
    }
    if (index >= ORB_GAME_COUNT) { fprintf(stderr, "index out of range\n"); return 2; }

    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    orb_ui_init(NULL);
    if (orb_fonts_set_content_size(size) != ORB_FONTS_OK ||
        orb_fonts_set_bold_text(bold) != ORB_FONTS_OK) return 1;
    orb_ui_set_reduce_transparency(rt);

    static const char *const progress[ORB_GAME_COUNT] = {
        "Stage 2 of 3 · 7 keepsakes", "Settle into the time",
        "Best 31", "Best 24", "Best 128",
    };
    orb_game_play_view_t *view = orb_game_play_view_create(lv_screen_active(), NULL, NULL, progress);
    if (!view) return 1;
    orb_game_play_view_select(view, (orb_game_kind_t)index);
    for (unsigned i = 0; i < 40; ++i) { s_now += 33; lv_timer_handler(); }
    lv_refr_now(display);
    if (!sim_display_save_png(shot_path)) return 1;
    printf("orb_play_sim: index=%u -> %s\n", index, shot_path);
    orb_game_play_view_destroy(view);
    return 0;
}
