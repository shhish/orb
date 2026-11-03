/*
 * orb_link without a hub: never online and unpaired, so requests fail with
 * ESP_ERR_INVALID_STATE and orb_link_hub_https has nothing to give. Events the
 * device sends are recorded for the test to look at.
 */
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "orb_link.h"
#include "orb_link_stub.h"

#define MAX_HANDLERS 16

static struct {
    const char *name;
    orb_link_event_cb_t cb;
    void *ctx;
} s_events[MAX_HANDLERS];
static int s_n_events;
static orb_link_hello_cb_t s_hello;
static void *s_hello_ctx;
static portMUX_TYPE s_sent_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_n_sent;
static char s_last_sent[256];

esp_err_t orb_link_start(orb_link_state_cb_t cb, void *ctx)
{
    return ESP_OK;
}

esp_err_t orb_link_status(orb_link_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = ORB_LINK_OFFLINE;
    return ESP_OK;
}

esp_err_t orb_link_on_event(const char *name, orb_link_event_cb_t cb, void *ctx)
{
    if (s_n_events == MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    s_events[s_n_events].name = name;
    s_events[s_n_events].cb = cb;
    s_events[s_n_events].ctx = ctx;
    s_n_events++;
    return ESP_OK;
}

esp_err_t orb_link_on_method(const char *method, orb_link_method_cb_t cb, void *ctx)
{
    return ESP_OK;
}

esp_err_t orb_link_on_hello(orb_link_hello_cb_t cb, void *ctx)
{
    s_hello = cb;
    s_hello_ctx = ctx;
    return ESP_OK;
}

esp_err_t orb_link_request(const char *method, cJSON *params, int timeout_ms, orb_link_reply_cb_t cb, void *ctx)
{
    cJSON_Delete(params);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t orb_link_notify(const char *event, cJSON *params)
{
    char *text = cJSON_PrintUnformatted(params);
    portENTER_CRITICAL(&s_sent_lock);
    s_n_sent++;
    strlcpy(s_last_sent, text ? text : "", sizeof(s_last_sent));
    portEXIT_CRITICAL(&s_sent_lock);
    free(text);
    cJSON_Delete(params);
    return ESP_OK;
}

bool orb_link_online(void)
{
    return false;
}

esp_err_t orb_link_unpair(void)
{
    return ESP_OK;
}

esp_err_t orb_link_hub_https(orb_link_https_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_ERR_INVALID_STATE;
}

void orb_link_stub_event(const char *name, const char *params_json)
{
    cJSON *params = cJSON_Parse(params_json);
    for (int i = 0; i < s_n_events; i++) {
        if (strcmp(s_events[i].name, name) == 0) {
            s_events[i].cb(name, params, s_events[i].ctx);
        }
    }
    cJSON_Delete(params);
}

cJSON *orb_link_stub_hello(void)
{
    cJSON *hello = cJSON_CreateObject();
    if (hello && s_hello) {
        s_hello(hello, s_hello_ctx);
    }
    return hello;
}

int orb_link_stub_sent(char *last, size_t len)
{
    portENTER_CRITICAL(&s_sent_lock);
    int n = s_n_sent;
    strlcpy(last, s_last_sent, len);
    portEXIT_CRITICAL(&s_sent_lock);
    return n;
}
