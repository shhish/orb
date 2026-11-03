/*
 * mDNS: finds hubs by browsing _orb-hub._tcp (docs/protocol.md, "Discovery").
 * Initialised only for the duration of a query and freed immediately after so
 * the 100 ms mDNS timer does not keep the CPU out of automatic light sleep.
 */
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "orb_net.h"
#include "orb_net_priv.h"
#include "orb_net_util.h"

static const char *TAG = "net";

#define HUB_SERVICE "_orb-hub"
#define HUB_PROTO "_tcp"
#define A_QUERY_MS 1000   /* for an answer that came without the hub's address */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_buf;

static SemaphoreHandle_t get_mutex(void)
{
    portENTER_CRITICAL(&s_lock);
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    }
    portEXIT_CRITICAL(&s_lock);
    return s_mutex;
}

static bool hub_ipv4(const mdns_result_t *r, esp_ip4_addr_t *out)
{
    for (const mdns_ip_addr_t *a = r->addr; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            *out = a->addr.u_addr.ip4;
            return true;
        }
    }
    return mdns_query_a(r->hostname, A_QUERY_MS, out) == ESP_OK;
}

static bool seen(const orb_hub_addr_t *hubs, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(hubs[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t orb_net_find_hubs(orb_hub_addr_t *out, int max, int *count, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(out && count && max > 0 && timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "args");
    *count = 0;
    ESP_RETURN_ON_FALSE(orb_net_is_connected(), ESP_ERR_INVALID_STATE, TAG, "not connected");

    SemaphoreHandle_t mutex = get_mutex();
    ESP_RETURN_ON_FALSE(mutex, ESP_ERR_NO_MEM, TAG, "mutex");
    xSemaphoreTake(mutex, portMAX_DELAY);

    if (!orb_net_is_connected()) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        xSemaphoreGive(mutex);
        ESP_LOGE(TAG, "mdns init: %s", esp_err_to_name(err));
        return err;
    }

    /* A hub usually answers once over IPv4 and once over IPv6; room for both. */
    mdns_result_t *results = NULL;
    err = mdns_query_ptr(HUB_SERVICE, HUB_PROTO, timeout_ms, max * 2, &results);
    if (err == ESP_OK) {
        for (const mdns_result_t *r = results; r && *count < max; r = r->next) {
            if (!r->instance_name || !r->hostname || !r->port || seen(out, *count, r->instance_name)) {
                continue;
            }
            orb_hub_txt_t txt = { 0 };
            for (size_t i = 0; i < r->txt_count; i++) {
                const char *v = r->txt[i].value;
                orb_hub_txt_add(&txt, r->txt[i].key, v, r->txt_value_len ? r->txt_value_len[i] : (v ? strlen(v) : 0));
            }
            if (!orb_hub_txt_usable(&txt)) {
                ESP_LOGW(TAG, "hub \"%s\": TXT not v=1 with pair and fp, skipped", r->instance_name);
                continue;
            }
            esp_ip4_addr_t ip;
            if (!hub_ipv4(r, &ip)) {
                ESP_LOGW(TAG, "hub \"%s\": no IPv4 address for %s.local", r->instance_name, r->hostname);
                continue;
            }
            orb_hub_addr_t *h = &out[(*count)++];
            memset(h, 0, sizeof(*h));
            snprintf(h->name, sizeof(h->name), "%s", r->instance_name);
            snprintf(h->host, sizeof(h->host), "%s.local", r->hostname);
            esp_ip4addr_ntoa(&ip, h->ip, sizeof(h->ip));
            h->port = r->port;
            h->pair_port = txt.pair_port;
            memcpy(h->fp, txt.fp, sizeof(h->fp));
        }
        if (results) {
            mdns_query_results_free(results);
        }
    } else {
        ESP_LOGW(TAG, "query: %s", esp_err_to_name(err));
    }

    mdns_free();
    xSemaphoreGive(mutex);
    if (err != ESP_OK) {
        return err;
    }
    return *count ? ESP_OK : ESP_ERR_NOT_FOUND;
}
