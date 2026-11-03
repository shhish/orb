/* Deterministic deadlines and real LVGL timer admission. Host wall-clock
 * throughput is deliberately irrelevant to this test. */
#include <stdio.h>
#include <stdlib.h>
#include "lvgl.h"
#include "orb_display_pacing.h"

static unsigned s_checks, s_flushes, s_inputs, s_gates;
static uint32_t s_now, s_allow_at;
static uint8_t s_pixels[32 * 32 * 2];
#define CHECK(c) do { s_checks++; if (!(c)) { \
    fprintf(stderr, "pacing:%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint32_t tick(void) { return s_now; }
static void input_tick(lv_timer_t *t) { LV_UNUSED(t); s_inputs++; }
static uint32_t gate(lv_display_t *d)
{
    LV_UNUSED(d);
    s_gates++;
    return s_now < s_allow_at ? s_allow_at - s_now : 0;
}
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    LV_UNUSED(a); LV_UNUSED(px);
    s_flushes++;
    lv_display_flush_ready(d);
}
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) { s_now++; lv_timer_handler(); }
}

static void deadlines(bool moving, uint32_t initial_work)
{
    orb_pacing_t p = {0};
    int64_t now = 1000000, previous = 0;
    const uint32_t period = 16800;
    orb_pacing_plan_t r = orb_pacing_plan(&p, now, now, period, moving);
    CHECK(!r.deadline_us && p.boundaries == 1 && p.unpaced == 1 && r.mode == (moving ? 2 : 1));
    uint8_t divisor = (uint8_t)((initial_work + 500 + period - 1) / period);
    orb_pacing_complete(&p, 0, now, now + initial_work, 0, r.mode, true, 0);
    CHECK(!p.probing && p.first_slot);
    now += initial_work + 100;
    for (unsigned i = 0; i < 100; i++) {
        int64_t fall = 1000000 + ((now - 1000000) / period) * period;
        r = orb_pacing_plan(&p, now, fall, period, moving);
        if (r.delay_us) {
            now += r.delay_us;
            fall = 1000000 + ((now - 1000000) / period) * period;
            r = orb_pacing_plan(&p, now, fall, period, moving);
        }
        CHECK(r.deadline_us > now && r.divisor == divisor);
        if (previous) CHECK(r.deadline_us - previous == period * r.divisor);
        previous = r.deadline_us;
        orb_pacing_complete(&p, r.deadline_us, now, r.deadline_us, 500,
                            r.mode, true, 0);
        now = r.deadline_us + 100;
    }
    CHECK(p.scheduled == 100 && p.on_slot == 100 && !p.late && !p.skipped);
    uint64_t scheduled = p.scheduled;
    now += 120000;
    int64_t fall = 1000000 + ((now - 1000000) / period) * period;
    r = orb_pacing_plan(&p, now, fall, period, moving);
    CHECK(!r.delay_us && r.deadline_us && p.skipped > 0 && p.scheduled == scheduled + 1);
    orb_pacing_complete(&p, r.deadline_us, now, r.deadline_us + 2000, 0, r.mode, false, 0);
    CHECK(p.late == 1 && p.on_slot == 100 && p.max_error_us == 2000);
    r = orb_pacing_plan(&p, now + 500000, now + 500000, period, moving);
    CHECK(!r.deadline_us && p.boundaries == 2); /* idle restarts feedback */
    r = orb_pacing_plan(&p, now + 600000, now, period, moving);
    CHECK(!r.deadline_us && !p.mode); /* asleep TE is not a fake success */
    r = orb_pacing_plan(&p, now + 600001, now + 600001, 100, moving);
    CHECK(!r.deadline_us && !p.mode); /* reject ISR glitch */
}

static void admission(void)
{
    /* A first TE edge at monotonic zero is a valid grid, not a missing target. */
    orb_pacing_t early = {0};
    orb_pacing_plan_t initial = orb_pacing_plan(&early, 100, 0, 16800, false);
    CHECK(initial.mode == 1 && early.probing);
    orb_pacing_complete(&early, 0, 100, 10100, 0, initial.mode, true, 0);
    orb_pacing_plan_t following = orb_pacing_plan(&early, 10200, 0, 16800, false);
    CHECK(!early.probing && early.boundaries == 1 && following.delay_us);

    orb_pacing_t p = { .budget_us = {22000, 30000} };
    orb_pacing_plan_t r = orb_pacing_plan(&p, 1000000, 1000000, 16800, false);
    CHECK(p.probing && r.mode == 1 && !r.deadline_us && !p.admitted_30);
    r = orb_pacing_plan(&p, 1000100, 1000000, 16800, false);
    CHECK(r.delay_us == 1000 && p.probing && !p.scheduled);
    orb_pacing_complete(&p, 0, 1000000, 1023000, 0, 0, true, 0);
    CHECK(p.probing && !p.admitted_30); /* forced/no-TE work cannot admit */
    orb_pacing_complete(&p, 0, 1000000, 1023000, 0, 1, true, 0);
    CHECK(!p.probing && p.divisor == 2 && p.admitted_30 == 1 && p.budget_us[0] == 23500);
    p.budget_us[0] = 10000;
    orb_pacing_plan(&p, 1023100, 1016800, 16800, false);
    CHECK(p.divisor == 2 && p.admitted_30 == 1); /* stable until a complete warm block */
    r = orb_pacing_plan(&p, 1500000, 1500000, 17000, false);
    CHECK(p.probing && p.period_us == 17000 && p.boundaries == 2);
    orb_pacing_complete(&p, 0, 1500000, 1510000, 0, r.mode, true, 0);
    CHECK(!p.probing && p.divisor == 1 && p.admitted_30 == 1);
    r = orb_pacing_plan(&p, 2000000, 2000000, 17000, false);
    orb_pacing_complete(&p, 0, 2000000, 2000100, 0, r.mode, false, 0);
    CHECK(!p.probing && !p.mode && !p.target_us); /* failed probes cannot stall rendering */
}

static void jitter(bool moving, uint32_t work)
{
    orb_pacing_t p = {0};
    const int64_t origin = 1000000;
    const uint32_t physical_period = 16800;
    const int32_t sample_jitter[] = {25, -17, 31, -39, 11, -11};
    const uint8_t divisor = (uint8_t)((work + 500 + physical_period - 1) / physical_period);
    int64_t now = origin, previous_cycle = 0;
    orb_pacing_plan_t first = orb_pacing_plan(&p, now, now, physical_period + sample_jitter[0], moving);
    orb_pacing_complete(&p, 0, now, now + work, 0, first.mode, true, 0);
    now += work + 100;
    for(unsigned frame = 0; frame < 500; frame++) {
        orb_pacing_plan_t plan = {0};
        for(unsigned attempt = 0; attempt < 8; attempt++) {
            uint64_t edge = (uint64_t)(now - origin) / physical_period;
            uint32_t measured = physical_period + sample_jitter[edge % 6];
            plan = orb_pacing_plan(&p, now, origin + edge * physical_period, measured, moving);
            CHECK(plan.divisor == 0 || plan.divisor == divisor);
            if(!plan.delay_us) break;
            now += plan.delay_us;
        }
        CHECK(plan.deadline_us > now && plan.divisor == divisor);
        int64_t cycle = (plan.deadline_us - origin + physical_period / 2) / physical_period;
        int64_t physical_deadline = origin + cycle * physical_period;
        int64_t error = plan.deadline_us - physical_deadline;
        CHECK(error >= -160 && error <= 160);
        if(previous_cycle) CHECK(cycle - previous_cycle == divisor);
        previous_cycle = cycle;
        CHECK(physical_deadline - now >= work);
        orb_pacing_complete(&p, plan.deadline_us, now, physical_deadline,
                            (uint32_t)(physical_deadline - now - work), plan.mode, true, 0);
        CHECK(p.divisor == divisor); /* jitter cannot move the stable work window across a divisor */
        now = physical_deadline + 100;
    }
    CHECK(p.scheduled == 500 && p.on_slot == 500 && !p.late && !p.skipped && p.boundaries == 1);
}

static orb_pacing_plan_t warm_frame(orb_pacing_t *p, int64_t *now, int64_t origin,
                                     uint32_t period, bool moving, uint32_t work)
{
    orb_pacing_plan_t plan = {0};
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        int64_t fall = origin + ((*now - origin) / period) * period;
        plan = orb_pacing_plan(p, *now, fall, period, moving);
        if (!plan.delay_us) break;
        *now += plan.delay_us;
    }
    CHECK(plan.deadline_us >= *now + work && plan.divisor);
    orb_pacing_complete(p, plan.deadline_us, *now, plan.deadline_us,
                        (uint32_t)(plan.deadline_us - *now - work), plan.mode, true, 0);
    *now = plan.deadline_us + 100;
    return plan;
}

static void cold_recovery(bool moving)
{
    orb_pacing_t p = {0};
    const int64_t origin = 1000000;
    const uint32_t period = 16800;
    int64_t now = origin;
    orb_pacing_plan_t plan = orb_pacing_plan(&p, now, now, period, moving);
    orb_pacing_complete(&p, 0, now, now + 100000, 0, plan.mode, true, 0);
    CHECK(p.divisor == 6 && p.work_count == 0);
    now += 100100;
    int64_t previous = 0;
    /* The cold probe is excluded. Eight actual warm presentations admit a
     * faster stable rate; one fast sample cannot create alternating rates. */
    for (unsigned i = 0; i < 8; i++) {
        plan = warm_frame(&p, &now, origin, period, moving, 10000);
        CHECK(plan.divisor == 6);
        if (previous) CHECK(plan.deadline_us - previous == 6 * period);
        previous = plan.deadline_us;
        CHECK(p.divisor == (i == 7 ? 1 : 6));
    }
    CHECK(p.boundaries == 2 && p.first_slot && p.work_count == 8);
    CHECK(p.budget_us[moving ? 1 : 0] == 10500);
    for (unsigned i = 0; i < 64; i++) {
        plan = warm_frame(&p, &now, origin, period, moving, 10000);
        CHECK(plan.divisor == 1 && p.divisor == 1);
        if (i) CHECK(plan.deadline_us - previous == period);
        previous = plan.deadline_us;
    }
    CHECK(p.boundaries == 2 && !p.late && !p.skipped);
}

static void warm_interference(void)
{
    orb_pacing_t p = {0};
    const uint32_t period = 16800;
    orb_pacing_plan_t plan = orb_pacing_plan(&p, 1000000, 1000000, period, true);
    orb_pacing_complete(&p, 0, 1000000, 1010000, 0, plan.mode, true, 0);
    CHECK(p.divisor == 1 && !p.work_count);
    for (unsigned i = 0; i < 32; i++) {
        uint32_t work = i == 5 ? 40000 : 10000;
        int64_t started = 1100000 + (int64_t)i * 60000;
        orb_pacing_complete(&p, started + 10000, started, started + work, 0, 2, true, 0);
        CHECK(p.divisor == 1); /* one isolated slow observation never halves motion */
    }
    CHECK(p.boundaries == 1 && p.budget_us[1] == 10500 && p.late == 1);
    for (unsigned i = 0; i < 16; i++) {
        uint32_t work = i < 2 ? 40000 : 10000;
        int64_t started = 3100000 + (int64_t)i * 60000;
        orb_pacing_complete(&p, started + 10000, started, started + work, 0, 2, true, 0);
        CHECK(p.divisor == (i == 15 ? 3 : 1));
    }
    CHECK(p.boundaries == 2 && p.first_slot && p.budget_us[1] == 40500 && p.late == 3);
    for (unsigned i = 0; i < 16; i++) {
        int64_t started = 4100000 + (int64_t)i * 60000;
        orb_pacing_complete(&p, started + 10000, started, started + 10000, 0, 2, true, 0);
        CHECK(p.divisor == (i == 15 ? 1 : 3));
    }
    CHECK(p.boundaries == 3 && p.budget_us[1] == 10500);
}

static void admission_edges(void)
{
    const uint32_t work[] = {16300, 16301, 40000, 100000};
    const uint8_t divisor[] = {1, 2, 3, 6};
    for (unsigned i = 0; i < sizeof(work) / sizeof(work[0]); i++) {
        orb_pacing_t p = {0};
        orb_pacing_plan_t plan = orb_pacing_plan(&p, 1000000, 1000000, 16800, true);
        orb_pacing_complete(&p, 0, 1000000, 1000000 + work[i], 0, plan.mode, true, 0);
        CHECK(p.divisor == divisor[i]);
    }
    orb_pacing_t p = {0};
    orb_pacing_plan_t plan = orb_pacing_plan(&p, 1000000, 1000000, 16800, true);
    /* Deliberate waiting is not renderer work and must not force a 2T rate. */
    orb_pacing_complete(&p, 0, 1000000, 1060000, 48000, plan.mode, true, 0);
    CHECK(p.budget_us[1] == 12500 && p.divisor == 1);
}

static void busy_retry_does_not_starve(void)
{
    orb_pacing_t p = {0};
    const int64_t origin = 1000000;
    const uint32_t period = 16800, work = 4000;
    int64_t now = origin;
    orb_pacing_plan_t plan = orb_pacing_plan(&p, now, now, period, true);
    orb_pacing_complete(&p, 0, now, now + work, 0, plan.mode, true, 0);
    now += work + 100;
    for (unsigned i = 0; i < 64; i++) {
        int64_t fall = origin + ((now - origin) / period) * period;
        plan = orb_pacing_plan(&p, now, fall, period, true);
        if (plan.delay_us) {
            /* A bounded input/model callback runs before the gate's retry.
             * Once its prior start was missed, the gate must admit work for
             * a later slot immediately instead of moving the start again. */
            now += plan.delay_us + 15000;
            fall = origin + ((now - origin) / period) * period;
            plan = orb_pacing_plan(&p, now, fall, period, true);
        }
        CHECK(!plan.delay_us && plan.deadline_us >= now + work);
        orb_pacing_complete(&p, plan.deadline_us, now, plan.deadline_us,
                            (uint32_t)(plan.deadline_us - now - work), plan.mode, true, 0);
        now = plan.deadline_us + 100;
    }
    CHECK(p.scheduled == 64 && p.on_slot == 64 && p.skipped > 0 && !p.late);
}

typedef struct {
    int64_t deadline, started, completed;
    uint8_t mode;
} pending_frame_t;

/* Producer readiness precedes the previous frame's final DMA in a pipeline.
 * Deliver completion observations at their actual virtual time, so a >T lead
 * cannot accidentally turn a sustainable 1T stream into a serial fixture. */
static void pipelined_capacity(bool moving, uint32_t lead, uint32_t capacity, uint8_t divisor)
{
    const int64_t origin = 1000000;
    const uint32_t period = 16800;
    pending_frame_t pending[128] = {0};
    unsigned queued = 0, drained = 0;
    orb_pacing_t p = {0};
    int64_t now = origin;
    orb_pacing_plan_t plan = orb_pacing_plan(&p, now, now, period, moving);
    orb_pacing_complete(&p, 0, now, now + lead, 0, plan.mode, true, capacity);
    CHECK(p.divisor == divisor && p.budget_us[moving ? 1 : 0] == lead + 500);
    CHECK(p.capacity_budget_us[moving ? 1 : 0] == capacity + 500);
    now += lead + 100;
    int64_t previous = 0;
    unsigned overlapped = 0;
    for (unsigned frame = 0; frame < 128; frame++) {
        for (unsigned attempt = 0; attempt < 8; attempt++) {
            while (drained < queued && pending[drained].completed <= now) {
                const pending_frame_t *f = &pending[drained++];
                orb_pacing_complete(&p, f->deadline, f->started, f->completed, 0,
                                    f->mode, true, capacity);
            }
            int64_t fall = origin + ((now - origin) / period) * period;
            plan = orb_pacing_plan(&p, now, fall, period, moving);
            if (!plan.delay_us) break;
            now += plan.delay_us;
        }
        CHECK(plan.deadline_us && plan.divisor == divisor && p.divisor == divisor);
        CHECK(plan.deadline_us - now == lead + 500);
        if (previous) CHECK(plan.deadline_us - previous == period * divisor);
        if (queued && pending[queued - 1].completed > now) overlapped++;
        pending[queued++] = (pending_frame_t){plan.deadline_us, now, now + lead, plan.mode};
        previous = plan.deadline_us;
        now += capacity;
    }
    while (drained < queued) {
        const pending_frame_t *f = &pending[drained++];
        orb_pacing_complete(&p, f->deadline, f->started, f->completed, 0, f->mode, true, capacity);
    }
    CHECK(p.scheduled == 128 && p.on_slot == 128 && !p.late && !p.skipped && p.boundaries == 1);
    CHECK(p.divisor == divisor && p.budget_us[moving ? 1 : 0] == lead + 500);
    CHECK(p.capacity_budget_us[moving ? 1 : 0] == capacity + 500);
    CHECK(divisor != 1 || overlapped == 127);
}

static void independent_capacity_window(void)
{
    const uint32_t period = 16800;
    orb_pacing_t p = {0};
    orb_pacing_plan_t plan = orb_pacing_plan(&p, 1000000, 1000000, period, true);
    orb_pacing_complete(&p, 0, 1000000, 1018000, 0, plan.mode, true, 12000);
    CHECK(p.divisor == 1 && p.budget_us[1] == 18500 && p.capacity_budget_us[1] == 12500);
    /* Sustained latency above two panel periods changes the required lead,
     * while independently measured producer/sender service still admits 1T. */
    for (unsigned i = 0; i < 32; i++) {
        int64_t start = 1100000 + (int64_t)i * 60000;
        orb_pacing_complete(&p, start + 40000, start, start + 40000, 0, 2, true,
                            i == 5 ? 40000 : 12000);
        CHECK(p.divisor == 1);
    }
    CHECK(p.budget_us[1] == 40500 && p.capacity_budget_us[1] == 12500 && p.boundaries == 1 && !p.late);
    /* Two slow capacity observations select 3T only at the full block; the
     * lead retains its own independently measured 40.5 ms window. */
    for (unsigned i = 0; i < 16; i++) {
        int64_t start = 3100000 + (int64_t)i * 60000;
        orb_pacing_complete(&p, start + 40000, start, start + 40000, 0, 2, true,
                            i < 2 ? 40000 : 12000);
        CHECK(p.divisor == (i == 15 ? 3 : 1));
    }
    CHECK(p.budget_us[1] == 40500 && p.capacity_budget_us[1] == 40500 && p.boundaries == 2 && p.first_slot);
    for (unsigned i = 0; i < 16; i++) {
        int64_t start = 4100000 + (int64_t)i * 60000;
        orb_pacing_complete(&p, start + 18000, start, start + 18000, 0, 2, true, 12000);
        CHECK(p.divisor == (i == 15 ? 1 : 3));
    }
    CHECK(p.budget_us[1] == 18500 && p.capacity_budget_us[1] == 12500 && p.boundaries == 3);
    /* Clamp malformed/huge capacity through 64-bit arithmetic before adding
     * the common 500 us reserve; do not wrap it into an optimistic 1T estimate. */
    orb_pacing_t huge = {0};
    plan = orb_pacing_plan(&huge, 1000000, 1000000, period, false);
    orb_pacing_complete(&huge, 0, 1000000, 1100000, 0, plan.mode, true, UINT32_MAX);
    CHECK(huge.divisor == 6 && huge.budget_us[0] == 100000 && huge.capacity_budget_us[0] == 100000);
}

static void lvgl_gate(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(32, 32);
    lv_display_set_buffers(d, s_pixels, NULL, sizeof(s_pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(d, flush);
    lv_display_set_refresh_gate_cb(d, gate);
    lv_timer_t *input = lv_timer_create(input_tick, 2, NULL);
    s_allow_at = 100;
    advance(50);
    CHECK(s_gates > 0 && !s_flushes && s_inputs >= 24);
    advance(55);
    CHECK(s_flushes == 1 && s_inputs >= 50);
    unsigned gates = s_gates;
    advance(1000);
    CHECK(s_gates == gates && s_flushes == 1); /* no idle TE loop */
    s_allow_at = s_now + 100;
    lv_obj_invalidate(lv_display_get_screen_active(d));
    advance(5);
    CHECK(s_flushes == 1);
    lv_refr_now(d);
    CHECK(s_flushes == 2); /* synchronous captures/drain bypass gate */
    lv_obj_invalidate(lv_display_get_screen_active(d));
    lv_display_delete(d);
    gates = s_gates;
    advance(200);
    CHECK(s_gates == gates); /* deferred display deletion is safe */
    lv_timer_delete(input);
    lv_deinit();
}

int main(void)
{
    deadlines(false, 10000); deadlines(true, 12000); deadlines(true, 28000);
    deadlines(true, 40000); admission(); admission_edges();
    jitter(false, 10000); jitter(true, 12000); jitter(true, 28000);
    cold_recovery(false); cold_recovery(true); warm_interference(); busy_retry_does_not_starve(); lvgl_gate();
    pipelined_capacity(false, 18000, 12000, 1); pipelined_capacity(true, 18000, 12000, 1);
    pipelined_capacity(false, 28000, 20000, 2); pipelined_capacity(true, 28000, 20000, 2);
    independent_capacity_window();
    printf("pacing: %u TE-grid/admission/miss/input/idle/lifetime checks passed\n", s_checks);
    return 0;
}
