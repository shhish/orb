#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct { unsigned observed; int64_t changed_at_us; } orb_sound_save_t;

/* UINT32_MAX is event-only idle, zero commits now. A new edit starts a fresh
 * 500 ms quiet interval, including one racing a previous flash commit. */
static inline uint32_t orb_sound_save_wait(orb_sound_save_t *save, unsigned generation,
                                          unsigned saved, int64_t now_us)
{
    if (generation != save->observed) {
        save->observed = generation;
        save->changed_at_us = now_us;
    }
    if (generation == saved) return UINT32_MAX;
    int64_t left = save->changed_at_us + 500000 - now_us;
    return left > 0 ? (uint32_t)((left + 999) / 1000) : 0;
}

/* Private handoff: request paths only update atomics and notify the main owner.
 * Storage and debounce state live outside the PSRAM audio translation unit. */
typedef struct { unsigned generation, completed; uint32_t packed; } orb_sound_save_snapshot_t;
void orb_sound_save_snapshot(orb_sound_save_snapshot_t *out);
void orb_sound_save_complete(unsigned generation, bool saved);
void orb_sound_save_restore(uint32_t packed);
void orb_sound_save_notify(void);
