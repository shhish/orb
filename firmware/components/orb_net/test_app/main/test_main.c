/*
 * Builds orb_net into a whole app, for the compiler and for `idf.py size`. On a
 * board it also runs: it joins the stored network (or provisions over BLE),
 * looks for hubs and prints what it finds.
 */
#include <time.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "orb_net.h"

static const char *TAG = "net_test";

static const char *const s_states[] = { "unprovisioned", "provisioning", "connecting", "connected", "failed" };

static void on_net(const orb_net_status_t *st, void *ctx)
{
    ESP_LOGI(TAG, "%s ssid=\"%s\" ip=%s rssi=%d service=%s time=%d", s_states[st->state], st->ssid, st->ip, st->rssi,
             st->service_name, st->time_synced);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "%s, internal free %u KB", orb_net_device_id(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ESP_ERROR_CHECK(orb_net_start(on_net, NULL));
    orb_net_set_timezone("America/New_York");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        orb_net_status_t st;
        ESP_ERROR_CHECK(orb_net_status(&st));
        ESP_LOGI(TAG, "status: %s rssi=%d", s_states[st.state], st.rssi);
        orb_hub_addr_t hubs[4];
        int n = 0;
        if (orb_net_find_hubs(hubs, 4, &n, 3000) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                ESP_LOGI(TAG, "hub \"%s\" %s (%s) port %u pair %u fp %s", hubs[i].name, hubs[i].host, hubs[i].ip,
                         hubs[i].port, hubs[i].pair_port, hubs[i].fp);
            }
        }
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        ESP_LOGI(TAG, "%04d-%02d-%02d %02d:%02d:%02d %s, internal free %u KB (min %u KB)", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_isdst ? "DST" : "std",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    }
}
