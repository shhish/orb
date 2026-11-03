/*
 * Time: SNTP sets the clock once Wi-Fi is up (there is no RTC chip), and the
 * hub's IANA zone becomes a POSIX TZ through the table in orb_net_tz.c. The
 * zone is kept in NVS so the face shows local time from boot, before the hub
 * answers.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "orb_net.h"
#include "orb_net_priv.h"
#include "orb_net_util.h"

static const char *TAG = "net";

#define NTP_SERVER "pool.ntp.org"
#define NVS_TZ "tz"
#define NVS_TZ_POSIX "tzp"
#define IANA_MAX 64
#define POSIX_MAX 64

static bool s_sntp_started;

static void apply(const char *iana, const char *posix)
{
    if (!orb_net_posix_tz(iana) && !orb_net_posix_tz_valid(posix)) {
        ESP_LOGW(TAG, "time zone \"%s\" not in the table and no rule from the hub, using UTC", iana ? iana : "");
    }
    setenv("TZ", orb_net_resolve_tz(iana, posix), 1);
    tzset();
}

void orb_net_time_restore(void)
{
    nvs_handle_t h;
    if (nvs_open(ORB_NET_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;   /* nothing saved yet */
    }
    char iana[IANA_MAX], posix[POSIX_MAX];
    size_t iana_len = sizeof(iana), posix_len = sizeof(posix);
    bool have_iana = nvs_get_str(h, NVS_TZ, iana, &iana_len) == ESP_OK;
    bool have_posix = nvs_get_str(h, NVS_TZ_POSIX, posix, &posix_len) == ESP_OK;
    nvs_close(h);
    if (have_iana) {
        apply(iana, have_posix ? posix : NULL);
    }
}

/* Writes key=value only when it differs; NULL erases it. True if NVS changed. */
static bool save_str(nvs_handle_t h, const char *key, const char *value)
{
    char old[IANA_MAX > POSIX_MAX ? IANA_MAX : POSIX_MAX];
    size_t len = sizeof(old);
    bool present = nvs_get_str(h, key, old, &len) == ESP_OK;
    if (!value) {
        return present && nvs_erase_key(h, key) == ESP_OK;
    }
    if (present && strcmp(old, value) == 0) {
        return false;
    }
    if (nvs_set_str(h, key, value) != ESP_OK) {
        ESP_LOGW(TAG, "time zone %s not saved", key);
        return false;
    }
    return true;
}

void orb_net_set_timezone(const char *iana, const char *posix)
{
    if (!orb_net_posix_tz_valid(posix)) {
        posix = NULL;
    }
    apply(iana, posix);
    if (!iana || strlen(iana) >= IANA_MAX) {
        return;
    }
    ESP_LOGI(TAG, "time zone %s%s%s", iana, posix ? " " : "", posix ? posix : "");

    /* Written only when it changes: the hub sends it on every hello. The rule
     * is erased when the hub sends none, so a stale one never outlives its zone. */
    nvs_handle_t h;
    if (nvs_open(ORB_NET_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    bool changed = save_str(h, NVS_TZ, iana);
    changed |= save_str(h, NVS_TZ_POSIX, posix);
    if (changed && nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "time zone not saved");
    }
    nvs_close(h);
}

void orb_net_time_start(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    cfg.wait_for_sync = false;   /* NETIF_SNTP_EVENT tells orb_net instead */
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP: %s", esp_err_to_name(err));
        return;
    }
    s_sntp_started = true;
}
