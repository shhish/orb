#include "check.h"
#include "orb_net_util.h"

static void test_radio_policy(void)
{
    for (unsigned failures = 0; failures < 8; failures++) {
        CHECK(orb_net_retry_ms(failures, failures, false, 73) == orb_net_backoff_ms(failures, 73));
    }
    CHECK(orb_net_retry_ms(8, 7, false, 0) == 30000);
    CHECK(orb_net_retry_ms(8, 8, false, 0) == 128000);
    CHECK(orb_net_retry_ms(9, 9, false, 0) == 256000);
    CHECK(orb_net_retry_ms(10, 10, false, UINT32_MAX) == 300000);
    CHECK(orb_net_retry_ms(8, 0, true, 0) == 128000);
    CHECK(orb_net_retry_ms(UINT32_MAX, 0, true, UINT32_MAX) == 300000);
    CHECK(orb_net_retry_ms(0, 8, true, UINT32_MAX) == 0);
    CHECK(orb_net_retry_ms(1, 0, true, 200) == 1200);
    for (unsigned i = 1; i < 100; i++) {
        uint32_t d = orb_net_retry_ms(i, 0, true, UINT32_MAX);
        CHECK(d > 0 && d <= 300000);
    }

    orb_net_retry_t retry = { .due_ms = 300000 };
    CHECK(orb_net_retry_prompt(&retry, 0));
    CHECK(retry.due_ms == 1);   /* preserve zero as the no-join sentinel at boot */
    retry.due_ms = 300001;
    CHECK(!orb_net_retry_prompt(&retry, 29999));
    CHECK(retry.due_ms == 300001);
    CHECK(orb_net_retry_prompt(&retry, 30000));
    CHECK(retry.due_ms == 30000);
    retry.due_ms = 0;
    CHECK(!orb_net_retry_prompt(&retry, 60000));   /* no pending join */
    retry.due_ms = 60000;
    CHECK(!orb_net_retry_prompt(&retry, 60000));   /* already due */
    retry.due_ms = 60001;
    CHECK(orb_net_retry_prompt(&retry, 60000));

    CHECK(orb_net_power_background(true, false, 0));
    CHECK(!orb_net_power_background(false, false, 0));
    CHECK(!orb_net_power_background(true, true, 0));
    CHECK(!orb_net_power_background(true, false, 1));
    CHECK(!orb_net_power_background(true, false, 2));
    CHECK(!orb_net_power_background(true, false, 3));
}

int main(void)
{
    test_radio_policy();
    return check_report();
}
