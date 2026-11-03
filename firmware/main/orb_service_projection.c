#include "orb_service_projection.h"

#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "orb_board.h"
#include "orb_link.h"
#include "orb_net.h"
#include "orb_ota.h"
#include "orb_time.h"

void orb_main_status_project(orb_shell_snapshot_t *view, int64_t now,
    const char *service_error, const char *action_error, int64_t action_error_until, bool demo)
{
    orb_net_status_t net = { 0 };
    orb_link_status_t link = { 0 };
    orb_ota_status_t ota = { 0 };
    bool have_net = orb_net_status(&net) == ESP_OK;
    orb_link_status(&link);
    orb_ota_status(&ota);
    static const orb_shell_net_t s_net_states[] = {
        ORB_SHELL_NET_UNPROVISIONED, ORB_SHELL_NET_PROVISIONING, ORB_SHELL_NET_CONNECTING,
        ORB_SHELL_NET_CONNECTED, ORB_SHELL_NET_FAILED,
    };
    static const orb_shell_link_t s_link_states[] = {
        ORB_SHELL_LINK_OFFLINE, ORB_SHELL_LINK_SEARCHING, ORB_SHELL_LINK_PAIRING,
        ORB_SHELL_LINK_CONNECTING, ORB_SHELL_LINK_ONLINE, ORB_SHELL_LINK_UNAUTHORIZED,
    };
    static const orb_shell_ota_t s_ota_states[] = {
        ORB_SHELL_OTA_IDLE, ORB_SHELL_OTA_OFFERED, ORB_SHELL_OTA_DOWNLOADING,
        ORB_SHELL_OTA_VERIFYING, ORB_SHELL_OTA_READY, ORB_SHELL_OTA_FAILED,
    };
    view->net = have_net ? s_net_states[net.state] : ORB_SHELL_NET_STARTING;
    view->link = s_link_states[link.state];
    view->ota = s_ota_states[ota.state];
    strlcpy(view->service_name, net.service_name, sizeof(view->service_name));
    strlcpy(view->pop, net.pop, sizeof(view->pop));
    strlcpy(view->ssid, net.ssid, sizeof(view->ssid));
    if (link.state == ORB_LINK_PAIRING) strlcpy(view->pairing_code, link.sas, sizeof(view->pairing_code));
    strlcpy(view->hub_name, link.hub_name, sizeof(view->hub_name));
    strlcpy(view->device_name, link.device_name, sizeof(view->device_name));
    strlcpy(view->device_id, orb_net_device_id(), sizeof(view->device_id));
    strlcpy(view->version, ota.version, sizeof(view->version));
    strlcpy(view->firmware_version, esp_app_get_description()->version, sizeof(view->firmware_version));
    strlcpy(view->notes, ota.notes, sizeof(view->notes));
    strlcpy(view->error, ota.error, sizeof(view->error));
    const char *failure = service_error[0] ? service_error :
                          (now < action_error_until ? action_error : "");
    strlcpy(view->service_error, failure, sizeof(view->service_error));
    view->progress = ota.progress;
    view->rssi = net.rssi;
    view->demo = demo;
    if (view->demo) {
        /* Offline fixtures must never look like a live hub or use saved
         * pairing credentials. Keep their identity visible on every Face. */
        view->net = ORB_SHELL_NET_CONNECTED;
        view->link = ORB_SHELL_LINK_ONLINE;
        strlcpy(view->device_name, "Orb Demo", sizeof(view->device_name));
        strlcpy(view->device_id, "demo-offline", sizeof(view->device_id));
        strlcpy(view->hub_name, "Demo · offline", sizeof(view->hub_name));
        strlcpy(view->ssid, "Offline preview", sizeof(view->ssid));
        view->rssi = -127;
    }
    time_t wall = time(NULL);
    if (wall > 1700000000) {
        struct tm local; localtime_r(&wall, &local);
        orb_time_format_clock(view->clock_time, sizeof(view->clock_time), (unsigned)local.tm_hour, (unsigned)local.tm_min);
        strftime(view->clock_date, sizeof(view->clock_date), "%a, %b %e", &local);
    } /* Empty until SNTP or the hub supplies actual wall time. */
    view->desk_charge_limit = orb_board_desk_charge_limit();
}
