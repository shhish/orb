/* Execute the production Beads benchmark worker with virtual task waits.
 * These checks establish bounded logic/publication/lifetime ordering, not
 * ESP32 throughput or cross-core scheduling latency. */
#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "orb_game_logic.h"

typedef unsigned TickType_t;
typedef int *SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY UINT_MAX
#define pdMS_TO_TICKS(ms) (ms)

typedef struct {
    orb_game_state_t state, published_state;
    orb_game_pose_t pose, published_pose;
    uint64_t accumulator_us, steps, dropped_steps, logic_us, logic_max_us;
    uint64_t skipped_slots;
    uint64_t marble_gate_steps, marble_rewind_steps, marble_stages;
    uint64_t published_accumulator_us;
    int64_t previous_us, published_at_us, next_present_us;
    uint32_t period_us;
    uint16_t marble_history_peak;
    bool reduce_motion, frame_credit;
    atomic_bool worker_stop;
    SemaphoreHandle_t publication_lock, worker_done;
} game_bench_t;

static game_bench_t s_bench;
static int s_lock, s_done;
static int64_t s_now, s_stop_at;
static uint32_t s_step_us;
static unsigned s_batch_steps, s_max_batch_steps, s_publications, s_waits;
static unsigned s_cancel_step;
static unsigned s_force_samples;
static float s_last_x, s_last_y;
static bool s_started, s_stall, s_stalled;
static jmp_buf s_suspended;

static int64_t esp_timer_get_time(void) { return s_now; }
/* Credit tests below model the live display gate; TE-off producer admission
 * and fractional deadlines are covered by the actual callback fixture. */
static bool orb_display_te_pacing_active(void) { return true; }
static int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned timeout)
{
    assert(semaphore == &s_lock && timeout == portMAX_DELAY && !s_lock);
    s_lock = 1;
    return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == &s_done) {
        assert(!s_lock && atomic_load(&s_bench.worker_stop));
        s_done = 1;
        return;
    }
    assert(semaphore == &s_lock && s_lock);
    s_lock = 0;
    if (s_batch_steps > s_max_batch_steps) s_max_batch_steps = s_batch_steps;
    s_batch_steps = 0;
    ++s_publications;
}
static unsigned ulTaskNotifyTake(int clear, unsigned timeout)
{
    assert(clear == pdTRUE && !s_lock);
    if (!s_started) {
        assert(timeout == portMAX_DELAY);
        s_started = true;
        return 1;
    }
    assert(timeout != portMAX_DELAY);
    s_now += (int64_t)timeout * 1000;
    if (s_stall && !s_stalled && s_now >= 40000) {
        s_now += 180000;
        s_stalled = true;
    }
    if (s_now >= s_stop_at) atomic_store(&s_bench.worker_stop, true);
    ++s_waits;
    return 0;
}
static void taskYIELD(void)
{
    assert(!s_lock);
    s_now += 50;
    if (s_now >= s_stop_at) atomic_store(&s_bench.worker_stop, true);
}
static void vTaskSuspend(void *task)
{
    assert(!task && s_done && !s_lock);
    longjmp(s_suspended, 1);
}
static void timed_game_step(orb_game_state_t *state, float x, float y, bool valid, bool reduce_motion)
{
    /* The real solver must never block a UI snapshot behind its CPU work. */
    assert(!s_lock);
    assert(valid && x * x + y * y >= .1599f && x * x + y * y <= .3026f);
    if (s_force_samples) {
        float dx = x - s_last_x, dy = y - s_last_y;
        assert(dx * dx + dy * dy > .0001f); /* No repeated/flat force samples. */
    }
    s_last_x = x; s_last_y = y; ++s_force_samples;
    assert(state->motion_moving && !state->game.beads.settle_ticks);
    orb_game_step(state, x, y, valid, reduce_motion);
    s_now += s_step_us;
    ++s_batch_steps;
    if (s_cancel_step && state->ticks >= s_cancel_step)
        atomic_store(&s_bench.worker_stop, true);
}

#define orb_game_step timed_game_step
#include "devtools_game_worker.inc"
#undef orb_game_step

static void run(bool stall, unsigned cancel_step)
{
    memset(&s_bench, 0, sizeof(s_bench));
    s_now = 0; s_stop_at = 1000000; s_step_us = 2000;
    s_lock = s_done = 0;
    s_batch_steps = s_max_batch_steps = s_publications = s_waits = 0;
    s_started = s_stalled = false;
    s_force_samples = 0;
    s_stall = stall; s_cancel_step = cancel_step;
    s_bench.publication_lock = &s_lock;
    s_bench.worker_done = &s_done;
    atomic_init(&s_bench.worker_stop, false);
    orb_game_init(&s_bench.state, ORB_GAME_BEADS, 0x4f5242);
    if (!setjmp(s_suspended)) game_benchmark_worker(&s_bench);
    assert(s_done && !s_lock && s_publications && s_max_batch_steps <= 6);
    assert(s_bench.logic_us == s_bench.steps * s_step_us);
    assert(s_bench.logic_max_us == s_step_us);
    assert(s_bench.state.ticks == s_bench.steps);
    assert(s_bench.published_state.ticks == s_bench.state.ticks);
    assert(s_bench.published_pose.ticks + 1 == s_bench.state.ticks);
    assert(s_bench.published_accumulator_us +
        (uint64_t)(s_bench.previous_us - s_bench.published_at_us) == s_bench.accumulator_us);
    if (cancel_step) {
        assert(s_bench.steps == cancel_step);
        assert(!s_bench.dropped_steps); /* Cancellation is not a missed step. */
    } else if (stall) {
        assert(s_stalled && s_max_batch_steps == 6 && s_bench.dropped_steps >= 3);
    } else {
        assert(s_bench.steps >= 59 && s_bench.steps <= 60);
        assert(!s_bench.dropped_steps && s_max_batch_steps == 1 && s_waits);
    }
}

static void continuous_pour_trace(void)
{
    memset(&s_bench, 0, sizeof(s_bench));
    s_lock = s_cancel_step = s_force_samples = 0;
    orb_game_init(&s_bench.state, ORB_GAME_BEADS, 0x4f5242);
    orb_game_beads_set_time(&s_bench.state, 10, 9);
    for (unsigned i = 0; i < 360; ++i) {
        game_benchmark_drive(&s_bench);
        assert(!s_bench.state.game.beads.settled);
        assert(!s_bench.state.game.beads.settle_ticks);
    }
    assert(s_force_samples == 360);
}

int main(void)
{
    run(false, 0);
    run(true, 0);
    run(true, 3);
    continuous_pour_trace();
    s_bench.period_us = 16918;
    s_bench.next_present_us = 10000;
    s_bench.skipped_slots = 0;
    s_bench.frame_credit = false;
    assert(!game_benchmark_present_due(&s_bench, 1000));
    s_bench.frame_credit = true;
    assert(game_benchmark_present_due(&s_bench, 1000));
    assert(s_bench.next_present_us == 10000 && !s_bench.skipped_slots);
    assert(!game_benchmark_present_due(&s_bench, 1000));
    assert(game_benchmark_present_due(&s_bench, 10000));
    assert(s_bench.next_present_us == 26918 && !s_bench.skipped_slots);
    assert(game_benchmark_present_due(&s_bench, 80000));
    assert(s_bench.next_present_us == 94590 && s_bench.skipped_slots == 3);
    puts("game benchmark worker: cadence, six-step cap, unlocked solver and cancellation passed");
    return 0;
}
