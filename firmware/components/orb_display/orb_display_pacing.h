#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A completion deadline belongs to a TE grid, never to the end of the previous
 * draw. Late work skips slots instead of building a queue of old presentations.
 * Caller serializes the UI planner and the sender's completion observations. */
typedef struct {
    int64_t target_us, started_us, last_request_us;
    uint32_t period_us, budget_us[2], capacity_budget_us[2];
    uint64_t scheduled, on_slot, late, skipped, boundaries, unpaced, admitted_30;
    uint32_t max_error_us;
    uint32_t work_us[16], capacity_us[16];
    uint8_t work_count, work_next;
    uint8_t mode, divisor;
    bool probing, first_slot;
} orb_pacing_t;

typedef struct {
    uint32_t delay_us;
    int64_t deadline_us;
    uint8_t mode, divisor;
} orb_pacing_plan_t;

static inline orb_pacing_plan_t orb_pacing_plan(orb_pacing_t *p, int64_t now,
                                                int64_t fall, uint32_t period,
                                                bool moving)
{
    orb_pacing_plan_t plan = {0};
    /* Reject startup, sleeping-panel and corrupt/glitch periods. Rendering
     * remains available without TE; those frames cannot claim paced success. */
    if (period < 8000 || period > 40000 || fall > now || now - fall > 4 * period) {
        p->target_us = 0;
        p->mode = 0;
        p->probing = false;
        p->unpaced++;
        return plan;
    }
    uint8_t mode = moving ? 2 : 1;
    if(p->probing) {
        /* Input/animation timers keep advancing while the immediate feedback
         * frame supplies a real whole-update work sample. */
        p->last_request_us = now;
        plan.delay_us = 1000;
        return plan;
    }
    if (p->mode != mode || now - p->last_request_us > 250000) {
        p->mode = mode;
        p->period_us = period;
        p->divisor = 1;
        p->work_count = p->work_next = 0;
        if (!p->budget_us[mode - 1]) p->budget_us[mode - 1] = moving ? 30000 : 12000;
        p->target_us = fall;
        p->probing = p->first_slot = true;
        p->boundaries++;
        p->last_request_us = now;
        /* The first changed frame is feedback, not a queued animation sample.
         * Present it immediately (including cold navigation), then join the TE
         * grid. Report it explicitly so it cannot inflate cadence success. */
        p->unpaced++;
        plan.mode = mode;
        return plan;
    }
    p->last_request_us = now;
    uint32_t budget = p->budget_us[mode - 1];
    int64_t step = (int64_t)period * p->divisor;
    if(p->first_slot) {
        /* Probe completion may have changed the admitted rate/budget. Start
         * its first paced slot on the current grid, not an expired probe slot. */
        p->target_us = fall;
        while(p->target_us < now + budget) p->target_us += step;
        p->first_slot = false;
    }
    /* A single ISR interval contains clock/interrupt jitter. Repeating that
     * one sample for an entire animation would drift away from the actual TE
     * edges. Reanchor the pending logical slot to the newest physical edge,
     * without changing its integer-divisor admission. Limit corrections to less than a
     * quarter-cycle so a glitch cannot silently change slot/parity. */
    int64_t delta = p->target_us - fall;
    int64_t cycles = delta >= 0 ? (delta + period / 2) / period :
                                 -((-delta + period / 2) / period);
    int64_t anchored = fall + cycles * period;
    int64_t correction = anchored - p->target_us;
    if(correction > -(int64_t)period / 4 && correction < (int64_t)period / 4) {
        p->target_us = anchored;
    }
    /* A producer slower than its slot remains visible as missed slots. Capping
     * the prediction avoids an unbounded feedback delay after a radio/OTA stall. */
    bool missed = false;
    while (now + budget > p->target_us + 1000) {
        p->target_us += step;
        p->skipped++;
        missed = true;
    }
    int64_t start = p->target_us - budget;
    if (now < start && !missed) {
        plan.delay_us = (uint32_t)(start - now);
        return plan;
    }
    /* A busy input/model timer may have consumed the start window. Render
     * immediately for the next feasible slot, rather than deferring again:
     * otherwise the same timer can repeatedly miss every new start and starve
     * drawing while invalidations accumulate into full-screen damage. */
    plan.deadline_us = p->target_us;
    plan.mode = mode;
    plan.divisor = p->divisor;
    p->period_us = period;
    p->target_us += step;
    p->started_us = now;
    p->scheduled++;
    return plan;
}

static inline void orb_pacing_complete(orb_pacing_t *p, int64_t deadline,
                                       int64_t started, int64_t completed,
                                       uint32_t intentional_wait, uint8_t mode, bool ok,
                                       uint32_t capacity_us)
{
    if (!mode) return;
    if(deadline) {
        uint64_t error = completed >= deadline ? (uint64_t)(completed - deadline) :
                                                (uint64_t)(deadline - completed);
        if (ok && error <= 1000) p->on_slot++;
        else p->late++;
        if (error > p->max_error_us) p->max_error_us = error > UINT32_MAX ? UINT32_MAX : (uint32_t)error;
    }
    if (!ok || completed < started + intentional_wait) {
        if(!deadline && p->probing && p->mode == mode) {
            p->probing = false;
            p->mode = 0;
            p->target_us = 0;
        }
        return;
    }
    uint64_t work = (uint64_t)(completed - started - intentional_wait) + 500;
    if (work > 100000) work = 100000;
    /* A double-buffered frame can take longer than T to reach the panel while
     * the producer and sender each sustain T. Preserve its measured latency
     * as the start lead, but admit the divisor from their maximum service time.
     * Zero retains the serial-latency fallback for callers without that sample.
     * The caller excludes intentional holds before supplying capacity_us. */
    uint64_t capacity = capacity_us ? (uint64_t)capacity_us + 500 : work;
    if (capacity > 100000) capacity = 100000;
    uint32_t *budget = &p->budget_us[mode - 1];
    bool probe = !deadline && p->probing && p->mode == mode;
    if (p->mode != mode) return;
    /* Keep a bounded warm work window. A cold font/cache fill must not lock a
     * game to 30 Hz for its entire session. HIG §6 applies equally to scrolling
     * and games: both may use every TE, and slow content gets a stable integer
     * divisor rather than a free-running 16/33 ms beat. The cold probe is not
     * a warm sample. One isolated radio/scan-wait outlier remains a counted
     * miss without halving otherwise sustainable motion for another window. */
    if (probe) {
        *budget = (uint32_t)work;
        p->capacity_budget_us[mode - 1] = (uint32_t)capacity;
        uint32_t divisor = ((uint32_t)capacity + p->period_us - 1) / p->period_us;
        if (divisor < 1) divisor = 1;
        if (divisor > 8) divisor = 8;
        p->divisor = (uint8_t)divisor;
        if (divisor == 2) p->admitted_30++;
        p->work_count = p->work_next = 0;
        p->probing = false;
        return;
    }
    p->work_us[p->work_next] = (uint32_t)work;
    p->capacity_us[p->work_next] = (uint32_t)capacity;
    p->work_next = (p->work_next + 1) % 16;
    if (p->work_count < 16) p->work_count++;
    uint32_t maximum = 0, second = 0, capacity_maximum = 0, capacity_second = 0;
    for (unsigned i = 0; i < p->work_count; i++) {
        if (p->work_us[i] >= maximum) {
            second = maximum;
            maximum = p->work_us[i];
        } else if (p->work_us[i] > second) second = p->work_us[i];
        if (p->capacity_us[i] >= capacity_maximum) {
            capacity_second = capacity_maximum;
            capacity_maximum = p->capacity_us[i];
        } else if (p->capacity_us[i] > capacity_second) capacity_second = p->capacity_us[i];
    }
    *budget = p->work_count >= 8 ? second : maximum;
    uint32_t capacity_budget = p->work_count >= 8 ? capacity_second : capacity_maximum;
    p->capacity_budget_us[mode - 1] = capacity_budget;
    /* Recover once after eight warm presentations, then reconsider only at
     * complete 16-sample boundaries. Two slow samples still determine the
     * sustainable divisor; per-frame timing noise never flips the rate. */
    if ((p->work_count == 8 && p->work_next == 8) || p->work_next == 0) {
        uint32_t divisor = (capacity_budget + p->period_us - 1) / p->period_us;
        if (divisor < 1) divisor = 1;
        if (divisor > 8) divisor = 8;
        if (divisor != p->divisor) {
            p->first_slot = true;
            p->boundaries++;
        }
        p->divisor = (uint8_t)divisor;
        if (divisor == 2) p->admitted_30++;
    }
}
