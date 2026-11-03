#include <limits.h>
#include <stdint.h>

#include "link_backoff.h"
#include "test.h"

void test_backoff(void)
{
    /* 1, 2, 4 ... 30 s with no jitter. */
    static const uint32_t base[] = { 1000, 2000, 4000, 8000, 16000, 30000, 30000 };
    for (unsigned i = 0; i < sizeof(base) / sizeof(base[0]); i++) {
        CHECK(link_backoff_ms(i, 0) == base[i]);
    }
    CHECK(link_backoff_ms(31, 0) == 30000);
    CHECK(link_backoff_ms(32, 0) == 30000);
    CHECK(link_backoff_ms(UINT_MAX, 0) == 30000);

    /* Jitter: up to 20 % on top, never below the base, and it actually varies. */
    for (unsigned i = 0; i < 8; i++) {
        uint32_t b = link_backoff_ms(i, 0), lo = UINT32_MAX, hi = 0;
        uint32_t x = 2463534242u;
        for (int n = 0; n < 10000; n++) {
            x ^= x << 13;   /* xorshift32 */
            x ^= x >> 17;
            x ^= x << 5;
            uint32_t d = link_backoff_ms(i, x);
            lo = d < lo ? d : lo;
            hi = d > hi ? d : hi;
        }
        CHECK(lo >= b);
        CHECK(hi <= b + b / 5);
        CHECK(hi - lo > b / 10);
    }
    CHECK(link_backoff_ms(0, 200) == 1200);
    CHECK(link_backoff_ms(0, 201) == 1000);
    CHECK(link_backoff_ms(5, UINT32_MAX) <= 36000);

    CHECK(link_backoff_power_ms(5, 0, false) == 30000);
    CHECK(link_backoff_power_ms(5, 0, true) == 32000);
    CHECK(link_backoff_power_ms(8, 0, true) == 256000);
    CHECK(link_backoff_power_ms(9, 0, true) == 300000);
    CHECK(link_backoff_power_ms(UINT_MAX, UINT32_MAX, true) == 300000);
    CHECK(link_backoff_power_ms(8, UINT32_MAX, true) <= 300000);

    link_retry_t retry = {0};
    link_retry_hint(&retry, true, 0);
    link_retry_hint(&retry, false, 1); /* Not waiting: preserve first prompt. */
    CHECK(!link_retry_take_prompt(&retry) && !retry.have_prompt);
    link_retry_wait(&retry, true);
    link_retry_hint(&retry, true, 2);
    link_retry_hint(&retry, false, 3);
    CHECK(link_retry_take_prompt(&retry));
    CHECK(!link_retry_take_prompt(&retry));
    link_retry_hint(&retry, true, 4);
    link_retry_hint(&retry, false, 30002);
    CHECK(!link_retry_take_prompt(&retry));
    link_retry_hint(&retry, true, 30002);
    link_retry_hint(&retry, false, 30003);
    CHECK(link_retry_take_prompt(&retry));
    link_retry_hint(&retry, true, 60004);
    link_retry_hint(&retry, false, 60005);
    link_retry_wait(&retry, false); /* A completed wait cannot leak a prompt. */
    CHECK(!link_retry_take_prompt(&retry));
}
