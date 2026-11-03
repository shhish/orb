/*
 * The device side of the end-to-end run: prints what the hub stub checks
 * ("E2E ..." lines on stdout) and answers what an app would.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "orb_link.h"
#include "psa/crypto.h"

/* What orb_ota would get: the address, the token and the pinned PEM (as its SHA-256). */
static void print_https(void)
{
    orb_link_https_t https;
    if (orb_link_hub_https(&https) != ESP_OK) {
        printf("E2E https none\n");
        return;
    }
    uint8_t sha[32];
    size_t n;
    psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)https.cert_pem, strlen(https.cert_pem), sha, sizeof(sha), &n);
    printf("E2E https url=%s auth=%s pem_sha256=", https.base_url, https.authorization);
    for (size_t i = 0; i < sizeof(sha); i++) {
        printf("%02x", sha[i]);
    }
    printf("\n");
    free(https.cert_pem);
}

static void on_state(const orb_link_status_t *st, void *ctx)
{
    printf("E2E state=%d paired=%d sas=%s hub=%s name=%s channel=%s\n", st->state, st->paired, st->sas,
           st->hub_name, st->device_name, st->channel);
    if (st->state == ORB_LINK_ONLINE) {
        print_https();
        orb_link_notify("log.batch", cJSON_CreateObject());
    }
    fflush(stdout);
}

static void on_test_event(const char *name, const cJSON *params, void *ctx)
{
    char *s = cJSON_PrintUnformatted(params);
    printf("E2E event=%s params=%s\n", name, s);
    fflush(stdout);
    cJSON_free(s);
}

static void on_check(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    char *s = cJSON_PrintUnformatted(err == ESP_OK ? result : error);
    printf("E2E reply=%s body=%s\n", esp_err_to_name(err), s ? s : "null");
    fflush(stdout);
    cJSON_free(s);
}

/* The hub asks for this one after the event, so the reply follows it. */
static esp_err_t on_identify(const cJSON *params, cJSON *result, void *ctx)
{
    orb_link_request("ota.check", NULL, 2000, on_check, NULL);
    orb_link_request("slow.method", NULL, 1000, on_check, NULL);
    return ESP_OK;
}

static esp_err_t on_config(const cJSON *params, cJSON *result, void *ctx)
{
    return cJSON_IsString(cJSON_GetObjectItem(params, "name")) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void add_last_ota(cJSON *hello, void *ctx)
{
    cJSON *ota = cJSON_AddObjectToObject(hello, "last_ota");
    cJSON_AddStringToObject(ota, "version", "0.0.1");
    cJSON_AddStringToObject(ota, "result", "ok");
}

static esp_err_t read_power(orb_link_power_t *out, void *ctx)
{
    *out = (orb_link_power_t){ .battery_pct = 87, .battery_mv = 4010,
                               .display_state = ORB_LINK_DISPLAY_ALWAYS_ON, .charging = true, .usb = true };
    return ESP_OK;
}

void app_main(void)
{
    /* Linux's capability allocator maps this to malloc; production requests
     * PSRAM with the same hooks before any callback can create a JSON tree. */
    orb_link_memory_init();
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());   /* as orb_net needs; esp_http_client posts to it */
    psa_crypto_init();   /* the target does this at boot; the linux port doesn't */
    ESP_ERROR_CHECK(orb_link_on_event("test.event", on_test_event, NULL));
    ESP_ERROR_CHECK(orb_link_on_method("device.identify", on_identify, NULL));
    ESP_ERROR_CHECK(orb_link_on_method("device.config", on_config, NULL));
    ESP_ERROR_CHECK(orb_link_on_hello(add_last_ota, NULL));
    ESP_ERROR_CHECK(orb_link_set_power_source(read_power, NULL));
    ESP_ERROR_CHECK(orb_link_start(on_state, NULL));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
