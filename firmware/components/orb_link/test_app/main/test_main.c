/*
 * Builds orb_link against the stub orb_net and touches every public call, so
 * a missing symbol or a header mismatch fails the build. On a board it runs
 * too, searching for a hub the stub never finds.
 */
#include <stdlib.h>

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "orb_link.h"

static const char *TAG = "link_test";

static void on_state(const orb_link_status_t *st, void *ctx)
{
    ESP_LOGI(TAG, "state %d, paired %d, code %s", st->state, st->paired, st->sas);
}

static void on_offer(const char *name, const cJSON *params, void *ctx)
{
    ESP_LOGI(TAG, "%s", name);
}

static esp_err_t on_identify(const cJSON *params, cJSON *result, void *ctx)
{
    return ESP_OK;
}

static void add_last_ota(cJSON *hello, void *ctx)
{
    cJSON *ota = cJSON_AddObjectToObject(hello, "last_ota");
    cJSON_AddStringToObject(ota, "version", "0.0.0");
    cJSON_AddStringToObject(ota, "result", "ok");
}

static esp_err_t read_power(orb_link_power_t *out, void *ctx)
{
    *out = (orb_link_power_t){ .battery_pct = -1, .charging = false, .usb = true };
    return ESP_OK;
}

static void on_check(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    ESP_LOGI(TAG, "ota.check: %s", esp_err_to_name(err));
}

void app_main(void)
{
    orb_link_memory_init();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(orb_link_on_event("ota.available", on_offer, NULL));
    ESP_ERROR_CHECK(orb_link_on_method("device.identify", on_identify, NULL));
    ESP_ERROR_CHECK(orb_link_on_hello(add_last_ota, NULL));
    ESP_ERROR_CHECK(orb_link_set_power_source(read_power, NULL));
    ESP_ERROR_CHECK(orb_link_start(on_state, NULL));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        orb_link_status_t st;
        orb_link_status(&st);
        orb_link_https_t https;
        bool paired = orb_link_hub_https(&https) == ESP_OK;
        ESP_LOGI(TAG, "online %d, state %d, hub https %s", orb_link_online(), st.state, paired ? https.base_url : "-");
        free(https.cert_pem);
        if (orb_link_online()) {
            orb_link_request("ota.check", NULL, 0, on_check, NULL);
            orb_link_notify("log.batch", cJSON_CreateObject());
        } else if (st.state == ORB_LINK_UNAUTHORIZED) {
            orb_link_unpair();
        }
    }
}
