#pragma once

/* Reconnect delays (docs/protocol.md, "Link"). ESP-free for the host tests. */

#include <stdint.h>
#include <stdbool.h>

/*
 * The wait before reconnect attempt `attempt` (0 for the first retry):
 * 1, 2, 4 ... 30 s, plus up to 20 % jitter taken from `rnd`, any uniformly
 * random 32-bit value. In milliseconds.
 */
uint32_t link_backoff_ms(unsigned attempt, uint32_t rnd);

/* Background waits include jitter inside the five-minute ceiling. */
uint32_t link_backoff_power_ms(unsigned attempt, uint32_t rnd, bool background);

typedef struct {
    bool background;
    bool waiting;
    bool prompt;
    bool have_prompt;
    uint64_t last_prompt_ms;
} link_retry_t;

/* Serialized by the adapter. Only a wake during backoff consumes the budget. */
void link_retry_hint(link_retry_t *retry, bool background, uint64_t now_ms);
void link_retry_wait(link_retry_t *retry, bool waiting);
bool link_retry_take_prompt(link_retry_t *retry);
