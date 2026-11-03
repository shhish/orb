#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "orb_display_cpu.h"

int main(void)
{
    float busy[2];
    orb_display_cpu_snapshot_t before = {
        .at_us = 1000, .runtime = 1000, .idle = {900, 100},
        .counter_bits = 32, .cores = 2, .valid = true,
    };
    orb_display_cpu_snapshot_t after = before;
    after.at_us += 1000;
    after.runtime += 1000;
    after.idle[0] += 950;
    after.idle[1] += 50;
    assert(orb_display_cpu_busy(&before, &after, busy));
    assert(fabsf(busy[0] - 5) < .001f && fabsf(busy[1] - 95) < .001f);
    /* The two-core percentages each use one core's elapsed clock. They must
     * never be halved by a machine-wide runtime denominator. */
    after.idle[0] = before.idle[0];
    after.idle[1] = before.idle[1];
    assert(orb_display_cpu_busy(&before, &after, busy));
    assert(busy[0] == 100 && busy[1] == 100);
    for (unsigned bits = 32; bits <= 64; bits += 32) {
        uint64_t mask = bits == 32 ? UINT32_MAX : UINT64_MAX;
        before.counter_bits = after.counter_bits = bits;
        before.runtime = mask - 499;
        after.runtime = 500;
        before.idle[0] = before.idle[1] = mask - 199;
        after.idle[0] = 300;
        after.idle[1] = mask - 99;
        assert(orb_display_cpu_busy(&before, &after, busy));
        assert(busy[0] == 50 && busy[1] == 90);
    }
    after.idle[0] = before.idle[0] + 1001;
    assert(orb_display_cpu_busy(&before, &after, busy));
    assert(busy[0] == 0);
    after.valid = false;
    assert(!orb_display_cpu_busy(&before, &after, busy));
    after.valid = true;
    after.counter_bits = 16;
    assert(!orb_display_cpu_busy(&before, &after, busy));
    after.counter_bits = before.counter_bits;
    after.at_us = before.at_us;
    assert(!orb_display_cpu_busy(&before, &after, busy));
    after.at_us++;
    after.runtime = before.runtime;
    assert(!orb_display_cpu_busy(&before, &after, busy));
    after.runtime++;
    after.cores = 1;
    assert(!orb_display_cpu_busy(&before, &after, busy));
    before.cores = 1;
    assert(orb_display_cpu_busy(&before, &after, busy));
    assert(busy[1] == 0);
    puts("CPU intervals: per-core load, 32/64-bit wrap, sampling skew and unavailable windows passed");
}
