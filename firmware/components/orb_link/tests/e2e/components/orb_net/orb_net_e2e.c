/*
 * TEST ONLY: a connected network whose one hub is hub_stub.py, at the ports
 * and certificate fingerprint it passes in ORB_E2E_PORT, ORB_E2E_PAIR_PORT
 * and ORB_E2E_FP.
 */
#include "orb_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

esp_err_t orb_net_start(orb_net_cb_t cb, void *ctx)
{
    return ESP_OK;
}

esp_err_t orb_net_status(orb_net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = ORB_NET_CONNECTED;
    out->rssi = -50;
    return ESP_OK;
}

esp_err_t orb_net_forget(void)
{
    return ESP_OK;
}

const char *orb_net_device_id(void)
{
    return "orb-e2e000000000";
}

esp_err_t orb_net_find_hubs(orb_hub_addr_t *out, int max, int *count, int timeout_ms)
{
    const char *port = getenv("ORB_E2E_PORT"), *pair = getenv("ORB_E2E_PAIR_PORT"), *fp = getenv("ORB_E2E_FP");
    *count = 0;
    if (!port || !pair || max < 1) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "E2E hub");
    snprintf(out->host, sizeof(out->host), "orb-hub-e2e.local");
    snprintf(out->ip, sizeof(out->ip), "127.0.0.1");
    out->port = (uint16_t)atoi(port);
    out->pair_port = (uint16_t)atoi(pair);
    snprintf(out->fp, sizeof(out->fp), "%s", fp ? fp : "");
    *count = 1;
    return ESP_OK;
}

void orb_net_set_timezone(const char *iana)
{
    printf("E2E tz=%s\n", iana);
}
