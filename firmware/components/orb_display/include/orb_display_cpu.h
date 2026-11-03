#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Idle tasks are pinned, so their runtime deltas measure each core without
 * assigning migrating draw/network tasks to an affinity. Runtime and idle use
 * the same FreeRTOS clock; its frequency need not equal esp_timer's. */
#define ORB_DISPLAY_CPU_CORES 2
typedef struct {
    int64_t at_us;
    uint64_t runtime, idle[ORB_DISPLAY_CPU_CORES];
    uint8_t counter_bits, cores;
    bool valid;
} orb_display_cpu_snapshot_t;

void orb_display_cpu_snapshot(orb_display_cpu_snapshot_t *out);

/* FreeRTOS updates task runtime at switches. Short windows can carry one idle
 * tick across a boundary; clamp this bounded sampling skew, never wrap it into
 * a fictitious busy core. Use at least 500 ms for useful attribution. */
static inline bool orb_display_cpu_busy(const orb_display_cpu_snapshot_t *before,
                                        const orb_display_cpu_snapshot_t *after,
                                        float busy[ORB_DISPLAY_CPU_CORES])
{
    for (unsigned core = 0; core < ORB_DISPLAY_CPU_CORES; core++) busy[core] = 0;
    if (!before->valid || !after->valid || before->cores != after->cores ||
        !after->cores || after->cores > ORB_DISPLAY_CPU_CORES ||
        before->counter_bits != after->counter_bits ||
        (after->counter_bits != 32 && after->counter_bits != 64) ||
        after->at_us <= before->at_us) return false;
    uint64_t mask = after->counter_bits == 32 ? UINT32_MAX : UINT64_MAX;
    uint64_t elapsed = (after->runtime - before->runtime) & mask;
    if (!elapsed) return false;
    for (unsigned core = 0; core < after->cores; core++) {
        uint64_t idle = (after->idle[core] - before->idle[core]) & mask;
        if (idle > elapsed) idle = elapsed;
        busy[core] = 100.0 * (double)(elapsed - idle) / elapsed;
    }
    return true;
}
