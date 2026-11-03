#pragma once

/*
 * The one connection to the hub (docs/protocol.md, "Pairing" and "Link"):
 * finds the hub with orb_net, pairs once (showing the 6-digit code), then keeps
 * a pinned-TLS WebSocket open, reconnecting with backoff. Requests, responses
 * and events are JSON (cJSON). Other components register handlers for the
 * methods and events they own.
 *
 * Callbacks run on orb_link's task; they must not block or call LVGL. Publish
 * model/mailbox state for the UI task instead. cJSON objects passed to callbacks are
 * owned by orb_link and only valid during the call.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "orb_link_memory.h"
#include "orb_model.h"

/* Bounded validated send receipt, also used by the existing voice flow. */
bool orb_link_read_message_receipt(const cJSON *result, const cJSON *error,
                                    orb_message_result_t *out);

typedef enum {
    ORB_LINK_OFFLINE,      /* no network, or not started */
    ORB_LINK_SEARCHING,    /* looking for the hub over mDNS */
    ORB_LINK_PAIRING,      /* unpaired: showing `sas`, waiting for `orb-hub pair <sas>` */
    ORB_LINK_CONNECTING,   /* paired: opening the link */
    ORB_LINK_ONLINE,       /* hello accepted */
    ORB_LINK_UNAUTHORIZED, /* the hub no longer knows this device; pairing restarts */
} orb_link_state_t;

typedef struct {
    orb_link_state_t state;
    bool paired;
    char sas[7];           /* while PAIRING */
    char hub_name[64];     /* from pair.begin / hello */
    char device_name[32];  /* from hello's device.name */
    char channel[16];      /* from hello's device.channel */
} orb_link_status_t;

typedef void (*orb_link_state_cb_t)(const orb_link_status_t *status, void *ctx);

/* Hub → device event. */
typedef void (*orb_link_event_cb_t)(const char *name, const cJSON *params, void *ctx);

/*
 * Hub → device request. Fill `result` (an empty object) and return ESP_OK, or
 * return an error: ESP_ERR_INVALID_ARG → bad_request, ESP_ERR_NOT_FOUND →
 * not_found, ESP_ERR_INVALID_STATE → busy, anything else → internal.
 * device.ping and device.reboot are built in; device.identify and
 * device.config belong to the app.
 */
typedef esp_err_t (*orb_link_method_cb_t)(const cJSON *params, cJSON *result, void *ctx);

/* Answer to orb_link_request: err is ESP_OK with result set, ESP_ERR_TIMEOUT
 * (also when the link drops first), or ESP_FAIL with the error in `error`
 * ({"code","msg"}): the hub's, or a local busy (16 requests already waiting)
 * or internal (the request is over 16 KB). */
typedef void (*orb_link_reply_cb_t)(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx);

/* Adds fields to every hello before it's sent (e.g. orb_ota adds last_ota). */
typedef void (*orb_link_hello_cb_t)(cJSON *hello, void *ctx);

/* The panel state at sampling time, as state.report carries it. */
typedef enum {
    ORB_LINK_DISPLAY_ACTIVE,
    ORB_LINK_DISPLAY_DIMMED,
    ORB_LINK_DISPLAY_ALWAYS_ON,
    ORB_LINK_DISPLAY_SLEEP,
} orb_link_display_state_t;

/* Battery and charging, as hello and state.report carry them. */
typedef struct {
    int battery_pct;   /* -1 when no battery */
    int battery_mv;    /* -1 when unknown */
    bool charging;
    bool usb;
    orb_link_display_state_t display_state;
} orb_link_power_t;

/* Reads the power state now; runs on orb_link's task. */
typedef esp_err_t (*orb_link_power_cb_t)(orb_link_power_t *out, void *ctx);

/* Starts the link task. It waits for orb_net to be CONNECTED by itself. */
esp_err_t orb_link_start(orb_link_state_cb_t cb, void *ctx);

esp_err_t orb_link_status(orb_link_status_t *out);

typedef struct {
    uint64_t messages_rx, messages_tx;
    uint64_t bytes_rx, bytes_tx;
    uint32_t connects, reconnects;
} orb_link_counters_t;

/* Since boot: complete application JSON/binary messages and payload bytes.
 * Successful transport writes only; excludes WebSocket/TLS headers and pings.
 * connects counts attempts, reconnects excludes the first attempt. */
esp_err_t orb_link_counters(orb_link_counters_t *out);

/*
 * Register before or after start; up to 16 of each, names under 32 bytes.
 * Several handlers may share an event; a method has one handler, and
 * registering its name again replaces it (built-ins included).
 */
esp_err_t orb_link_on_event(const char *name, orb_link_event_cb_t cb, void *ctx);
esp_err_t orb_link_on_method(const char *method, orb_link_method_cb_t cb, void *ctx);
esp_err_t orb_link_on_hello(orb_link_hello_cb_t cb, void *ctx);

/*
 * Where hello and state.report get "power" from; it's left out while unset or
 * when cb fails. state.report goes out every 60 s, and within 5 s of a change.
 */
esp_err_t orb_link_set_power_source(orb_link_power_cb_t cb, void *ctx);
/* Eligible idle polls local outgoing queues at 1 Hz; leaving it immediately
 * prompts a pump so the first intentional interaction retains 50 ms latency. */
void orb_link_set_sleep_eligible(bool eligible);

/*
 * Sends a request; takes ownership of params (may be NULL for {}). cb may be
 * NULL; timeout_ms <= 0 means 10 s. ESP_ERR_INVALID_STATE when not ONLINE
 * (params are freed).
 */
esp_err_t orb_link_request(const char *method, cJSON *params, int timeout_ms, orb_link_reply_cb_t cb, void *ctx);
/* Recording operations belong to the source connection. Reject instead of
 * adopting a newly connected transport between the worker's poll and send. */
esp_err_t orb_link_request_on(uint32_t generation, const char *method, cJSON *params,
                             int timeout_ms, orb_link_reply_cb_t cb, void *ctx);

/* Sends an event; takes ownership of params. Dropped when not ONLINE. */
esp_err_t orb_link_notify(const char *event, cJSON *params);
esp_err_t orb_link_notify_on(uint32_t generation, const char *event, cJSON *params);

/* Complete bounded inbound audio frame on the link task, ordered with JSON.
 * Callback storage is valid only for this call; do not block. */
typedef void (*orb_link_audio_cb_t)(const uint8_t *frame, size_t bytes, uint32_t generation, void *ctx);
esp_err_t orb_link_on_audio(orb_link_audio_cb_t cb, void *ctx);

bool orb_link_online(void);

/* Negotiated hello capabilities, false until hello acceptance. A generation
 * identifies one WebSocket connection; recording IDs are unique within it. */
bool orb_link_supports_cards(void);
bool orb_link_supports_home(void);
bool orb_link_supports_weather(void);
bool orb_link_supports_voice(void);
uint32_t orb_link_generation(void);
/* Set by the voice service before link startup, after its worker is ready. */
void orb_link_set_voice_available(bool available);

/* Copies a framed voice packet (7-byte header + <=1024 PCM bytes) into bounded
 * PSRAM queue storage. Same FIFO as requests: voice.end follows accepted PCM.
 * ESP_OK means queued; reconnect discards old-generation data. */
esp_err_t orb_link_send_audio(const void *frame, size_t len);
esp_err_t orb_link_send_audio_on(uint32_t generation, const void *frame, size_t len);

/* Forgets the hub (token, pinned certificate) and starts pairing again. An
 * unauthorized hello does the same by itself. */
esp_err_t orb_link_unpair(void);

/* What HTTPS to the hub (firmware downloads) needs, as copies: the pairing can
 * change at any time (orb_link_unpair, an unauthorized hello). */
typedef struct {
    char base_url[32];        /* "https://<ip>:<port>" */
    char authorization[72];   /* "Bearer <token>" */
    char *cert_pem;           /* the pinned certificate, the only one to trust; free() it */
} orb_link_https_t;

/*
 * Fills `out` from the current pairing. Give cert_pem to esp-tls as the only
 * trusted certificate and skip its name check (skip_cert_common_name_check):
 * base_url holds the hub's last IP, which a new DHCP lease takes out of the
 * certificate, and the pin alone identifies the hub. ESP_ERR_INVALID_STATE
 * when unpaired, ESP_ERR_NO_MEM; cert_pem is NULL then.
 */
esp_err_t orb_link_hub_https(orb_link_https_t *out);

/* Development-channel capability, revoked on channel change/disconnect. */
bool orb_link_supports_devtools(void);
/* Takes params ownership; cb reports the actual bounded transport write.
 * Output remains tied to generation after waiting for queue/send contention. */
esp_err_t orb_link_devtools_notify_on(uint32_t generation, const char *event, cJSON *params,
                                     orb_link_reply_cb_t cb, void *ctx);
/* Absolute esp_timer deadline shared by all receipts in an operation. The
 * queue and transport enforce it as well as the normal 5 s write limit. */
esp_err_t orb_link_devtools_notify_until(uint32_t generation, const char *event, cJSON *params,
                                        int64_t deadline_us, orb_link_reply_cb_t cb, void *ctx);
