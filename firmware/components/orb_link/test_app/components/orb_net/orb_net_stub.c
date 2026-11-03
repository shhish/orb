/*
 * TEST ONLY: orb_net's API with no Wi-Fi behind it. It reports a connected
 * network and no hub, which keeps orb_link searching with backoff.
 */
#include "orb_net.h"

#include <string.h>

esp_err_t orb_net_start(orb_net_cb_t cb, void *ctx)
{
    return ESP_OK;
}

esp_err_t orb_net_status(orb_net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = ORB_NET_CONNECTED;
    return ESP_OK;
}

esp_err_t orb_net_forget(void)
{
    return ESP_OK;
}

const char *orb_net_device_id(void)
{
    return "orb-000000000000";
}

esp_err_t orb_net_find_hubs(orb_hub_addr_t *out, int max, int *count, int timeout_ms)
{
    *count = 0;
    return ESP_ERR_NOT_FOUND;
}

void orb_net_set_timezone(const char *iana)
{
}
