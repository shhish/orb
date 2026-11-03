#include "check.h"
#include "esp_wifi.h"
#include "orb_net.h"
#include "orb_net_priv.h"
#include "orb_net_util.h"

static int s_wakes, s_notifies, s_applies;
static esp_err_t s_error;
static wifi_ps_type_t s_mode;

void orb_net_power_wake(void)
{
    s_wakes++;
}

esp_err_t esp_wifi_set_ps(wifi_ps_type_t mode)
{
    s_applies++;
    s_mode = mode;
    return s_error;
}

static void on_hint(void *ctx)
{
    CHECK(ctx == &s_notifies);
    s_notifies++;
    /* The callback can safely read the authoritative effective state. */
    CHECK(orb_net_get_power_hint() == ORB_NET_POWER_INTERACTIVE ||
          orb_net_get_power_hint() == ORB_NET_POWER_BACKGROUND);
}

int main(void)
{
    orb_net_on_power_hint(on_hint, &s_notifies);
    CHECK(orb_net_get_power_hint() == ORB_NET_POWER_INTERACTIVE);
    orb_net_power_apply();
    CHECK(s_applies == 1 && s_mode == WIFI_PS_MIN_MODEM);
    orb_net_power_apply();
    CHECK(s_applies == 1);
    orb_net_set_power_hint(ORB_NET_POWER_BACKGROUND);
    CHECK(s_wakes == 1 && s_notifies == 1);
    CHECK(s_applies == 1);   /* the setter does not call Wi-Fi on its caller */
    orb_net_power_apply();
    CHECK(s_applies == 2 && s_mode == WIFI_PS_MAX_MODEM);
    orb_net_set_power_hint(ORB_NET_POWER_BACKGROUND);
    CHECK(s_notifies == 1);

    orb_net_power_provisioning(true);
    CHECK(orb_net_get_power_hint() == ORB_NET_POWER_INTERACTIVE);
    orb_net_power_apply();
    CHECK(s_mode == WIFI_PS_MIN_MODEM);
    orb_net_set_work_active(ORB_NET_WORK_OTA, true);
    orb_net_power_provisioning(false);
    CHECK(orb_net_get_power_hint() == ORB_NET_POWER_INTERACTIVE);
    orb_net_set_work_active(ORB_NET_WORK_VOICE, true);
    orb_net_set_work_active(ORB_NET_WORK_OTA, false);
    CHECK(orb_net_get_power_hint() == ORB_NET_POWER_INTERACTIVE);
    orb_net_set_work_active(ORB_NET_WORK_VOICE, false);
    CHECK(orb_net_get_power_hint() == ORB_NET_POWER_BACKGROUND);
    CHECK(s_notifies == 3);

    s_error = -1;
    orb_net_power_apply();
    int applies = s_applies;
    orb_net_power_apply();
    CHECK(s_applies == applies + 1);   /* a failed policy application is retried */
    s_error = ESP_OK;
    orb_net_power_apply();
    orb_net_set_power_hint(ORB_NET_POWER_INTERACTIVE);
    orb_net_power_apply();
    CHECK(s_mode == WIFI_PS_MIN_MODEM);
    CHECK(s_wakes == s_notifies);

    uint32_t generation = orb_net_interactive_generation();
    orb_net_set_power_hint(ORB_NET_POWER_BACKGROUND);
    orb_net_set_power_hint(ORB_NET_POWER_INTERACTIVE);
    orb_net_set_power_hint(ORB_NET_POWER_BACKGROUND);
    CHECK(orb_net_get_power_hint() == ORB_NET_POWER_BACKGROUND);
    CHECK(orb_net_interactive_generation() == generation + 1);
    orb_net_retry_t retry = { .due_ms = 300000 };
    if (generation != orb_net_interactive_generation()) {
        CHECK(orb_net_retry_prompt(&retry, 1000));
    }
    CHECK(retry.due_ms == 1000);   /* queued task still sees the intervening wake */
    generation = orb_net_interactive_generation();
    orb_net_set_power_hint(ORB_NET_POWER_INTERACTIVE);
    orb_net_set_power_hint(ORB_NET_POWER_BACKGROUND);
    CHECK(orb_net_interactive_generation() == generation + 1);
    retry.due_ms = 300000;
    CHECK(!orb_net_retry_prompt(&retry, 2000));
    CHECK(retry.due_ms == 300000);   /* repeated wakes retain the 30-second bound */

    orb_net_count_scan();
    orb_net_count_scan();
    orb_net_count_join();
    orb_net_count_join();
    orb_net_count_join();
    orb_net_counters_t counters;
    orb_net_get_counters(&counters);
    CHECK(counters.scans == 2 && counters.reconnects == 2);
    orb_net_get_counters(NULL);
    return check_report();
}
