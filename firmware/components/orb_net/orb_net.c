/*
 * Wi-Fi for Orb (orb_net.h; docs/protocol.md, "Wi-Fi setup").
 *
 * One task owns the state. ESP-IDF's event handlers only forward what
 * happened to its queue, so state changes, reconnect timing and the app's
 * callback all happen in order on that task.
 *
 * The decisions are in orb_net_sm.c, plain logic with host tests; this file
 * turns ESP-IDF events into its events and carries out what it asks for.
 *
 * With no stored network the device runs ESP-IDF network provisioning over
 * BLE (security 1, the PoP shown on screen; the client side is in README.md).
 * Bluetooth is only for that, so once there is a network its static memory is
 * handed to the heap: at boot when a network is stored, or when provisioning
 * ends. It can't be started again without a restart after that, which is why
 * orb_net_forget() restarts into provisioning. The provisioning manager stores
 * credentials before it tries them, so a network counts at boot only once it
 * has had an address (NVS orb_net/verified); otherwise the device provisions
 * again instead of giving up Bluetooth for credentials that may be wrong.
 */
#include "orb_net.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network_provisioning/manager.h"
#include "nvs.h"
#include "orb_net_priv.h"
#include "orb_net_sm.h"
#include "orb_net_util.h"
#include "orb_mem.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED) || defined(CONFIG_BT_BLUEDROID_ENABLED)
#define ORB_NET_BLE 1
#include "bootloader_random.h"
#include "esp_bt.h"
#include "network_provisioning/scheme_ble.h"
#if !CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1
#error "orb_net provisions with security 1: set CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1=y (see README.md)"
#endif
/* protocomm's security 1 logs the session key at INFO; prov_start turns that
 * tag down, which needs per-tag log levels. */
#if CONFIG_LOG_MAXIMUM_LEVEL >= 3 && (!CONFIG_LOG_DYNAMIC_LEVEL_CONTROL || CONFIG_LOG_TAG_LEVEL_IMPL_NONE)
#error "orb_net needs per-tag log levels (CONFIG_LOG_DYNAMIC_LEVEL_CONTROL) to keep provisioning keys out of the log"
#endif
#else
/* Without Bluetooth in sdkconfig (README.md) a device with no stored network
 * can't be set up: it stays UNPROVISIONED and says why in the log. */
#define ORB_NET_BLE 0
#endif

static const char *TAG = "net";

#define NET_CORE 0             /* with Wi-Fi and the link; the UI has core 1 */
#define NET_PRIORITY 4
#define NET_STACK 4096         /* event handling plus the app's (non-blocking) callback */
#define PROV_JOIN_ATTEMPTS 3   /* joins with new credentials before the client is told they failed */
#define RESTART_DELAY_MS 500   /* lets the UI show UNPROVISIONED before restarting into provisioning */
#define NVS_VERIFIED "verified"   /* u8: the stored network has had an address */

typedef enum {
    EV_STA_START,
    EV_DISCONNECTED,
    EV_GOT_IP,
    EV_CREDENTIALS,
    EV_CREDENTIALS_FAILED,
    EV_PROV_END,
    EV_TIME_SYNCED,
    EV_FORGET,
    EV_POWER_HINT,
} ev_type_t;

typedef struct {
    ev_type_t type;
    union {
        uint16_t reason;     /* EV_DISCONNECTED: wifi_err_reason_t */
        esp_ip4_addr_t ip;   /* EV_GOT_IP */
        char ssid[33];       /* EV_CREDENTIALS */
    };
} ev_t;

static QueueHandle_t s_events;
static orb_net_cb_t s_cb;
static void *s_ctx;
static char s_device_id[ORB_NET_DEVICE_ID_LEN], s_service[ORB_NET_SHORT_NAME_LEN], s_host[ORB_NET_SHORT_NAME_LEN];

/* Owned by the task (and by orb_net_start before the task exists). */
static orb_net_status_t s_cur;   /* state mirrors s_sm.state */
static orb_net_sm_t s_sm;
static orb_net_retry_t s_retry;
static uint32_t s_prompt_generation;

/* The copy other tasks read. */
static orb_net_status_t s_pub;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void names_init(void)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        orb_net_names(mac, s_device_id, s_service, s_host);
    }
}

static void publish(void)
{
    s_cur.rssi = 0;
    if (s_cur.state == ORB_NET_CONNECTED) {
        esp_wifi_sta_get_rssi(&s_cur.rssi);
    }
    portENTER_CRITICAL(&s_lock);
    s_pub = s_cur;
    portEXIT_CRITICAL(&s_lock);
    if (s_cb) {
        const orb_net_status_t st = s_cur;
        s_cb(&st, s_ctx);
    }
}

static int internal_free_kb(void)
{
    return (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
}

/* At boot with a stored network: Bluetooth was never started, so all of its
 * static memory can go to the heap now. */
static void release_bluetooth(void)
{
#if ORB_NET_BLE
    int before = internal_free_kb();
    orb_mem_heap_regions_begin();
    esp_err_t err = esp_bt_mem_release(ESP_BT_MODE_BTDM);
    orb_mem_heap_regions_end();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Bluetooth memory released: %d KB more internal RAM", internal_free_kb() - before);
    } else {
        ESP_LOGW(TAG, "Bluetooth memory release: %s", esp_err_to_name(err));
    }
#endif
}

#if ORB_NET_BLE
static void provisioning_release_bluetooth(void *context, network_prov_cb_event_t event, void *data)
{
    /* The stock handler also returns Bluetooth regions at INIT/DEINIT. Keep
     * those insertions outside IDF's ordinal-based local-minimum snapshot. */
    orb_mem_heap_regions_begin();
    network_prov_scheme_ble_event_cb_free_btdm(context, event, data);
    orb_mem_heap_regions_end();
}

/* Lowers a tag's log level, never raises it. */
static void log_at_most(const char *tag, esp_log_level_t level)
{
    if (esp_log_level_get(tag) > level) {
        esp_log_level_set(tag, level);
    }
}
#endif

#if ORB_NET_BLE
static void provision_config(void *ctx, network_prov_cb_event_t event, void *data)
{
    if (event == NETWORK_PROV_SET_WIFI_STA_CONFIG && data) {
        /* This synchronous callback runs before the manager sets station config. */
        ((wifi_config_t *)data)->sta.listen_interval = 3;
    }
}
#endif

static esp_err_t prov_start(void)
{
#if ORB_NET_BLE
    const network_prov_mgr_config_t cfg = {
        .scheme = network_prov_scheme_ble,
        .app_event_handler = { .event_cb = provision_config },
        /* Hands Bluetooth's memory to the heap when provisioning is over. */
        .scheme_event_handler = {.event_cb = provisioning_release_bluetooth},
        .network_prov_wifi_conn_cfg = { .wifi_conn_attempts = PROV_JOIN_ATTEMPTS },
    };
    ESP_RETURN_ON_ERROR(network_prov_mgr_init(cfg), TAG, "provisioning init");
    /* protocomm's security1.c hex-dumps the session key, the CTR nonce and
     * both public keys at INFO: with a capture of the BLE traffic, that log
     * would give away the Wi-Fi password. The manager logs the SSID and a
     * partly masked password at DEBUG. */
    log_at_most("security1", ESP_LOG_WARN);
    log_at_most("network_prov_mgr", ESP_LOG_INFO);
    /* The radio isn't on yet, so esp_random() needs the ADC entropy source
     * to be a true random number generator. */
    bootloader_random_enable();
    orb_net_pop(esp_random, s_cur.pop);
    bootloader_random_disable();
    /* Starts Wi-Fi as well, for the network scan the client asks for. */
    esp_err_t err = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, s_cur.pop, s_service, NULL);
    if (err != ESP_OK) {
        memset(s_cur.pop, 0, sizeof(s_cur.pop));
        network_prov_mgr_deinit();
        /* The manager starts Wi-Fi before the BLE service and leaves it on
         * when the service fails. */
        esp_wifi_stop();
        ESP_LOGE(TAG, "provisioning start: %s", esp_err_to_name(err));
        return err;
    }
    strcpy(s_cur.service_name, s_service);
    ESP_LOGI(TAG, "provisioning over BLE as %s", s_service);
    return ESP_OK;
#else
    ESP_LOGE(TAG, "no working network stored and BLE provisioning isn't built in (needs CONFIG_BT_NIMBLE_ENABLED)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void prov_reset(void)
{
    esp_err_t err = network_prov_mgr_reset_wifi_sm_state_on_failure();
    if (err != ESP_OK) {
        /* The client got there first (prov-ctrl reset), or sent new credentials already. */
        ESP_LOGW(TAG, "provisioning reset: %s", esp_err_to_name(err));
    }
}

static void prov_end(void)
{
    int before = internal_free_kb();
    network_prov_mgr_deinit();
    s_cur.service_name[0] = '\0';
    memset(s_cur.pop, 0, sizeof(s_cur.pop));
    ESP_LOGI(TAG, "provisioning over; Bluetooth released, %d KB more internal RAM", internal_free_kb() - before);
}

static bool network_stored(void)
{
    wifi_config_t cfg = { 0 };
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0];
}

static bool load_verified(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(ORB_NET_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_VERIFIED, &v);
        nvs_close(h);
    }
    return v;
}

static void save_verified(bool verified)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ORB_NET_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_VERIFIED, verified);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        /* Not fatal: the next boot provisions again, or trusts a network one boot longer. */
        ESP_LOGW(TAG, "network check not saved: %s", esp_err_to_name(err));
    }
}

/* Called with UNPROVISIONED published: gives the UI time to show it. */
static void restart(void)
{
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}

/* Carries out what orb_net_sm_step asked for, in the order orb_net_sm.h lists. */
static void act(unsigned todo)
{
    if (todo & ORB_NET_DO_RESET_PROV) {
        prov_reset();
    }
    if (todo & ORB_NET_DO_END_PROV) {
        prov_end();
    }
    if (todo & ORB_NET_DO_SAVE_VERIFIED) {
        save_verified(s_sm.verified);
    }
    if (todo & ORB_NET_DO_CANCEL_JOIN) {
        s_retry.due_ms = 0;
    }
    if (todo & ORB_NET_DO_JOIN) {
        bool background = orb_net_get_power_hint() == ORB_NET_POWER_BACKGROUND;
        uint32_t ms = orb_net_retry_ms(s_sm.failures, s_sm.absent, background, esp_random());
        s_retry.due_ms = esp_timer_get_time() / 1000 + ms;
    }
    s_cur.state = s_sm.state;
    orb_net_power_provisioning(s_sm.provisioning);
    if (todo & ORB_NET_DO_PUBLISH) {
        publish();
    }
    if (todo & ORB_NET_DO_START_TIME) {
        orb_net_time_start();
    }
    if (todo & ORB_NET_DO_RESTART) {
        ESP_LOGW(TAG, "provisioning ended with no network stored; restarting into provisioning");
        restart();
    }
}

static void step(const orb_net_sm_ev_t *ev)
{
    act(orb_net_sm_step(&s_sm, ev));
}

static void join(void)
{
    s_retry.due_ms = 0;
    wifi_config_t cfg = { 0 };
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK && cfg.sta.listen_interval != 3) {
        cfg.sta.listen_interval = 3;
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    if (err == ESP_OK) {
        orb_net_count_scan();
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "join: %s", esp_err_to_name(err));
        step(&(const orb_net_sm_ev_t){ .type = ORB_NET_SM_JOIN_ERROR });
    }
}

/* Disconnects that mean these credentials won't work here (wrong password,
 * no such network in range) rather than a flaky link. */
static bool is_refusal(uint16_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return true;
    default:
        return false;
    }
}

/* Erases the network and restarts: Bluetooth is released by now (or is
 * mid-provisioning), and a clean boot is the one way back to provisioning. */
static void forget(void)
{
    ESP_LOGW(TAG, "forgetting %s; restarting into provisioning", s_cur.ssid);
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase network: %s", esp_err_to_name(err));
        return;
    }
    memset(&s_cur, 0, sizeof(s_cur));
    s_cur.state = ORB_NET_UNPROVISIONED;
    publish();
    restart();
}

static void handle(const ev_t *ev)
{
    orb_net_sm_ev_t sm_ev = { 0 };
    switch (ev->type) {
    case EV_STA_START:
        sm_ev.type = ORB_NET_SM_STA_START;
        break;
    case EV_DISCONNECTED:
        if (s_cur.state == ORB_NET_CONNECTED) {
            ESP_LOGW(TAG, "lost %s (reason %u)", s_cur.ssid, ev->reason);
        }
        s_cur.ip[0] = '\0';
        sm_ev.type = ORB_NET_SM_DISCONNECTED;
        sm_ev.refused = is_refusal(ev->reason);
        sm_ev.absent = ev->reason == WIFI_REASON_NO_AP_FOUND ||
                       ev->reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
                       ev->reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
                       ev->reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
        break;
    case EV_GOT_IP:
        if (s_sm.state != ORB_NET_CONNECTED) {
            orb_net_count_join();
        }
        esp_ip4addr_ntoa(&ev->ip, s_cur.ip, sizeof(s_cur.ip));
        ESP_LOGI(TAG, "joined %s as %s", s_cur.ssid, s_cur.ip);
        sm_ev.type = ORB_NET_SM_GOT_IP;
        break;
    case EV_CREDENTIALS:
        memcpy(s_cur.ssid, ev->ssid, sizeof(s_cur.ssid));
        ESP_LOGI(TAG, "provisioning: trying %s", s_cur.ssid);
        sm_ev.type = ORB_NET_SM_CREDENTIALS;
        break;
    case EV_CREDENTIALS_FAILED:
        sm_ev.type = ORB_NET_SM_CREDENTIALS_FAILED;
        break;
    case EV_PROV_END:
        sm_ev.type = ORB_NET_SM_PROV_END;
        sm_ev.stored = network_stored();
        break;
    case EV_TIME_SYNCED:
        if (!s_cur.time_synced) {
            s_cur.time_synced = true;
            publish();
        }
        return;
    case EV_FORGET:
        forget();
        return;
    case EV_POWER_HINT:
        return;   /* authoritative hint is read before the next queue wait */
    }
    const orb_net_state_t before = s_sm.state;
    step(&sm_ev);
    if (s_sm.state == ORB_NET_FAILED && before != ORB_NET_FAILED) {
        if (ev->type == EV_DISCONNECTED) {
            ESP_LOGW(TAG, "%s keeps refusing (reason %u); still retrying", s_cur.ssid, ev->reason);
        } else {
            ESP_LOGW(TAG, "provisioning: %s refused; waiting for new credentials", s_cur.ssid);
        }
    }
}

static void net_task(void *arg)
{
    publish();   /* the state orb_net_start settled on */
    for (;;) {
        uint32_t generation = orb_net_interactive_generation();
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (generation != s_prompt_generation) {
            orb_net_retry_prompt(&s_retry, now_ms);
            s_prompt_generation = generation;
        }
        orb_net_power_apply();
        TickType_t wait = portMAX_DELAY;
        if (s_retry.due_ms) {
            int64_t left_ms = s_retry.due_ms - now_ms;
            if (left_ms <= 0) {
                join();
                continue;
            }
            wait = pdMS_TO_TICKS(left_ms) + 1;
        }
        ev_t ev;
        if (xQueueReceive(s_events, &ev, wait) == pdTRUE) {
            handle(&ev);
        }
    }
}

/* Runs on the default event loop's task: copy what's needed and move on. */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ev_t ev = { 0 };
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        orb_net_count_scan();
        return;   /* connect-triggered scans do not emit SCAN_DONE (ESP-IDF) */
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ev.type = EV_STA_START;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ev.type = EV_DISCONNECTED;
        ev.reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ev.type = EV_GOT_IP;
        ev.ip = ((const ip_event_got_ip_t *)data)->ip_info.ip;
    } else if (base == NETWORK_PROV_EVENT && id == NETWORK_PROV_WIFI_CRED_RECV) {
        /* The event also carries the password; only the SSID goes further. */
        ev.type = EV_CREDENTIALS;
        memcpy(ev.ssid, ((const wifi_sta_config_t *)data)->ssid, sizeof(ev.ssid) - 1);
    } else if (base == NETWORK_PROV_EVENT && id == NETWORK_PROV_WIFI_CRED_FAIL) {
        ev.type = EV_CREDENTIALS_FAILED;
    } else if (base == NETWORK_PROV_EVENT && id == NETWORK_PROV_END) {
        ev.type = EV_PROV_END;
    } else if (base == NETIF_SNTP_EVENT) {
        ev.type = EV_TIME_SYNCED;
    } else {
        return;
    }
    if (xQueueSend(s_events, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "event %d dropped", ev.type);
    }
}

esp_err_t orb_net_start(orb_net_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(!s_events, ESP_ERR_INVALID_STATE, TAG, "already started");
    names_init();
    s_cb = cb;
    s_ctx = ctx;
    orb_net_time_restore();

    esp_err_t err = esp_netif_init();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "netif init");
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, TAG, "station netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(netif, s_host), TAG, "DHCP host name");
    const wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "wifi init");
    /* Keep BLE coexistence at the default minimum policy throughout setup. */
    orb_net_power_provisioning(true);
    orb_net_power_apply();

    /* Handlers go in before Wi-Fi starts; what they queue waits for the task. */
    QueueHandle_t events = xQueueCreate(16, sizeof(ev_t));
    ESP_RETURN_ON_FALSE(events, ESP_ERR_NO_MEM, TAG, "queue");
    portENTER_CRITICAL(&s_lock);
    s_events = events;
    portEXIT_CRITICAL(&s_lock);
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG,
                        "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, on_event, NULL), TAG,
                        "events");

    wifi_config_t stored = { 0 };
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &stored), TAG, "stored network");
    bool verified = load_verified();
    orb_net_state_t state;
    if (stored.sta.ssid[0] && verified) {
        state = ORB_NET_CONNECTING;
    } else {
        if (stored.sta.ssid[0]) {
            ESP_LOGW(TAG, "%.*s never connected; provisioning again", (int)sizeof(stored.sta.ssid),
                     (const char *)stored.sta.ssid);
        } else if (verified) {
            verified = false;   /* it was about a network that is gone; the next one is untried */
            save_verified(false);
        }
        if (prov_start() == ESP_OK) {
            state = ORB_NET_PROVISIONING;
        } else {
            /* Without provisioning, a stored network is still worth trying. */
            state = stored.sta.ssid[0] ? ORB_NET_CONNECTING : ORB_NET_UNPROVISIONED;
        }
    }
    if (state == ORB_NET_CONNECTING) {
        memcpy(s_cur.ssid, stored.sta.ssid, sizeof(stored.sta.ssid));
        release_bluetooth();
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "station mode");
        stored.sta.listen_interval = 3;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &stored), TAG, "listen interval");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    }
    orb_net_sm_init(&s_sm, state, verified);
    orb_net_power_provisioning(s_sm.provisioning);
    s_cur.state = state;
    portENTER_CRITICAL(&s_lock);
    s_pub = s_cur;   /* orb_net_status is right before the task's first callback */
    portEXIT_CRITICAL(&s_lock);

    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(net_task, "net", NET_STACK, NULL, NET_PRIORITY, NULL, NET_CORE) ==
                            pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

esp_err_t orb_net_status(orb_net_status_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out");
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_INVALID_STATE, TAG, "not started");
    portENTER_CRITICAL(&s_lock);
    *out = s_pub;
    portEXIT_CRITICAL(&s_lock);
    if (out->state == ORB_NET_CONNECTED) {
        esp_wifi_sta_get_rssi(&out->rssi);   /* now, not as of the last change */
    }
    return ESP_OK;
}

bool orb_net_is_connected(void)
{
    portENTER_CRITICAL(&s_lock);
    bool connected = s_pub.state == ORB_NET_CONNECTED;
    portEXIT_CRITICAL(&s_lock);
    return connected;
}

esp_err_t orb_net_forget(void)
{
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_INVALID_STATE, TAG, "not started");
    const ev_t ev = { .type = EV_FORGET };
    ESP_RETURN_ON_FALSE(xQueueSend(s_events, &ev, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "queue full");
    return ESP_OK;
}

const char *orb_net_device_id(void)
{
    if (!s_device_id[0]) {
        names_init();
    }
    return s_device_id;
}

void orb_net_power_wake(void)
{
    portENTER_CRITICAL(&s_lock);
    QueueHandle_t events = s_events;
    portEXIT_CRITICAL(&s_lock);
    if (events) {
        const ev_t ev = { .type = EV_POWER_HINT };
        /* A full queue already wakes the owner; its next loop reads the hint. */
        xQueueSend(events, &ev, 0);
    }
}
