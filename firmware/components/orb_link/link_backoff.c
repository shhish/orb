#include "link_backoff.h"

#define BASE_MS 1000u
#define CAP_MS 30000u
#define BACKGROUND_CAP_MS 300000u
#define PROMPT_MS 30000u

uint32_t link_backoff_ms(unsigned attempt, uint32_t rnd)
{
    /* 1, 2, 4, 8, 16 s; the next doubling (32 s) is past the cap. */
    uint32_t base = attempt < 5 ? BASE_MS << attempt : CAP_MS;
    /* Jitter spreads devices out after a hub restart, so they don't all knock at once. */
    return base + rnd % (base / 5 + 1);
}

uint32_t link_backoff_power_ms(unsigned attempt, uint32_t rnd, bool background)
{
    if (!background) return link_backoff_ms(attempt, rnd);
    uint32_t base = attempt < 9 ? BASE_MS << attempt : BACKGROUND_CAP_MS;
    uint32_t delay = base + rnd % (base / 5 + 1);
    return delay < BACKGROUND_CAP_MS ? delay : BACKGROUND_CAP_MS;
}

void link_retry_hint(link_retry_t *retry, bool background, uint64_t now_ms)
{
    bool waking = retry->background && !background;
    retry->background = background;
    if (waking && retry->waiting &&
        (!retry->have_prompt || now_ms - retry->last_prompt_ms >= PROMPT_MS)) {
        retry->prompt = true;
        retry->have_prompt = true;
        retry->last_prompt_ms = now_ms;
    }
}

void link_retry_wait(link_retry_t *retry, bool waiting)
{
    retry->waiting = waiting;
    if (!waiting) retry->prompt = false;
}

bool link_retry_take_prompt(link_retry_t *retry)
{
    bool prompt = retry->prompt;
    retry->prompt = false;
    return prompt;
}
