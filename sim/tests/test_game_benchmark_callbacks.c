/* Real benchmark refresh callbacks with virtual LVGL events. Clean/inactive
 * refresh attempts cannot create frame credit or spin a static producer. */
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    void *view, *screen, *timer;
    bool refresh_had_damage, frame_credit;
    int64_t next_present_us;
    uint32_t period_us;
    uint64_t skipped_slots;
} game_bench_t;
typedef struct { game_bench_t *data; } lv_event_t;

static void *s_screen;
static bool s_pending, s_te_pacing = true;
static unsigned s_sampled, s_ready;
static game_bench_t *lv_event_get_user_data(lv_event_t *event) { return event->data; }
static void *lv_screen_active(void) { return s_screen; }
static bool orb_display_refresh_pending(void) { return s_pending; }
static bool orb_display_te_pacing_active(void) { return s_te_pacing; }
static void game_benchmark_render_latest(game_bench_t *bench)
{
    assert(bench->view && bench->screen == s_screen && s_pending);
    ++s_sampled;
}
static void lv_timer_ready(void *timer) { assert(timer); ++s_ready; }

#include "devtools_game_callbacks.inc"
#include "devtools_game_due.inc"

int main(void)
{
    int screen, other_screen, view, timer;
    game_bench_t bench = {.view = &view, .screen = &screen, .timer = &timer};
    lv_event_t event = {.data = &bench};
    s_screen = &screen; s_pending = true;
    game_benchmark_refresh_start(&event);
    assert(s_sampled == 1 && bench.refresh_had_damage && !bench.frame_credit);
    game_benchmark_refresh_ready(&event);
    assert(s_ready == 1 && !bench.refresh_had_damage && bench.frame_credit);
    game_benchmark_refresh_ready(&event);
    assert(s_ready == 1); /* The completion credit is one-shot. */
    bench.frame_credit = false;
    s_pending = false;
    for (unsigned i = 0; i < 100; ++i) {
        game_benchmark_refresh_start(&event);
        game_benchmark_refresh_ready(&event);
    }
    assert(s_sampled == 1 && s_ready == 1 && !bench.frame_credit);
    s_screen = &other_screen; s_pending = true;
    game_benchmark_refresh_start(&event);
    game_benchmark_refresh_ready(&event);
    assert(s_sampled == 1 && s_ready == 1 && !bench.frame_credit);
    s_screen = &screen; bench.view = NULL;
    game_benchmark_refresh_start(&event);
    game_benchmark_refresh_ready(&event);
    assert(s_sampled == 1 && s_ready == 1 && !bench.frame_credit);
    bench.view = &view;
    bench.frame_credit = false;
    bench.period_us = 16920;
    bench.next_present_us = bench.period_us;
    s_te_pacing = false;
    s_pending = true;
    for (unsigned i = 0; i < 100; ++i) {
        game_benchmark_refresh_start(&event);
        game_benchmark_refresh_ready(&event);
        assert(!bench.frame_credit && !game_benchmark_present_due(&bench, 1000));
    }
    assert(s_ready == 1); /* No unpaced READY feedback loop. */
    unsigned frames = 0;
    for (int64_t now = 0; now <= 1692000; now += 1000) {
        if (game_benchmark_present_due(&bench, now)) ++frames;
        assert(!game_benchmark_present_due(&bench, now));
    }
    assert(frames == 100 && !bench.skipped_slots);
    assert(game_benchmark_present_due(&bench, 2000000));
    assert(bench.skipped_slots > 0 && !game_benchmark_present_due(&bench, 2000000));
    /* TE admission may consume one credit early. Its existing grid remains
     * authoritative and consuming it never advances nominal missed slots. */
    s_te_pacing = true;
    bench.frame_credit = true;
    int64_t deadline = bench.next_present_us;
    assert(game_benchmark_present_due(&bench, 2000000));
    assert(!bench.frame_credit && bench.next_present_us == deadline);
    assert(!game_benchmark_present_due(&bench, 2000000));
    puts("game benchmark callbacks: active damage credit, clean idle and inactive cleanup passed");
    return 0;
}
