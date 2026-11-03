/*
 * On-device smoke test of orb_ota against the orb_link stub (no hub, no
 * network): offers and install policies (the battery floor too), hello's
 * last_ota, and an install that fails cleanly. Flash it and look for "orb_ota test: N passed, 0 failed".
 *
 * The image must be signed (scripts/sign-app.sh) like the real firmware;
 * test_app/sign_check.sh does that with a throwaway key.
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "orb_link_stub.h"
#include "orb_ota.h"

static const char *TAG = "test";

#define SHA_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SHA_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static int s_pass, s_fail, s_changes;

#define CHECK(cond)                                              \
    do {                                                         \
        if (cond) {                                              \
            s_pass++;                                            \
        } else {                                                 \
            s_fail++;                                            \
            ESP_LOGE(TAG, "line %d: %s", __LINE__, #cond);       \
        }                                                        \
    } while (0)

static void on_status(const orb_ota_status_t *st, void *ctx)
{
    s_changes++;
    ESP_LOGI(TAG, "state %d, %d%%, \"%s\" %s", st->state, st->progress, st->version, st->error);
}

static orb_ota_status_t status(void)
{
    orb_ota_status_t st = { 0 };
    orb_ota_status(&st);
    return st;
}

static void offer(const char *version, const char *install, const char *sha)
{
    char json[400];
    snprintf(json, sizeof(json),
             "{\"available\":true,\"version\":\"%s\",\"build\":\"e1f2a3b\",\"size\":823456,\"sha256\":\"%s\","
             "\"url\":\"https://hub.local:8443/v1/firmware/%s.bin\",\"install\":\"%s\",\"notes\":\"Test\"}",
             version, sha, sha, install);
    orb_link_stub_event("ota.available", json);
}

/* The install task fails at once without a hub; then its report goes out. */
static void wait_for_failed(void)
{
    for (int i = 0; i < 100 && status().state != ORB_OTA_FAILED; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    CHECK(orb_ota_boot_check() == ESP_OK);
    CHECK(orb_ota_start(on_status, NULL) == ESP_OK);
    orb_ota_status_t st = status();
    CHECK(st.state == ORB_OTA_IDLE);
    ESP_LOGI(TAG, "%s, on probation: %d", esp_app_get_description()->version, st.pending_verify);

    /* hello carries last_ota only with something to report; when it does, it's well formed. */
    cJSON *hello = orb_link_stub_hello();
    const cJSON *last = cJSON_GetObjectItem(hello, "last_ota");
    CHECK(!last || (cJSON_IsString(cJSON_GetObjectItem(last, "version")) &&
                    cJSON_IsString(cJSON_GetObjectItem(last, "result"))));
    cJSON_Delete(hello);

    if (st.pending_verify) {
        /* After an OTA of this test image: confirm it, as the app would. */
        orb_ota_mark_healthy();
        CHECK(!status().pending_verify);
    }

    /* "ask" waits for a tap, even when idle. */
    offer("9.9.9", "ask", SHA_A);
    st = status();
    CHECK(st.state == ORB_OTA_OFFERED && strcmp(st.version, "9.9.9") == 0 && strcmp(st.notes, "Test") == 0);
    orb_ota_set_idle(true);
    CHECK(status().state == ORB_OTA_OFFERED);
    orb_ota_set_idle(false);

    /* A broken offer changes nothing. */
    orb_link_stub_event("ota.available", "{\"available\":true,\"version\":\"9.9.10\",\"sha256\":\"xyz\"}");
    CHECK(strcmp(status().version, "9.9.9") == 0);

    /* The running version is never installed. */
    offer(esp_app_get_description()->version, "now", SHA_B);
    CHECK(strcmp(status().version, "9.9.9") == 0);

    /* "idle" waits until the app says so. */
    offer("9.9.11", "idle", SHA_B);
    st = status();
    CHECK(st.state == ORB_OTA_OFFERED && strcmp(st.version, "9.9.11") == 0);

    /* Nothing available any more: the waiting offer goes. */
    orb_link_stub_event("ota.available", "{\"available\":false}");
    CHECK(status().state == ORB_OTA_IDLE);
    CHECK(orb_ota_install() == ESP_ERR_INVALID_STATE);

    /* A tap starts the install. Unpaired, it fails cleanly and tells the hub. */
    offer("9.9.12", "ask", SHA_A);
    CHECK(orb_ota_install() == ESP_OK);
    wait_for_failed();
    st = status();
    CHECK(st.state == ORB_OTA_FAILED && strcmp(st.error, "not paired") == 0);
    char sent[256];
    CHECK(orb_link_stub_sent(sent, sizeof(sent)) >= 2 && strstr(sent, "\"state\":\"failed\"") != NULL);

    /* A network-type failure isn't held against the image: a tap retries. */
    CHECK(orb_ota_install() == ESP_OK);
    wait_for_failed();

    /* On a low battery "now" waits until the device charges. */
    orb_ota_set_power(10, false);
    offer("9.9.13", "now", SHA_B);
    CHECK(status().state == ORB_OTA_OFFERED);
    orb_ota_set_power(25, true);   /* charging is enough */
    wait_for_failed();
    st = status();
    CHECK(st.state == ORB_OTA_FAILED && strcmp(st.version, "9.9.13") == 0);

    CHECK(s_changes > 0);
    ESP_LOGI(TAG, "orb_ota test: %d passed, %d failed", s_pass, s_fail);
}
