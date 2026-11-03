/*
 * The link to the hub (docs/protocol.md, "Pairing" and "Link").
 *
 * One task owns everything: discovery, the certificate fetch, the WebSocket,
 * the request table, NVS and every callback. Other tasks only post to its
 * queue, and the WebSocket client's own task only forwards connection changes
 * and whole text messages. What other tasks read (the status, the handler
 * tables, the pairing strings for orb_ota) sits behind s_lock.
 *
 * Wire formats live in link_proto.c, the pairing code in link_sas.c and the
 * reconnect delays in link_backoff.c, all ESP-free and host-tested.
 */
#include "orb_link.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "link_backoff.h"
#include "link_control.h"
#include "link_device.h"
#include "link_crash.h"
#include "link_radio.h"
#include "link_proto.h"
#include "link_sas.h"
#include "link_v11.h"
#include "link_home.h"
#include "mbedtls/pem.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "orb_net.h"
#include "orb_time.h"
#include "orb_model.h"
#include "orb_mem.h"
#include "orb_memory.h"
#include "psa/crypto.h"

static const char *TAG = "link";

#define LINK_CORE 0            /* Wi-Fi, link and audio on core 0; LVGL on core 1 */
#define LINK_PRIORITY 4
#define LINK_STACK 8192        /* X.509, the HTTP client and cJSON (LINK_MAX_DEPTH) are the deep paths */
#define WS_PRIORITY 5
#define WS_STACK 6144          /* the TLS handshake runs on the WebSocket task */
#define WS_BUFFER 4096         /* longer messages go out as fragments, come in as chunks */
#define QUEUE_LEN 32
#define MAX_HANDLERS 16
/* Methods outnumber events and hellos: devtools alone registers 22 over Wi-Fi.
 * A full table aborts boot (main's device.identify uses ESP_ERROR_CHECK), so
 * leave headroom; each slot is 40 bytes of internal RAM. */
#define MAX_METHOD_HANDLERS 40
#define BUILTIN_METHODS 2
#define MAX_PENDING 16
#define MAX_HUBS 4
#define MAX_PEM 4096           /* a P-256 certificate is under 1 KB */
#define NAME_LEN 32
#define EXT_MAX_MSG 4096

#define REQUEST_MS 10000       /* the protocol's default request timeout */
#define CONNECT_MS 15000       /* TCP, TLS and the upgrade */
#define NETWORK_MS 10000
#define SEND_MS 5000
#define CLOSE_MS 2000
#define CERT_FETCH_MS 5000
#define DISCOVERY_MS 3000
#define NET_POLL_MS 1000
#define PAIR_WAIT_MS ((5 * 60 + 10) * 1000)   /* the hub holds a request 5 min */
#define REPORT_MS 60000
#define POWER_POLL_MS 5000
#define MAX_SLEEP_MS 60000
#define PING_S 15
#define PONG_TIMEOUT_S 45      /* the hub drops a link silent this long */
#define PAIR_PORT 8080         /* when the TXT record has no pair= */

#define NVS_NAMESPACE "orb_link"

#define PEM_BEGIN "-----BEGIN CERTIFICATE-----\n"
#define PEM_END "-----END CERTIFICATE-----\n"

#define US(ms) ((int64_t)(ms) * 1000)

typedef enum {
    MSG_WS_STATE,   /* advisory wake; authoritative state is outside this queue */
    MSG_WS_TEXT,
    MSG_WS_BINARY,
    MSG_REQUEST,    /* from the API */
    MSG_EVENT,
    MSG_AUDIO,
    MSG_UNPAIR,
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    uint32_t conn;          /* the connection it came from, or was meant for */
    char *text;             /* WS_TEXT: the message, NUL-terminated, owned */
    size_t len;
    uint32_t sequence;      /* WS_TEXT: accepted-frame order within conn */
    char name[NAME_LEN];    /* REQUEST, EVENT */
    cJSON *body;            /* REQUEST, EVENT: params, owned */
    int timeout_ms;
    orb_link_reply_cb_t cb;
    void *ctx;
    int64_t deadline;       /* API admission through transport, including queue wait */
} msg_t;

typedef struct {
    int id;                 /* 0: free */
    int64_t deadline;       /* esp_timer µs */
    orb_link_reply_cb_t cb;
    void *ctx;
} pending_t;

typedef struct {
    char name[NAME_LEN];
    orb_link_event_cb_t cb;
    void *ctx;
} event_handler_t;

typedef struct {
    char name[NAME_LEN];
    orb_link_method_cb_t cb;
    void *ctx;
} method_handler_t;

typedef struct {
    orb_link_hello_cb_t cb;
    void *ctx;
} hello_handler_t;

static esp_err_t method_ping(const cJSON *params, cJSON *result, void *ctx);
static esp_err_t method_reboot(const cJSON *params, cJSON *result, void *ctx);

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Under s_lock. */
static orb_link_status_t s_status;
static event_handler_t s_events[MAX_HANDLERS];
static method_handler_t s_methods[MAX_METHOD_HANDLERS + BUILTIN_METHODS] = {
    { "device.ping", method_ping, NULL },
    { "device.reboot", method_reboot, NULL },
};
static hello_handler_t s_hellos[MAX_HANDLERS];
static int s_n_events, s_n_methods = BUILTIN_METHODS, s_n_hellos;
static orb_link_power_cb_t s_power_cb;
static void *s_power_ctx;
static orb_link_audio_cb_t s_audio_cb;
static void *s_audio_ctx;

/* The pairing (NVS namespace orb_link). Written by the link task (or by
 * orb_link_unpair before start); token[0] == '\0' means unpaired. */
static struct {
    char token[65];
    char *pem;                 /* exactly one certificate (pin_cert) */
    uint8_t fp[LINK_FP_LEN];   /* SHA-256 of the certificate, to pick our hub out of mDNS */
    char ip[16];               /* last address that worked */
    char host[64];
    uint16_t port;
} s_pair;
static char s_base_url[32];        /* "https://<ip>:<port>" */
static char s_auth[7 + 64 + 1];    /* "Bearer <token>" */
_Static_assert(sizeof(s_base_url) == sizeof(((orb_link_https_t *)0)->base_url), "base_url size");
_Static_assert(sizeof(s_auth) == sizeof(((orb_link_https_t *)0)->authorization), "authorization size");

static orb_link_state_cb_t s_state_cb;
static void *s_state_ctx;
static TaskHandle_t s_task;
static QueueHandle_t s_queue;
static link_control_t s_control; /* Under s_lock; never dropped with payloads. */

/* The link task's own. */
static esp_websocket_client_handle_t s_ws;
static volatile uint32_t s_conn;   /* bumped per connection; the API tags messages with it */
static bool s_up;                  /* open, or draining accepted text after peer close */
static bool s_peer_closed;         /* stops sending before the accepted prefix drains */
static uint32_t s_handled_frame;
static bool s_end;                 /* the connection should end */
static bool s_unpair;              /* forget the pairing once the connection is down */
static int s_next_id;
static pending_t s_pending[MAX_PENDING];
static int64_t s_report_at, s_power_at;
static bool s_have_power;
static link_power_t s_power;       /* as last told to the hub */

/* One connection attempt: what it's for and how it went. */
static struct {
    orb_hub_addr_t hub;
    bool pairing;
    char *pem;                     /* pairing: the certificate just fetched, pinned */
    uint8_t fp[LINK_FP_LEN];
    uint8_t nonce_d[LINK_NONCE_LEN];
    link_pair_begin_result_t begin; /* the hub's nonce_h and name, from pair.begin */
    int64_t pair_deadline;         /* 0 until pair.reveal is answered */
    bool progressed;               /* the hub answered: retry soon */
    bool unauthorized;
    bool reboot;
} s_sess;

/* The text message being received, on the WebSocket task. */
static char *s_rx;
static size_t s_rx_len;
static bool s_rx_open, s_rx_skip;
static bool s_rx_binary_open;
static size_t s_rx_binary_len; /* Ignored inbound binary still obeys its aggregate ceiling. */

/* Capabilities are published under s_lock. Delivery state belongs to the link
 * task; LVGL only observes snapshots and enqueues user intent. */
static bool s_cards, s_voice, s_home, s_usage, s_nowplaying, s_weather;
static bool s_media_pending;
static orb_media_request_t s_media_request;
static bool s_devtools;
static bool s_voice_available;
static bool s_rx_too_large;
static bool s_subscribe_pending;
static int64_t s_model_at, s_answer_at;
static bool s_answer_pending;
static orb_answer_t s_answer;
static bool s_message_pending;
static orb_message_t s_message;

static void pump_model(int64_t now);

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static int rssi(void)
{
    orb_net_status_t net;
    return orb_net_status(&net) == ESP_OK ? net.rssi : 0;
}

static void sha256(const void *data, size_t len, uint8_t out[32])
{
    size_t n;
    /* PSA is up from boot and SHA-256 is always built in. Should it still fail,
     * a zero hash makes pairing fail rather than pass. */
    if (psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &n) != PSA_SUCCESS) {
        memset(out, 0, 32);
    }
}

/*
 * The hub's PEM must hold exactly one certificate. esp-tls trusts every
 * certificate it's given, while the pairing code and the TXT fp cover one, so
 * a second one would be trusted unchecked: a man in the middle could serve the
 * real certificate plus its own and pass the code. Returns that certificate
 * alone, re-encoded, so the bytes trusted are exactly the ones hashed into
 * `fp`; NULL for anything else.
 */
static char *pin_cert(const char *pem, uint8_t fp[LINK_FP_LEN])
{
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    char *out = NULL;
    size_t len = 0;
    /* A positive result counts certificates that didn't parse. */
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1) == 0 && !crt.next) {
        /* The first call only sizes the output, NUL included. */
        mbedtls_pem_write_buffer(PEM_BEGIN, PEM_END, crt.raw.p, crt.raw.len, NULL, 0, &len);
        out = len > 0 && len <= MAX_PEM + 1 ?
              orb_memory_alloc(ORB_MEMORY_NETWORK, len) : NULL;
        if (out && mbedtls_pem_write_buffer(PEM_BEGIN, PEM_END, crt.raw.p, crt.raw.len, (unsigned char *)out, len,
                                            &len) == 0) {
            sha256(crt.raw.p, crt.raw.len, fp);
        } else {
            orb_memory_free(out);
            out = NULL;
        }
    }
    mbedtls_x509_crt_free(&crt);
    return out;
}

/* TXT fp= is the first 16 hex digits of the certificate's SHA-256. */
static bool fp_matches(const uint8_t fp[LINK_FP_LEN], const char *txt)
{
    char hex[2 * LINK_FP_LEN + 1];
    link_hex(fp, LINK_FP_LEN, hex);
    return strlen(txt) == 16 && strncasecmp(hex, txt, 16) == 0;
}

/* ---- Status ---- */

/* Publishes a new status. Only the link task writes it once started, so the
 * task may read s_status without the lock to build `st`. */
static void commit(const orb_link_status_t *st)
{
    taskENTER_CRITICAL(&s_lock);
    bool changed = memcmp(&s_status, st, sizeof(*st)) != 0;
    s_status = *st;
    taskEXIT_CRITICAL(&s_lock);
    if (changed && s_state_cb) {
        s_state_cb(st, s_state_ctx);
    }
}

static void set_state(orb_link_state_t state)
{
    orb_link_status_t st = s_status;
    st.state = state;
    if (state != ORB_LINK_PAIRING) {
        st.sas[0] = '\0';
    }
    commit(&st);
}

/* ---- Pairing storage ---- */

static void set_address(const char *ip, const char *host, uint16_t port)
{
    char ip_copy[sizeof(s_pair.ip)], host_copy[sizeof(s_pair.host)], url[sizeof(s_base_url)] = "";
    snprintf(ip_copy, sizeof(ip_copy), "%s", ip);
    snprintf(host_copy, sizeof(host_copy), "%s", host);
    if (ip[0]) {
        snprintf(url, sizeof(url), "https://%s:%u", ip, port);
    }
    taskENTER_CRITICAL(&s_lock);
    memcpy(s_pair.ip, ip_copy, sizeof(ip_copy));
    memcpy(s_pair.host, host_copy, sizeof(host_copy));
    s_pair.port = port;
    memcpy(s_base_url, url, sizeof(url));
    taskEXIT_CRITICAL(&s_lock);
}

/* Makes a pairing current; takes `pem`, which pin_cert made. */
static void install_pairing(const char *token, char *pem, const uint8_t fp[LINK_FP_LEN])
{
    char auth[sizeof(s_auth)];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    taskENTER_CRITICAL(&s_lock);
    s_pair.pem = pem;
    memcpy(s_pair.fp, fp, LINK_FP_LEN);
    memcpy(s_auth, auth, sizeof(auth));
    memcpy(s_pair.token, token, sizeof(s_pair.token));
    taskEXIT_CRITICAL(&s_lock);

    orb_link_status_t st = s_status;
    st.paired = true;
    commit(&st);
}

static void forget_pairing(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    taskENTER_CRITICAL(&s_lock);
    char *pem = s_pair.pem;
    memset(&s_pair, 0, sizeof(s_pair));
    s_base_url[0] = '\0';
    s_auth[0] = '\0';
    taskEXIT_CRITICAL(&s_lock);
    orb_memory_free(pem);
    ESP_LOGI(TAG, "forgot the hub");

    /* The names came from that hub's records. */
    orb_link_status_t st = s_status;
    st.paired = false;
    st.hub_name[0] = st.device_name[0] = st.channel[0] = '\0';
    commit(&st);
}

static esp_err_t save_address(nvs_handle_t h, const orb_hub_addr_t *hub)
{
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "hub_ip", hub->ip), TAG, "save hub_ip");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "hub_host", hub->host), TAG, "save hub_host");
    ESP_RETURN_ON_ERROR(nvs_set_u16(h, "hub_port", hub->port), TAG, "save hub_port");
    return nvs_commit(h);
}

/* Pairing step 7: device_id, token, the hub's PEM and its address. */
static esp_err_t save_pairing(const char *token, const char *pem, const orb_hub_addr_t *hub)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "open NVS");
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(nvs_set_str(h, "device_id", orb_net_device_id()), done, TAG, "save device_id");
    ESP_GOTO_ON_ERROR(nvs_set_str(h, "token", token), done, TAG, "save token");
    ESP_GOTO_ON_ERROR(nvs_set_blob(h, "hub_pem", pem, strlen(pem) + 1), done, TAG, "save hub_pem");
    ret = save_address(h, hub);
done:
    nvs_close(h);
    return ret;
}

/* After a hello: keep the address that worked, for when mDNS doesn't answer. */
static void remember_address(const orb_hub_addr_t *hub)
{
    if (strcmp(hub->ip, s_pair.ip) == 0 && strcmp(hub->host, s_pair.host) == 0 && hub->port == s_pair.port) {
        return;
    }
    set_address(hub->ip, hub->host, hub->port);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        save_address(h, hub);
        nvs_close(h);
    }
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t size)
{
    if (nvs_get_str(h, key, out, &size) != ESP_OK) {
        out[0] = '\0';
    }
}

/* The pairing in NVS, if any. A PEM that isn't exactly one certificate (as
 * pin_cert wants) counts as unpaired. */
static void load_pairing(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;   /* never paired: the namespace doesn't exist yet */
    }
    char token[65], ip[16], host[64];
    uint8_t fp[LINK_FP_LEN];
    uint16_t port = 0;
    size_t len = 0;
    char *stored = NULL, *pem = NULL;
    get_str(h, "token", token, sizeof(token));
    if (token[0] && nvs_get_blob(h, "hub_pem", NULL, &len) == ESP_OK && len > 1 && len <= MAX_PEM + 1) {
        stored = orb_memory_alloc(ORB_MEMORY_NETWORK, len);
    }
    if (stored && nvs_get_blob(h, "hub_pem", stored, &len) == ESP_OK && stored[len - 1] == '\0') {
        pem = pin_cert(stored, fp);
    }
    if (stored && !pem) {
        ESP_LOGW(TAG, "stored hub certificate is unusable; pairing again");
    }
    orb_memory_free(stored);
    get_str(h, "hub_ip", ip, sizeof(ip));
    get_str(h, "hub_host", host, sizeof(host));
    nvs_get_u16(h, "hub_port", &port);
    nvs_close(h);
    if (pem) {
        install_pairing(token, pem, fp);
        set_address(ip, host, port);
    }
}

/* ---- Sending ---- */

/* Sends and frees `text`; NULL (a message that couldn't be encoded) is skipped. */
static bool send_text_until(char *text, int64_t deadline)
{
    if (!text) return false;
    bool sent = false;
    int64_t budget_ms = deadline ? (deadline - esp_timer_get_time()) / 1000 : SEND_MS;
    if (budget_ms <= 0) { cJSON_free(text); return false; }
    if (budget_ms > SEND_MS) budget_ms = SEND_MS;
    size_t len = strlen(text);
    if ((orb_link_supports_cards() || orb_link_supports_home() || orb_link_supports_weather()) && len > EXT_MAX_MSG) {
        ESP_LOGW(TAG, "extension envelope exceeds budget");
        s_end = true;
    } else if (s_up && !s_peer_closed) {
        sent = esp_websocket_client_send_text(s_ws, text, len, pdMS_TO_TICKS(budget_ms)) == (int)len;
        if (sent) link_radio_tx(len);
        if (!sent) {
            ESP_LOGW(TAG, "send failed; dropping the link");
            s_end = true;
        }
    }
    cJSON_free(text);
    return sent;
}

static void send_text(char *text)
{
    send_text_until(text, 0);
}

/* Fails a request here, with an error shaped like the hub's. */
static void reply_local(orb_link_reply_cb_t cb, void *ctx, const char *code, const char *msg)
{
    if (!cb) {
        return;
    }
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "msg", msg);
    cb(ESP_FAIL, NULL, e, ctx);
    cJSON_Delete(e);
}

static void send_request(const char *method, cJSON *params, int timeout_ms, orb_link_reply_cb_t cb, void *ctx)
{
    int64_t deadline = esp_timer_get_time() + US(timeout_ms > 0 ? timeout_ms : REQUEST_MS);
    /* Without a callback nobody waits for the answer, so it needs no slot. */
    pending_t *slot = NULL;
    for (int i = 0; cb && !slot && i < MAX_PENDING; i++) {
        if (!s_pending[i].id) {
            slot = &s_pending[i];
        }
    }
    if (cb && !slot) {
        cJSON_Delete(params);
        reply_local(cb, ctx, "busy", "too many requests waiting");
        return;
    }
    int id = s_next_id++;
    char *text = link_encode_req(id, method, params);
    if (!text || ((orb_link_supports_cards() || orb_link_supports_home() || orb_link_supports_weather()) && strlen(text) > EXT_MAX_MSG)) {
        cJSON_free(text);
        reply_local(cb, ctx, "internal", "request too large");
        return;
    }
    if (slot) {
        *slot = (pending_t){
            .id = id,
            .deadline = deadline,
            .cb = cb,
            .ctx = ctx,
        };
    }
    send_text_until(text, deadline);
}

static bool send_event(const char *name, cJSON *params)
{
    char *text = link_encode_evt(name, params);
    if (!text) {
        ESP_LOGW(TAG, "%s dropped: too large", name);
    }
    return send_text_until(text, 0);
}

/* ---- Power and state.report ---- */

static bool read_power(link_power_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    orb_link_power_cb_t cb = s_power_cb;
    void *ctx = s_power_ctx;
    taskEXIT_CRITICAL(&s_lock);
    orb_link_power_t p;
    if (!cb || cb(&p, ctx) != ESP_OK) {
        return false;
    }
    *out = (link_power_t){ .battery_pct = p.battery_pct, .battery_mv = p.battery_mv,
                           .charging = p.charging, .usb = p.usb,
                           .display_state = link_device_display_state_name(p.display_state) };
    return true;
}

/* Reads the power state; true if it differs from what the hub last heard. */
static bool refresh_power(void)
{
    link_power_t p = { 0 };
    bool have = read_power(&p);
    bool changed = have != s_have_power || (have && (p.battery_pct != s_power.battery_pct ||
                                                     p.charging != s_power.charging || p.usb != s_power.usb));
    s_have_power = have;
    s_power = p;
    return changed;
}

static void send_report(void)
{
    cJSON *p = link_state_report_params(s_have_power ? &s_power : NULL, rssi(),
                                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), uptime_s());
    if (p && link_radio_report(p) && link_crash_report(p)) {
        if (send_event("state.report", p)) { link_radio_report_sent(); link_crash_report_sent(); }
        else s_end = true;
        esp_websocket_client_set_ping_interval_sec(s_ws, link_radio_ping_s());
    } else { cJSON_Delete(p); s_end = true; }
    s_report_at = esp_timer_get_time() + US(REPORT_MS);
}

/* ---- What the device says about itself ---- */

static cJSON *hello_params(void)
{
    link_device_t dev;
    char build[8];
    link_device_info(&dev, build);
    refresh_power();
    cJSON *hello = link_hello_params(&dev, s_pair.token, s_have_power ? &s_power : NULL, rssi());
    if (!hello) {
        return NULL;
    }
    if (!link_radio_hello(hello)) { cJSON_Delete(hello); return NULL; }
    taskENTER_CRITICAL(&s_lock);
    bool voice_available = s_voice_available;
    taskEXIT_CRITICAL(&s_lock);
    if (!voice_available) cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(hello, "caps"), 1);
    if (orb_home_revision() &&
        !cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(hello, "caps"), cJSON_CreateString("home"))) {
        cJSON_Delete(hello);
        return NULL;
    }
    hello_handler_t hs[MAX_HANDLERS];
    taskENTER_CRITICAL(&s_lock);
    int n = s_n_hellos;
    memcpy(hs, s_hellos, sizeof(hs[0]) * n);
    taskEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < n; i++) {
        hs[i].cb(hello, hs[i].ctx);
    }
    return hello;
}

/* ---- Answers to our own requests ---- */

static const char *error_code(const cJSON *error)
{
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code"));
    return code ? code : "?";
}

static void on_pair_reveal(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx);

/* Pairing step 3: the hub has picked nonce_h without knowing nonce_d, so
 * nonce_d can be revealed now. */
static void on_pair_begin(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    if (err != ESP_OK || !link_read_pair_begin(result, &s_sess.begin)) {
        ESP_LOGW(TAG, "pair.begin failed: %s", err == ESP_FAIL ? error_code(error) : esp_err_to_name(err));
        s_end = true;
        return;
    }
    cJSON *params = link_pair_reveal_params(s_sess.nonce_d);
    if (!params) {
        s_end = true;   /* out of memory */
        return;
    }
    send_request("pair.reveal", params, REQUEST_MS, on_pair_reveal, NULL);
}

/* Pairing step 4 answered: the hub holds the request; show the code. */
static void on_pair_reveal(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pair.reveal failed: %s", err == ESP_FAIL ? error_code(error) : esp_err_to_name(err));
        s_end = true;
        return;
    }
    orb_link_status_t st = s_status;
    link_sas(sha256, s_sess.fp, s_sess.nonce_d, s_sess.begin.nonce_h, st.sas);
    memcpy(st.hub_name, s_sess.begin.hub_name, sizeof(st.hub_name));
    st.state = ORB_LINK_PAIRING;
    s_sess.pair_deadline = esp_timer_get_time() + US(PAIR_WAIT_MS);
    ESP_LOGI(TAG, "pairing with %s: confirm the code shown on Orb", st.hub_name);
    commit(&st);
}

/* No RTC: until SNTP answers, the hub's clock is the best there is. */
static void set_clock(int64_t time_ms)
{
    orb_net_status_t net;
    if (time_ms <= 0 || (orb_net_status(&net) == ESP_OK && net.time_synced)) {
        return;
    }
    const struct timeval tv = { .tv_sec = time_ms / 1000, .tv_usec = (time_ms % 1000) * 1000 };
    settimeofday(&tv, NULL);
}

static void on_hello(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hello failed: %s", err == ESP_FAIL ? error_code(error) : esp_err_to_name(err));
        /* The hub answered, but has forgotten us: pair again, right away. */
        s_sess.unauthorized = err == ESP_FAIL && strcmp(error_code(error), "unauthorized") == 0;
        s_sess.progressed = s_sess.unauthorized;
        s_end = true;
        orb_mem_end(ORB_MEM_HELLO);
        return;
    }
    link_hello_result_t r;
    link_read_hello(result, &r);
    link_radio_accept(result);
    ESP_LOGI(TAG, "online: %s (hub %s), %s on %s", r.hub_name, r.hub_version, r.device_name, r.channel);
    if (r.tz[0] || r.tz_posix[0]) {
        orb_net_set_timezone(r.tz[0] ? r.tz : NULL, r.tz_posix[0] ? r.tz_posix : NULL);
    }
    if (r.have_hour24) orb_time_set_clock_default(r.hour24);
    set_clock(r.time_ms);
    remember_address(&s_sess.hub);
    s_sess.progressed = true;

    int64_t now = esp_timer_get_time();
    s_report_at = now + US(REPORT_MS);
    s_power_at = now + US(POWER_POLL_MS);
    bool cards = false, voice = false, home = false, usage = false, devtools = false, nowplaying = false, weather = false;
    const cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "caps");
    if (caps && !cJSON_IsArray(caps)) {
        s_end = true;
        orb_mem_end(ORB_MEM_HELLO);
        return;
    }
    const cJSON *cap;
    cJSON_ArrayForEach(cap, caps) {
        const char *name = cJSON_GetStringValue(cap);
        if (name && !strcmp(name, "cards")) cards = true;
        if (name && !strcmp(name, "voice")) voice = true;
        if (name && !strcmp(name, "home")) home = true;
        if (name && !strcmp(name, "usage")) usage = true;
        if (name && !strcmp(name, "nowplaying")) nowplaying = true;
        if (name && !strcmp(name, "weather")) weather = true;
#if CONFIG_ORB_DEVTOOLS
        if (name && !strcmp(name, "devtools")) devtools = true;
#endif
    }
    orb_prefs_t prefs;
    if ((cards || home || weather) && !link_v11_read_prefs(cJSON_GetObjectItemCaseSensitive(result, "prefs"), &prefs)) {
        s_end = true;
        orb_mem_end(ORB_MEM_HELLO);
        return;
    }
    if (cards || home || weather) orb_model_set_prefs(&prefs);
    home = home && orb_home_revision() != 0;
    taskENTER_CRITICAL(&s_lock);
    s_cards = cards;
    s_devtools = devtools && !strcmp(r.channel, "dev");
    s_home = home;
    s_usage = usage;
    s_nowplaying = nowplaying;
    s_weather = weather;
    s_voice = cards && voice && s_voice_available;
    taskEXIT_CRITICAL(&s_lock);
    s_subscribe_pending = false;
    s_model_at = now;
    s_answer_at = now;
    orb_model_set_connection(true, false);
    orb_model_nowplaying_capability(nowplaying);
    orb_model_weather_capability(weather);
    orb_home_set_connection(home, false);
    orb_link_status_t st = s_status;
    st.state = ORB_LINK_ONLINE;
    memcpy(st.hub_name, r.hub_name, sizeof(st.hub_name));
    memcpy(st.device_name, r.device_name, sizeof(st.device_name));
    memcpy(st.channel, r.channel, sizeof(st.channel));
    commit(&st);
    orb_mem_end(ORB_MEM_HELLO);
}

/* ---- Reliable model delivery (link task only) ---- */

static bool s_prefs_pending, s_idle_pending;
static orb_idle_request_t s_idle_request;

static void on_prefs(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    (void)ctx;
    s_prefs_pending = false;
    orb_prefs_t prefs;
    if (err == ESP_OK && result && link_v11_read_prefs(result, &prefs)) {
        orb_model_complete_prefs(&prefs, NULL);
    } else {
        orb_model_complete_prefs(NULL, err == ESP_FAIL ? error_code(error) :
                                 err == ESP_ERR_TIMEOUT ? "timeout" : "bad_response");
        if (err == ESP_OK) s_end = true;
    }
}

static void on_idle(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    (void)ctx;
    s_idle_pending = false;
    if (err == ESP_OK && link_v11_apply_idle(result, &s_idle_request)) return;
    orb_model_complete_idle(&s_idle_request, NULL, 0, NULL,
                            err == ESP_FAIL ? error_code(error) :
                            err == ESP_ERR_TIMEOUT ? "timeout" : "bad_response");
    if (err == ESP_OK) s_end = true;
}

static void on_subscribe(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    (void)result;
    (void)error;
    (void)ctx;
    if (err != ESP_OK) s_end = true; /* A fresh hello is the recovery boundary. */
}

static void on_answer(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    (void)ctx;
    s_answer_pending = false;
    if (err == ESP_OK) {
        orb_answer_t recorded;
        bool delivered;
        if (!link_v11_read_answer(result, s_answer.card, &recorded, &delivered)) {
            /* Keep the original answer when acknowledgment is unusable. The
             * hub's durable first-answer record makes its retry safe. */
            s_end = true;
        } else {
            orb_model_complete_answer(s_answer.card, &recorded, delivered, NULL);
        }
    } else if (err == ESP_FAIL && strcmp(error_code(error), "busy") &&
               strcmp(error_code(error), "internal") && strcmp(error_code(error), "timeout")) {
        orb_model_complete_answer(s_answer.card, NULL, false, error_code(error));
    }
    s_answer_at = esp_timer_get_time() + US(1000);
}

static void on_message(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    (void)ctx;
    s_message_pending = false;
    orb_message_result_t receipt;
    if ((err == ESP_OK || err == ESP_FAIL) &&
        orb_link_read_message_receipt(err == ESP_OK ? result : NULL,
                                      err == ESP_FAIL ? error : NULL, &receipt)) {
        orb_model_complete_message_receipt(&s_message, receipt.status, receipt.reason, receipt.error);
    } else orb_model_complete_message_receipt(&s_message, ORB_SEND_FAILED,
        err == ESP_ERR_TIMEOUT ? "Delivery unconfirmed. Check the conversation." : "Message receipt unavailable.",
        err == ESP_ERR_TIMEOUT ? "timeout" : "internal");
}

static bool s_home_pending;
static orb_home_request_t s_home_request;
/* Home rejection diagnostics belong to the link task. Fixed labels and parser
 * reasons keep accessory names, provider content and credentials out of logs. */
static const char *const s_home_rejection_names[] = {
    "home.snapshot", "home.update", "home.set", "home.toggle", "home.scene", "home.other",
};
static uint32_t s_home_rejections[sizeof(s_home_rejection_names) / sizeof(s_home_rejection_names[0])];
static void reject_home(const char *name, const char *reason)
{
    size_t type = 0, last = sizeof(s_home_rejection_names) / sizeof(s_home_rejection_names[0]) - 1;
    while (type < last && strcmp(name, s_home_rejection_names[type])) type++;
    if (!s_home_rejections[type]) {
        ESP_LOGW(TAG, "rejected %s: %s", s_home_rejection_names[type], reason);
    }
    if (s_home_rejections[type] != UINT32_MAX) s_home_rejections[type]++;
    /* No Home resync method exists in v1.1. Wait for a fresh reset snapshot;
     * sessions, attention cards, voice and firmware delivery remain online. */
    orb_home_snapshot_discard();
    orb_home_set_connection(s_home, false);
}
static void on_home(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    (void)ctx;
    s_home_pending = false;
    orb_home_state_t accepted;
    bool ok = err == ESP_OK && link_home_read_reply(result, &s_home_request, &accepted);
    orb_home_complete(&s_home_request, ok ? &accepted : NULL, ok ? NULL :
        err == ESP_FAIL ? error_code(error) : err == ESP_ERR_TIMEOUT ? "timeout" : "invalid_reply");
    if (err == ESP_OK && !ok) reject_home(link_home_request_method(&s_home_request), link_home_error());
}

static void on_nowplaying(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    (void)ctx;
    s_media_pending = false;
    bool ok = err == ESP_OK && link_v11_apply_nowplaying_reply(result, &s_media_request);
    orb_model_complete_nowplaying(ok ? NULL : err == ESP_FAIL ? error_code(error) :
        err == ESP_ERR_TIMEOUT ? "timeout" : "bad_response");
    if (err == ESP_OK && !ok) s_end = true;
}

static void pump_model(int64_t now)
{
    s_model_at = now + US(link_radio_model_poll_ms());
    if (s_home && !s_home_pending && orb_home_take_request(&s_home_request)) {
        cJSON *params = link_home_request_params(&s_home_request);
        if (params) {
            s_home_pending = true;
            send_request(link_home_request_method(&s_home_request), params, REQUEST_MS, on_home, NULL);
        } else orb_home_complete(&s_home_request, NULL, "internal");
    }
    if (s_nowplaying && !s_media_pending && orb_model_take_nowplaying_request(&s_media_request)) {
        cJSON *params = link_v11_nowplaying_params(&s_media_request);
        if (params) {
            s_media_pending = true;
            send_request("nowplaying.control", params, REQUEST_MS, on_nowplaying, NULL);
        } else orb_model_complete_nowplaying("internal");
    }
    orb_prefs_patch_t prefs;
    if ((orb_link_supports_cards() || s_home || s_weather) && !s_prefs_pending && orb_model_take_prefs_request(&prefs)) {
        cJSON *params = link_v11_prefs_params(&prefs);
        if (params) {
            s_prefs_pending = true;
            send_request("prefs.set", params, REQUEST_MS, on_prefs, NULL);
        } else orb_model_complete_prefs(NULL, "internal");
    }
    if (!orb_link_supports_cards()) return;
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 0) orb_model_expire((uint64_t)tv.tv_sec);
    if (!s_subscribe_pending) {
        s_subscribe_pending = true;
        send_request("sessions.subscribe", NULL, REQUEST_MS, on_subscribe, NULL);
    }
    if (!s_idle_pending && orb_model_take_idle_request(&s_idle_request)) {
        cJSON *params = link_v11_idle_params(&s_idle_request);
        if (params) {
            s_idle_pending = true;
            send_request("sessions.list", params, REQUEST_MS, on_idle, NULL);
        } else orb_model_complete_idle(&s_idle_request, NULL, 0, NULL, "internal");
    }
    /* One immutable answer in flight. Timeout, queue loss and reconnect never
     * turn a later UI tap into a replacement decision for the same card. */
    if (!s_answer_pending && now >= s_answer_at && orb_model_peek_answer(&s_answer)) {
        cJSON *params = link_v11_answer_params(&s_answer);
        if (params) {
            s_answer_pending = true;
            send_request("card.answer", params, REQUEST_MS, on_answer, NULL);
        }
    }
    if (!s_message_pending && orb_model_take_message(&s_message)) {
        bool stale = s_message.stream && s_message.generation != orb_link_generation();
        cJSON *params = stale ? NULL : link_v11_message_params(s_message.target, s_message.text, s_message.stream);
        if (params) {
            s_message_pending = true;
            send_request("message.send", params, REQUEST_MS, on_message, NULL);
        } else orb_model_complete_message(&s_message, false, stale ? "expired" : "internal");
    }
}

/* ---- Messages from the hub ---- */

static void on_response(const link_msg_t *m)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        pending_t p = s_pending[i];
        if (p.id == 0 || p.id != m->id) {
            continue;
        }
        s_pending[i].id = 0;
        if (m->ok) {
            p.cb(ESP_OK, m->body, NULL, p.ctx);
        } else {
            p.cb(ESP_FAIL, NULL, m->body, p.ctx);
        }
        return;
    }
    /* Otherwise an answer that came after its timeout, or to a request with no callback. */
}

static const char *home_message_name(const link_msg_t *m)
{
    if (!s_home) return NULL;
    if (m->kind == LINK_EVT && m->name && !strncmp(m->name, "home.", 5)) return m->name;
    if (m->kind == LINK_RES) {
        for (int i = 0; i < MAX_PENDING; i++) {
            if (s_pending[i].id && s_pending[i].id == m->id && s_pending[i].cb == on_home) {
                return link_home_request_method(&s_home_request);
            }
        }
    }
    return NULL;
}

static const char *code_for(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG: return "bad_request";
    case ESP_ERR_NOT_FOUND: return "not_found";
    case ESP_ERR_INVALID_STATE: return "busy";
    default: return "internal";
    }
}

static void close_link(void);

static void on_request(const link_msg_t *m)
{
    method_handler_t h = { 0 };
    if (m->name) {
        taskENTER_CRITICAL(&s_lock);
        for (int i = 0; i < s_n_methods && !h.cb; i++) {
            if (strcmp(s_methods[i].name, m->name) == 0) {
                h = s_methods[i];
            }
        }
        taskEXIT_CRITICAL(&s_lock);
    }

    char *out;
    if (!m->name || !m->body) {
        out = link_encode_err(m->id, "bad_request", "malformed request");
    } else if (s_status.state != ORB_LINK_ONLINE) {
        out = link_encode_err(m->id, "busy", "not online yet");
    } else if (!strncmp(m->name, "devtools.", 9) && strcmp(s_status.channel, "dev")) {
        out = link_encode_err(m->id, "unauthorized", "developer channel required");
    } else if (!strncmp(m->name, "devtools.", 9) && !orb_link_supports_devtools()) {
        out = link_encode_err(m->id, "unsupported", "developer tools unavailable");
    } else if (!h.cb) {
        out = link_encode_err(m->id, "unknown_method", m->name);
    } else {
        cJSON *result = cJSON_CreateObject();
        esp_err_t err = result ? h.cb(m->body, result, h.ctx) : ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            if (!strcmp(m->name, "device.config")) {
                const char *channel = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m->body, "channel"));
                if (channel && strcmp(channel, s_status.channel)) {
                    taskENTER_CRITICAL(&s_lock);
                    s_devtools = false;
                    taskEXIT_CRITICAL(&s_lock);
                    orb_link_status_t st = s_status;
                    snprintf(st.channel, sizeof(st.channel), "%s", channel);
                    commit(&st);
                    /* on_request writes the response before the loop closes;
                     * access is already revoked, and re-enabling needs hello. */
                    s_end = true;
                }
            }
            out = link_encode_res(m->id, result);
            if (!out) {
                out = link_encode_err(m->id, "internal", "result too large");
            }
        } else {
            cJSON_Delete(result);
            out = link_encode_err(m->id, code_for(err), esp_err_to_name(err));
        }
    }
    send_text(out);

    if (s_sess.reboot) {
        /* A clean close puts the response on the wire before the restart. */
        ESP_LOGW(TAG, "rebooting, as the hub asked");
        close_link();
        esp_restart();
    }
}

static void on_pair_event(const char *name, const cJSON *params)
{
    if (strcmp(name, "pair.failed") == 0) {
        const char *reason = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "reason"));
        ESP_LOGW(TAG, "pairing %s", reason ? reason : "failed");
        /* Expired: show a fresh code straight away. Rejected: back off. */
        s_sess.progressed = reason && strcmp(reason, "expired") == 0;
        s_end = true;
    } else if (strcmp(name, "pair.done") == 0) {
        char token[65];
        if (!link_read_pair_done(params, token)) {
            ESP_LOGW(TAG, "pair.done without a valid token");
        } else if (save_pairing(token, s_sess.pem, &s_sess.hub) == ESP_OK) {
            install_pairing(token, s_sess.pem, s_sess.fp);
            set_address(s_sess.hub.ip, s_sess.hub.host, s_sess.hub.port);
            s_sess.pem = NULL;
            s_sess.progressed = true;
            ESP_LOGI(TAG, "paired with %s", s_status.hub_name);
        }
        s_end = true;   /* pairing step 7: close and reconnect normally */
    }
}

static void on_event(const link_msg_t *m)
{
    if (s_sess.pairing) {
        if (s_status.state == ORB_LINK_PAIRING) {
            on_pair_event(m->name, m->body);
        }
        return;
    }
    if (s_status.state != ORB_LINK_ONLINE) {
        return;
    }
    if (m->name && !strcmp(m->name, "power.batch")) {
        if (!link_radio_negotiated()) return;
        if (!link_batch_valid(m->body)) { s_end = true; return; }
        const cJSON *event;
        cJSON_ArrayForEach(event, cJSON_GetObjectItemCaseSensitive(m->body, "events")) {
            link_msg_t child = {.kind = LINK_EVT,
                .name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "m")),
                .body = cJSON_GetObjectItemCaseSensitive(event, "p")};
            on_event(&child);
            if (s_end) break;
        }
        return;
    }
    if (m->name && !strncmp(m->name, "home.", 5)) {
        if (!s_home) return;
        link_v11_result_t mapped = link_home_event(m->name, m->body);
        if (mapped == LINK_V11_INVALID) {
            reject_home(m->name, link_home_error());
        }
        return;
    }
    if (m->name && !strcmp(m->name, "weather.state")) {
        if (s_weather && link_v11_event(m->name, m->body) == LINK_V11_INVALID) s_end = true;
        return;
    }
    if (m->name && !strcmp(m->name, "nowplaying.snapshot")) {
        if (s_nowplaying && link_v11_event(m->name, m->body) == LINK_V11_INVALID) s_end = true;
        return;
    }
    if (m->name && !strcmp(m->name, "usage.snapshot") && !s_usage) return;
    if (orb_link_supports_cards() || (s_usage && m->name && !strcmp(m->name, "usage.snapshot")) ||
        ((s_home || s_weather) && m->name && !strcmp(m->name, "prefs.update"))) {
        link_v11_result_t mapped = link_v11_event(m->name, m->body);
        if (mapped == LINK_V11_RESYNC) {
            /* A missed coalesced stream replacement is repaired in-band. Do
             * not tear down a capture or replay a consequential send. */
            s_subscribe_pending = false;
            return;
        }
        if (mapped == LINK_V11_INVALID) {
            ESP_LOGW(TAG, "invalid model event; reconnecting for fresh state");
            s_end = true;
            return;
        }
    }
    struct {
        orb_link_event_cb_t cb;
        void *ctx;
    } hs[MAX_HANDLERS];
    int n = 0;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_n_events; i++) {
        if (strcmp(s_events[i].name, m->name) == 0) {
            hs[n].cb = s_events[i].cb;
            hs[n++].ctx = s_events[i].ctx;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    /* No handler: an unknown event, ignored as the protocol says. */
    for (int i = 0; i < n; i++) {
        hs[i].cb(m->name, m->body, hs[i].ctx);
    }
}

static void on_text(const char *text, size_t len)
{
    link_msg_t m;
    orb_mem_begin(ORB_MEM_MESSAGE_PARSE);
    /* Incoming extension messages use one presized PSRAM workspace. Bracket
     * only decoding: callbacks can queue JSON with independent lifetimes. */
    bool pooled = len <= EXT_MAX_MSG;
    if (pooled && !orb_link_memory_begin_parse()) {
        orb_mem_end(ORB_MEM_MESSAGE_PARSE);
        orb_model_set_connection(false, false);
        s_end = true;
        return;
    }
    cJSON *root = link_parse_dispatch(text, len, &m);
    if (pooled) orb_link_memory_end_parse();
    if (root && m.kind == LINK_EVT && m.name && !strncmp(m.name, "home.", 5) &&
        (!s_home || (strcmp(m.name, "home.snapshot") && strcmp(m.name, "home.update")))) {
        /* Optional and future Home events are ignored even when their body is
         * unknown. They cannot change the negotiated model or transport. */
        cJSON_Delete(root);
        orb_mem_end(ORB_MEM_MESSAGE_PARSE);
        return;
    }
    const char *home = root ? home_message_name(&m) : NULL;
    if (home && (len > EXT_MAX_MSG || m.invalid)) {
        /* Decode within the legacy transport bound only to identify Home.
         * Its model limits must never take the shared pinned link down. */
        if (m.kind == LINK_RES) {
            for (int i = 0; i < MAX_PENDING; i++) {
                if (s_pending[i].id == m.id && s_pending[i].cb == on_home) s_pending[i].id = 0;
            }
            s_home_pending = false;
            orb_home_complete(&s_home_request, NULL, "invalid_reply");
        }
        reject_home(home, len > EXT_MAX_MSG ? "envelope exceeds 4096 bytes" : "invalid application body");
        cJSON_Delete(root);
        orb_mem_end(ORB_MEM_MESSAGE_PARSE);
        return;
    }
    if ((orb_link_supports_cards() || orb_link_supports_home() || orb_link_supports_weather() ||
         (link_radio_negotiated() && root && m.kind == LINK_EVT && m.name && !strcmp(m.name, "power.batch"))) && len > EXT_MAX_MSG) {
        taskENTER_CRITICAL(&s_lock);
        s_rx_too_large = true;
        taskEXIT_CRITICAL(&s_lock);
        s_end = true;
        cJSON_Delete(root);
        orb_mem_end(ORB_MEM_MESSAGE_PARSE);
        return;
    }
    if (!root || m.invalid) {
        cJSON_Delete(root);
        orb_mem_end(ORB_MEM_MESSAGE_PARSE);
        ESP_LOGW(TAG, "dropped a message that isn't a valid envelope");
        if (orb_link_supports_cards() || orb_link_supports_home() || orb_link_supports_weather()) {
            /* An undecodable delta cannot leave the model advertised current.
             * Legacy keeps its original drop semantics; v1.1 resynchronizes. */
            orb_model_set_connection(false, false);
            orb_home_set_connection(false, false);
            s_end = true;
        }
        return;
    }
    /* Classify only after decoding, then include the DOM peak in that phase.
     * No substring guesses or session/card content enter the diagnostics. */
    orb_mem_phase_t phase = ORB_MEM_MESSAGE_PARSE;
    if (m.kind == LINK_EVT && m.name) {
        if (!strcmp(m.name, "sessions.snapshot")) phase = ORB_MEM_SNAPSHOT_PARSE;
        else if (!strcmp(m.name, "card.show")) phase = ORB_MEM_CARD_SHOW;
    }
    switch (m.kind) {
    case LINK_REQ:
        on_request(&m);
        break;
    case LINK_RES:
        on_response(&m);
        break;
    case LINK_EVT:
        on_event(&m);
        break;
    }
    cJSON_Delete(root);
    orb_mem_end_as(ORB_MEM_MESSAGE_PARSE, phase);
}

/* ---- The WebSocket task's side ---- */

static void observe_ws(uint32_t conn, link_control_event_t event)
{
    taskENTER_CRITICAL(&s_lock);
    bool current = link_control_observe(&s_control, conn, event);
    taskEXIT_CRITICAL(&s_lock);
    if (current) {
        /* A full queue already has work to wake the consumer. Never wait here:
         * the WebSocket task may hold the send lock needed by the link task. */
        msg_t wake = { .kind = MSG_WS_STATE, .conn = conn };
        xQueueSend(s_queue, &wake, 0);
    }
}

static void sync_ws_state(void)
{
    taskENTER_CRITICAL(&s_lock);
    link_control_t state = s_control;
    taskEXIT_CRITICAL(&s_lock);
    if (state.conn != s_conn) return;
    s_peer_closed = state.ended;
    if (link_control_should_end(&state, s_handled_frame)) {
        if (state.lost_frame && !s_end) ESP_LOGW(TAG, "lost an application frame; reconnecting for fresh state");
        s_up = false;
        s_end = true;
    } else if (!s_end && state.opened) s_up = true;
}

static void post_text(msg_t *m)
{
    taskENTER_CRITICAL(&s_lock);
    bool current = link_control_accept_frame(&s_control, m->conn, &m->sequence);
    taskEXIT_CRITICAL(&s_lock);
    if (!current) {
        orb_link_memory_rx_free(m->text);
        return;
    }
    link_radio_rx(m->len);
    if (xQueueSend(s_queue, m, 0) != pdTRUE) {
        /* Continuing after a missing state delta would leave a plausible but
         * stale UI. Reconnect through hello instead of silently dropping it. */
        orb_link_memory_rx_free(m->text);
        observe_ws(m->conn, LINK_CONTROL_LOST_FRAME);
    }
}

/* Collect fragmented text without blocking the WebSocket task. Home gets only
 * the bounded legacy text buffer for classification: on_text rejects its
 * over-limit application envelopes locally. Binary budgets remain strict. */
static void on_ws_data(uint32_t conn, const esp_websocket_event_data_t *d)
{
    taskENTER_CRITICAL(&s_lock);
    bool current = s_control.conn == conn && !s_control.ended;
    taskEXIT_CRITICAL(&s_lock);
    if (!current) return; /* A stale fragment must not alter the current assembler. */
    bool home = orb_link_supports_home();
    bool extension = home || orb_link_supports_cards() || orb_link_supports_weather();
    bool continuation = d->op_code == WS_TRANSPORT_OPCODES_CONT;
    bool binary = d->op_code == WS_TRANSPORT_OPCODES_BINARY || (continuation && s_rx_binary_open);
    size_t ceiling = extension && (!home || binary) ? EXT_MAX_MSG : LINK_MAX_MSG;
    bool audio = binary && orb_link_supports_voice();
    if (audio) ceiling = 1031;
    if (d->payload_len < 0 || d->payload_offset < 0 || d->data_len < 0) {
        observe_ws(conn, LINK_CONTROL_LOST_FRAME);
        return;
    }
    if ((extension || audio) && (size_t)d->payload_len > ceiling) {
        taskENTER_CRITICAL(&s_lock);
        s_rx_too_large = true;
        taskEXIT_CRITICAL(&s_lock);
        observe_ws(conn, LINK_CONTROL_LOST_FRAME);
        return;
    }
    if ((d->op_code == WS_TRANSPORT_OPCODES_TEXT || d->op_code == WS_TRANSPORT_OPCODES_BINARY) &&
        d->payload_offset == 0) {
        if (s_rx_open || s_rx_binary_open) {
            /* WebSocket data messages cannot interleave. Never append a new
             * binary message's continuation to unfinished JSON text. */
            observe_ws(conn, LINK_CONTROL_LOST_FRAME);
            return;
        }
        if (binary) {
            s_rx_binary_open = true;
            s_rx_binary_len = 0;
        }
    }
    if (binary) {
        if (!s_rx_binary_open) {
            observe_ws(conn, LINK_CONTROL_LOST_FRAME);
            return;
        }
        /* Audio uses the same ordered queue as its begin/end events. Never
         * call playback from the WebSocket task or bypass frame-loss recovery. */
        size_t n = (size_t)d->data_len;
        if ((extension || audio) && n > ceiling - s_rx_binary_len) {
            taskENTER_CRITICAL(&s_lock);
            s_rx_too_large = true;
            taskEXIT_CRITICAL(&s_lock);
            observe_ws(conn, LINK_CONTROL_LOST_FRAME);
            return;
        }
        if (audio && n) {
            char *grown = orb_memory_realloc(ORB_MEMORY_NETWORK, s_rx, s_rx_binary_len + n);
            if (!grown) {
                orb_memory_free(s_rx); s_rx = NULL;
                observe_ws(conn, LINK_CONTROL_LOST_FRAME);
                return;
            }
            s_rx = grown;
            memcpy(s_rx + s_rx_binary_len, d->data_ptr, n);
        }
        if (extension || audio) s_rx_binary_len += n;
        if (d->fin && (size_t)d->payload_offset + n >= (size_t)d->payload_len) {
            if (audio && s_rx) {
                msg_t m = {.kind = MSG_WS_BINARY, .conn = conn, .text = s_rx, .len = s_rx_binary_len};
                s_rx = NULL;
                post_text(&m);
            }
            s_rx_binary_open = false;
            s_rx_binary_len = 0;
        }
        return;
    }
    if (d->op_code == WS_TRANSPORT_OPCODES_TEXT && d->payload_offset == 0) {
        orb_link_memory_rx_free(s_rx);
        s_rx = NULL;
        s_rx_len = 0;
        s_rx_open = true;
        s_rx_skip = false;
    } else if (d->op_code != WS_TRANSPORT_OPCODES_TEXT && !continuation) {
        return;
    } else if (!s_rx_open || s_rx_binary_open) {
        observe_ws(conn, LINK_CONTROL_LOST_FRAME);
        return;
    }

    if (!s_rx_skip && d->data_len > 0) {
        bool oversized = s_rx_len + d->data_len > ceiling;
        char *grown = !oversized ? orb_link_memory_rx_resize(s_rx, s_rx_len + d->data_len + 1) : NULL;
        if (grown) {
            memcpy(grown + s_rx_len, d->data_ptr, d->data_len);
            s_rx = grown;
            s_rx_len += d->data_len;
        } else {
            if (oversized) {
                ESP_LOGW(TAG, "message exceeds negotiated budget");
                if (home || ceiling == EXT_MAX_MSG) {
                    taskENTER_CRITICAL(&s_lock);
                    s_rx_too_large = true;
                    taskEXIT_CRITICAL(&s_lock);
                    observe_ws(conn, LINK_CONTROL_LOST_FRAME);
                }
            }
            else {
                ESP_LOGW(TAG, "out of memory receiving an application frame");
                observe_ws(conn, LINK_CONTROL_LOST_FRAME);
            }
            orb_link_memory_rx_free(s_rx);
            s_rx = NULL;
            s_rx_skip = true;
        }
    }

    if (d->fin && (size_t)d->payload_offset + (size_t)d->data_len >= (size_t)d->payload_len) {
        if (!s_rx_skip && s_rx) {
            s_rx[s_rx_len] = '\0';
            msg_t m = { .kind = MSG_WS_TEXT, .conn = conn, .text = s_rx, .len = s_rx_len };
            post_text(&m);
        }
        s_rx = NULL;
        s_rx_open = false;
    }
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    uint32_t conn = (uint32_t)(uintptr_t)arg;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        observe_ws(conn, LINK_CONTROL_UP);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        observe_ws(conn, LINK_CONTROL_DOWN);
        break;
    case WEBSOCKET_EVENT_DATA:
        on_ws_data(conn, data);
        break;
    default:
        break;
    }
}

/* ---- The link task ---- */

static void fail_requests(void)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        pending_t p = s_pending[i];
        if (p.id) {
            s_pending[i].id = 0;
            p.cb(ESP_ERR_TIMEOUT, NULL, NULL, p.ctx);
        }
    }
}

static void handle(msg_t *m)
{
    bool current = m->conn == s_conn;
    switch (m->kind) {
    case MSG_WS_STATE:
        break; /* step() already consumed the authoritative control snapshot. */
    case MSG_WS_TEXT:
    case MSG_WS_BINARY:
        if (current && s_up) {
            if (m->kind == MSG_WS_TEXT) on_text(m->text, m->len);
            else if (orb_link_supports_voice()) {
                taskENTER_CRITICAL(&s_lock);
                orb_link_audio_cb_t cb = s_audio_cb;
                void *ctx = s_audio_ctx;
                taskEXIT_CRITICAL(&s_lock);
                if (cb) cb((const uint8_t *)m->text, m->len, m->conn, ctx);
            }
        }
        if (current) s_handled_frame = m->sequence;
        orb_link_memory_rx_free(m->text);
        break;
    case MSG_REQUEST:
        /* Made while online on this connection, or it's stale. */
        if (current && s_up && !s_peer_closed && !s_end && s_status.state == ORB_LINK_ONLINE) {
            int64_t remaining = m->deadline - esp_timer_get_time();
            if (remaining > 0) send_request(m->name, m->body, (int)((remaining + 999) / 1000), m->cb, m->ctx);
            else {
                cJSON_Delete(m->body);
                if (m->cb) m->cb(ESP_ERR_TIMEOUT, NULL, NULL, m->ctx);
            }
        } else {
            cJSON_Delete(m->body);
            if (m->cb) {
                m->cb(ESP_ERR_TIMEOUT, NULL, NULL, m->ctx);
            }
        }
        break;
    case MSG_EVENT: {
        bool developer = !strncmp(m->name, "devtools.", 9);
        bool sent = false;
        if (current && s_up && !s_peer_closed && !s_end && s_status.state == ORB_LINK_ONLINE &&
            (!developer || orb_link_supports_devtools()) &&
            (!m->cb || esp_timer_get_time() < m->deadline)) {
            if (m->cb) {
                char *text = link_encode_evt(m->name, m->body);
                if (text && strlen(text) <= EXT_MAX_MSG) {
                    sent = send_text_until(text, m->deadline);
                } else cJSON_free(text);
            } else send_event(m->name, m->body);
        } else cJSON_Delete(m->body);
        if (m->cb) m->cb(sent ? ESP_OK : ESP_ERR_TIMEOUT, NULL, NULL, m->ctx);
        break;
    }
    case MSG_AUDIO:
        if (current && s_up && !s_peer_closed && !s_end && orb_link_supports_voice()) {
            if (esp_websocket_client_send_bin(s_ws, m->text, m->len, pdMS_TO_TICKS(SEND_MS)) != (int)m->len)
                s_end = true; /* Never silently skip a sample packet. */
            else link_radio_tx(m->len);
        }
        orb_memory_free(m->text);
        break;
    case MSG_UNPAIR:
        /* An open connection may still be using the certificate. */
        if (s_ws) {
            s_unpair = true;
            s_end = true;
        } else {
            forget_pairing();
        }
        break;
    }
}

static int64_t next_deadline(int64_t until)
{
    int64_t t = until;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (s_pending[i].id && s_pending[i].deadline < t) {
            t = s_pending[i].deadline;
        }
    }
    if (s_sess.pair_deadline && s_sess.pair_deadline < t) {
        t = s_sess.pair_deadline;
    }
    if (s_status.state == ORB_LINK_ONLINE) {
        t = s_report_at < t ? s_report_at : t;
        t = s_power_at < t ? s_power_at : t;
        t = s_model_at < t ? s_model_at : t;
    }
    return t;
}

static void run_timers(int64_t now)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        pending_t p = s_pending[i];
        if (p.id && now >= p.deadline) {
            s_pending[i].id = 0;
            p.cb(ESP_ERR_TIMEOUT, NULL, NULL, p.ctx);
        }
    }
    if (s_sess.pair_deadline && now >= s_sess.pair_deadline) {
        /* The hub has expired it by now: start over with a fresh code. */
        ESP_LOGW(TAG, "pairing wasn't confirmed in time");
        s_sess.pair_deadline = 0;
        s_sess.progressed = true;
        s_end = true;
    }
    if (s_status.state == ORB_LINK_ONLINE) {
        if (link_radio_changed()) send_report();
        if (link_radio_model_prompt() || now >= s_model_at) pump_model(now);
        if (now >= s_power_at) {
            s_power_at = now + US(POWER_POLL_MS);
            if (refresh_power()) {
                send_report();
            }
        }
        if (now >= s_report_at) {
            refresh_power();
            send_report();
        }
    }
}

/* Handles the timers that are due, then one message or the next timer,
 * whichever comes first, waiting no later than `until`. */
static void step(int64_t until)
{
    sync_ws_state();
    int64_t now = esp_timer_get_time();
    if (!s_end && !s_peer_closed) run_timers(now);
    int64_t ms = (next_deadline(until) - now + 999) / 1000;
    ms = ms < 0 ? 0 : ms > MAX_SLEEP_MS ? MAX_SLEEP_MS : ms;
    msg_t m;
    if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(ms)) == pdTRUE) {
        sync_ws_state();
        handle(&m);
        sync_ws_state();
    }
}

static void wait_ms(uint32_t ms)
{
    int64_t until = esp_timer_get_time() + US(ms);
    while (esp_timer_get_time() < until) {
        step(until);
    }
}

/* OFFLINE until orb_net has an address. */
static void wait_for_network(void)
{
    orb_net_status_t net;
    while (orb_net_status(&net) != ESP_OK || net.state != ORB_NET_CONNECTED) {
        set_state(ORB_LINK_OFFLINE);
        wait_ms(NET_POLL_MS);
    }
}

/* A paired device takes the hub whose TXT fp matches its pinned certificate,
 * else the last address that worked; an unpaired one takes the first hub. */
static bool find_hub(orb_hub_addr_t *out)
{
    orb_hub_addr_t hubs[MAX_HUBS];
    int n = 0;
    bool paired = s_pair.token[0];
    esp_err_t err = orb_net_find_hubs(hubs, MAX_HUBS, &n, DISCOVERY_MS);
    if (err == ESP_OK) {
        for (int i = 0; i < n; i++) {
            if (!paired || fp_matches(s_pair.fp, hubs[i].fp)) {
                *out = hubs[i];
                return true;
            }
        }
        ESP_LOGW(TAG, "%d hub(s) answered; none has our pinned certificate", n);
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "discovery failed: %s", esp_err_to_name(err));
    }
    if (paired && s_pair.ip[0]) {
        *out = (orb_hub_addr_t){ .port = s_pair.port };
        memcpy(out->ip, s_pair.ip, sizeof(out->ip));
        memcpy(out->host, s_pair.host, sizeof(out->host));
        return true;
    }
    return false;
}

/* Pairing step 1: the PEM from http://<hub>:<pair port>/v1/hub-cert. */
static char *fetch_cert(const orb_hub_addr_t *hub)
{
    char url[64];
    snprintf(url, sizeof(url), "http://%s:%u/v1/hub-cert", hub->ip, hub->pair_port ? hub->pair_port : PAIR_PORT);
    const esp_http_client_config_t cfg = { .url = url, .timeout_ms = CERT_FETCH_MS, .disable_auto_redirect = true };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        return NULL;
    }
    char *pem = NULL;
    int len = 0, r = 0;
    if (esp_http_client_open(http, 0) == ESP_OK && esp_http_client_fetch_headers(http) >= 0 &&
        esp_http_client_get_status_code(http) == 200) {
        /* Reads one byte past the cap, so an oversized body shows up as len > MAX_PEM. */
        pem = orb_memory_alloc(ORB_MEMORY_NETWORK, MAX_PEM + 2);
        while (pem && len <= MAX_PEM && (r = esp_http_client_read(http, pem + len, MAX_PEM + 1 - len)) > 0) {
            len += r;
        }
    }
    esp_http_client_cleanup(http);
    if (!pem || r < 0 || len == 0 || len > MAX_PEM) {
        ESP_LOGW(TAG, "no certificate from %s", url);
        orb_memory_free(pem);
        return NULL;
    }
    pem[len] = '\0';
    return pem;
}

/* Opens wss://<hub>/v1/link trusting only `pem`; true once it's up. */
static bool open_link(const orb_hub_addr_t *hub, const char *pem)
{
    orb_mem_begin(ORB_MEM_LINK_CONNECT);
    char uri[48];
    snprintf(uri, sizeof(uri), "wss://%s:%u/v1/link", hub->ip, hub->port);
    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .cert_pem = pem,
        /* The hub's SRV host is the orb-hub-*.local name in its certificate,
         * which stays when the Mac is renamed (docs/protocol.md, "Discovery"),
         * while the IPs in it go stale with a new DHCP lease. */
        .cert_common_name = hub->host[0] ? hub->host : NULL,
        /* Reconnects go through discovery and the backoff here instead. */
        .disable_auto_reconnect = true,
        .task_prio = WS_PRIORITY,
        .task_stack = WS_STACK,
        .task_core_id_set = true,
        .task_core_id = LINK_CORE,
        /* The managed WebSocket client has no allocator/caps option. Its RX/TX
         * mallocs are 4096 bytes, above ALWAYSINTERNAL=1024, so IDF prefers
         * PSRAM. Our fragment buffer and cJSON allocations are strict PSRAM;
         * mbedTLS is strict PSRAM via MBEDTLS_EXTERNAL_MEM_ALLOC. */
        .buffer_size = WS_BUFFER,
        .ping_interval_sec = PING_S,
        .pingpong_timeout_sec = PONG_TIMEOUT_S,
        .network_timeout_ms = NETWORK_MS,
    };
    taskENTER_CRITICAL(&s_lock);
    s_conn++;
    s_cards = s_voice = s_home = s_usage = s_nowplaying = s_weather = s_devtools = s_rx_too_large = false;
    s_media_pending = false;
    s_control = (link_control_t){ .conn = s_conn };
    taskEXIT_CRITICAL(&s_lock);
    link_v11_reset();
    link_home_reset();
    s_home_pending = false;
    s_up = false;
    s_peer_closed = false;
    s_handled_frame = 0;
    s_end = false;
    s_next_id = 1;
    /* The client keeps pointers to the PEM and the host name; both outlive it. */
    /* The public client API exposes connect completion after TLS + HTTP
     * upgrade, so this window deliberately includes both and client setup. */
    orb_mem_begin(ORB_MEM_TLS_HANDSHAKE);
    s_ws = esp_websocket_client_init(&cfg);
    link_radio_reset();
    link_radio_connect();
    if (!s_ws ||
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws_event, (void *)(uintptr_t)s_conn) != ESP_OK ||
        esp_websocket_client_start(s_ws) != ESP_OK) {
        ESP_LOGE(TAG, "couldn't start the WebSocket client");
        orb_mem_end(ORB_MEM_TLS_HANDSHAKE);
        orb_mem_end(ORB_MEM_LINK_CONNECT);
        return false;
    }
    int64_t deadline = esp_timer_get_time() + US(CONNECT_MS);
    while (!s_up && !s_end && esp_timer_get_time() < deadline) {
        step(deadline);
    }
    if (!s_up) {
        ESP_LOGW(TAG, "couldn't reach %s", uri);
    }
    orb_mem_end(ORB_MEM_TLS_HANDSHAKE);
    orb_mem_end(ORB_MEM_LINK_CONNECT);
    return s_up && !s_end;
}

static void close_link(void)
{
    if (s_ws) {
        if (esp_websocket_client_is_connected(s_ws)) {
            taskENTER_CRITICAL(&s_lock);
            bool too_large = s_rx_too_large;
            taskEXIT_CRITICAL(&s_lock);
            if (too_large) esp_websocket_client_close_with_code(s_ws, 1009, NULL, 0, pdMS_TO_TICKS(CLOSE_MS));
            else esp_websocket_client_close(s_ws, pdMS_TO_TICKS(CLOSE_MS));
        }
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    /* The WebSocket task is gone, so its half-received message is ours to free. */
    orb_link_memory_rx_free(s_rx);
    s_rx = NULL;
    s_rx_open = false;
    s_rx_binary_open = false;
    s_rx_binary_len = 0;
    s_up = false;
    taskENTER_CRITICAL(&s_lock);
    s_cards = s_voice = s_home = s_usage = s_nowplaying = s_weather = s_devtools = false;
    s_media_pending = false;
    taskEXIT_CRITICAL(&s_lock);
    link_v11_reset();
    link_home_reset();
    s_home_pending = false;
    s_sess.pair_deadline = 0;
    fail_requests();
}

static void run_pairing(void)
{
    s_sess.pairing = true;
    char *fetched = fetch_cert(&s_sess.hub);
    if (!fetched) {
        return;
    }
    s_sess.pem = pin_cert(fetched, s_sess.fp);
    orb_memory_free(fetched);
    if (!s_sess.pem) {
        ESP_LOGW(TAG, "%s didn't serve exactly one certificate", s_sess.hub.ip);
        return;
    }
    /* The TXT record and the certificate both came unauthenticated; the code
     * is what defeats a man in the middle. This only catches a mix-up. */
    if (s_sess.hub.fp[0] && !fp_matches(s_sess.fp, s_sess.hub.fp)) {
        ESP_LOGW(TAG, "the certificate from %s doesn't match its advertised fingerprint", s_sess.hub.ip);
        return;
    }
    if (!open_link(&s_sess.hub, s_sess.pem)) {
        return;
    }
    link_device_t dev;
    char build[8];
    link_device_info(&dev, build);
    esp_fill_random(s_sess.nonce_d, sizeof(s_sess.nonce_d));
    uint8_t commit_d[LINK_COMMIT_LEN];
    link_commit(sha256, s_sess.nonce_d, commit_d);
    cJSON *params = link_pair_begin_params(&dev, commit_d);
    if (!params) {
        return;
    }
    send_request("pair.begin", params, REQUEST_MS, on_pair_begin, NULL);
    while (!s_end) {
        step(INT64_MAX);
    }
}

static void run_hello(void)
{
    set_state(ORB_LINK_CONNECTING);
    if (!open_link(&s_sess.hub, s_pair.pem)) {
        return;
    }
    /* Never an empty hello: the hub would answer unauthorized and we'd unpair. */
    orb_mem_begin(ORB_MEM_HELLO);
    cJSON *params = hello_params();
    if (!params) {
        orb_mem_end(ORB_MEM_HELLO);
        return;
    }
    send_request("hello", params, REQUEST_MS, on_hello, NULL);
    while (!s_end) {
        step(INT64_MAX);
    }
}

static void link_task(void *arg)
{
    unsigned waits = 0;
    for (;;) {
        wait_for_network();
        memset(&s_sess, 0, sizeof(s_sess));
        set_state(ORB_LINK_SEARCHING);
        if (find_hub(&s_sess.hub)) {
            if (s_pair.token[0]) {
                run_hello();
            } else {
                run_pairing();
            }
        }
        close_link();
        orb_memory_free(s_sess.pem);

        bool unpaired = s_unpair || s_sess.unauthorized;
        if (unpaired) {
            forget_pairing();
        }
        set_state(s_sess.unauthorized ? ORB_LINK_UNAUTHORIZED : ORB_LINK_SEARCHING);
        s_unpair = false;
        if (s_sess.progressed || unpaired) {
            waits = 0;
        }
        uint32_t delay = link_radio_wait_begin(waits, esp_random());
        if (waits < UINT32_MAX) waits++;
        int64_t until = esp_timer_get_time() + US(delay);
        while (esp_timer_get_time() < until && !link_radio_wait_prompt()) {
            until = link_radio_wait_until(until, esp_timer_get_time());
            step(until);
        }
        link_radio_wait_end();
    }
}

/* ---- Built-in methods ---- */

static esp_err_t method_ping(const cJSON *params, cJSON *result, void *ctx)
{
    return cJSON_AddNumberToObject(result, "uptime_s", uptime_s()) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Only marks it: on_request restarts once the response has gone out. */
static esp_err_t method_reboot(const cJSON *params, cJSON *result, void *ctx)
{
    s_sess.reboot = true;
    return ESP_OK;
}

/* ---- API ---- */

static void radio_wake(void)
{
    msg_t wake = {.kind = MSG_WS_STATE};
    xQueueSend(s_queue, &wake, 0);
}

esp_err_t orb_link_start(orb_link_state_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(!s_task, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "no memory for the queue");
    link_radio_init(radio_wake);
    /* Before the callback is set: callbacks only ever run on the link task. */
    load_pairing();
    s_state_cb = cb;
    s_state_ctx = ctx;
    if (xTaskCreatePinnedToCore(link_task, "orb_link", LINK_STACK, NULL, LINK_PRIORITY, &s_task, LINK_CORE) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t orb_link_status(orb_link_status_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t orb_link_on_event(const char *name, orb_link_event_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(name && cb && strlen(name) < NAME_LEN, ESP_ERR_INVALID_ARG, TAG, "bad event handler");
    esp_err_t err = ESP_ERR_NO_MEM;
    taskENTER_CRITICAL(&s_lock);
    if (s_n_events < MAX_HANDLERS) {
        event_handler_t *h = &s_events[s_n_events++];
        strcpy(h->name, name);
        h->cb = cb;
        h->ctx = ctx;
        err = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t orb_link_on_method(const char *method, orb_link_method_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(method && cb && strlen(method) < NAME_LEN, ESP_ERR_INVALID_ARG, TAG, "bad method handler");
    esp_err_t err = ESP_ERR_NO_MEM;
    taskENTER_CRITICAL(&s_lock);
    method_handler_t *h = NULL;
    for (int i = 0; i < s_n_methods && !h; i++) {
        if (strcmp(s_methods[i].name, method) == 0) {
            h = &s_methods[i];
        }
    }
    if (!h && s_n_methods < MAX_METHOD_HANDLERS + BUILTIN_METHODS) {
        h = &s_methods[s_n_methods++];
        strcpy(h->name, method);
    }
    if (h) {
        h->cb = cb;
        h->ctx = ctx;
        err = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t orb_link_on_hello(orb_link_hello_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "cb is NULL");
    esp_err_t err = ESP_ERR_NO_MEM;
    taskENTER_CRITICAL(&s_lock);
    if (s_n_hellos < MAX_HANDLERS) {
        s_hellos[s_n_hellos++] = (hello_handler_t){ cb, ctx };
        err = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t orb_link_set_power_source(orb_link_power_cb_t cb, void *ctx)
{
    taskENTER_CRITICAL(&s_lock);
    s_power_cb = cb;
    s_power_ctx = ctx;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

/* Queues a request or event made while online; the link task drops it if
 * that connection is gone by then. */
static esp_err_t post_outgoing(msg_kind_t kind, uint32_t generation, const char *name, cJSON *body, int timeout_ms,
                               int64_t deadline_us, orb_link_reply_cb_t cb, void *ctx)
{
    esp_err_t err = ESP_OK;
    taskENTER_CRITICAL(&s_lock);
    uint32_t conn = s_conn;
    bool online = s_status.state == ORB_LINK_ONLINE && (!generation || conn == generation);
    taskEXIT_CRITICAL(&s_lock);
    if (!name || strlen(name) >= NAME_LEN) {
        err = ESP_ERR_INVALID_ARG;
    } else if (!online) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        msg_t m = { .kind = kind, .conn = conn, .body = body, .timeout_ms = timeout_ms, .cb = cb, .ctx = ctx,
                    .deadline = esp_timer_get_time() + US(timeout_ms > 0 ? timeout_ms : REQUEST_MS) };
        /* A receipt can share one deadline across several fragments. Preserve
         * it in the queue so transport contention cannot extend that budget. */
        if (deadline_us && deadline_us < m.deadline) m.deadline = deadline_us;
        strcpy(m.name, name);
        if (esp_timer_get_time() >= m.deadline) err = ESP_ERR_TIMEOUT;
        else err = xQueueSend(s_queue, &m, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        cJSON_Delete(body);
    }
    return err;
}

esp_err_t orb_link_request(const char *method, cJSON *params, int timeout_ms, orb_link_reply_cb_t cb, void *ctx)
{
    return post_outgoing(MSG_REQUEST, 0, method, params, timeout_ms, 0, cb, ctx);
}

esp_err_t orb_link_request_on(uint32_t generation, const char *method, cJSON *params,
                             int timeout_ms, orb_link_reply_cb_t cb, void *ctx)
{
    if (!generation) { cJSON_Delete(params); return ESP_ERR_INVALID_ARG; }
    return post_outgoing(MSG_REQUEST, generation, method, params, timeout_ms, 0, cb, ctx);
}

esp_err_t orb_link_notify(const char *event, cJSON *params)
{
    return post_outgoing(MSG_EVENT, 0, event, params, 0, 0, NULL, NULL);
}

esp_err_t orb_link_notify_on(uint32_t generation, const char *event, cJSON *params)
{
    if (!generation) { cJSON_Delete(params); return ESP_ERR_INVALID_ARG; }
    return post_outgoing(MSG_EVENT, generation, event, params, 0, 0, NULL, NULL);
}

esp_err_t orb_link_on_audio(orb_link_audio_cb_t cb, void *ctx)
{
    taskENTER_CRITICAL(&s_lock);
    s_audio_cb = cb;
    s_audio_ctx = ctx;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

bool orb_link_online(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool online = s_status.state == ORB_LINK_ONLINE;
    taskEXIT_CRITICAL(&s_lock);
    return online;
}

bool orb_link_supports_cards(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool cards = s_cards;
    taskEXIT_CRITICAL(&s_lock);
    return cards;
}

bool orb_link_supports_home(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool home = s_home;
    taskEXIT_CRITICAL(&s_lock);
    return home;
}

bool orb_link_supports_weather(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool weather = s_weather;
    taskEXIT_CRITICAL(&s_lock);
    return weather;
}

bool orb_link_supports_voice(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool voice = s_voice;
    taskEXIT_CRITICAL(&s_lock);
    return voice;
}

uint32_t orb_link_generation(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint32_t generation = s_conn;
    taskEXIT_CRITICAL(&s_lock);
    return generation;
}

void orb_link_set_voice_available(bool available)
{
    taskENTER_CRITICAL(&s_lock);
    s_voice_available = available;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t orb_link_send_audio(const void *frame, size_t len)
{
    return orb_link_send_audio_on(orb_link_generation(), frame, len);
}

esp_err_t orb_link_send_audio_on(uint32_t generation, const void *frame, size_t len)
{
    const uint8_t *bytes = frame;
    if (!bytes || len < 9 || len > 1031 || ((len - 7) & 1) || bytes[0] != 1 ||
        !(bytes[1] || bytes[2])) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    bool online = generation && generation == s_conn && s_voice && s_status.state == ORB_LINK_ONLINE;
    taskEXIT_CRITICAL(&s_lock);
    if (!online) return ESP_ERR_INVALID_STATE;
    char *copy = orb_memory_alloc(ORB_MEMORY_NETWORK, len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, frame, len);
    msg_t m = { .kind = MSG_AUDIO, .conn = generation, .text = copy, .len = len };
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        orb_memory_free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Reliable output acknowledges the transport write, not queue admission.
 * This must be called off the link task; the callback runs on that task. */
esp_err_t orb_link_devtools_notify_until(uint32_t generation, const char *event, cJSON *params,
                                        int64_t deadline_us, orb_link_reply_cb_t cb, void *ctx)
{
    if (!generation || !event || strncmp(event, "devtools.", 9) || !cb || !orb_link_supports_devtools()) {
        cJSON_Delete(params);
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_timer_get_time() >= deadline_us) {
        cJSON_Delete(params);
        return ESP_ERR_TIMEOUT;
    }
    return post_outgoing(MSG_EVENT, generation, event, params, 5000, deadline_us, cb, ctx);
}

esp_err_t orb_link_devtools_notify_on(uint32_t generation, const char *event, cJSON *params,
                                     orb_link_reply_cb_t cb, void *ctx)
{
    return orb_link_devtools_notify_until(generation, event, params,
                                          esp_timer_get_time() + US(5000), cb, ctx);
}

bool orb_link_supports_devtools(void)
{
    taskENTER_CRITICAL(&s_lock);
#if CONFIG_ORB_DEVTOOLS
    bool enabled = s_devtools && s_status.state == ORB_LINK_ONLINE && !strcmp(s_status.channel, "dev");
#else
    bool enabled = false;
#endif
    taskEXIT_CRITICAL(&s_lock);
    return enabled;
}

esp_err_t orb_link_unpair(void)
{
    if (!s_queue) {
        forget_pairing();   /* not started: nothing else is using the pairing */
        return ESP_OK;
    }
    msg_t m = { .kind = MSG_UNPAIR };
    return xQueueSend(s_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t orb_link_hub_https(orb_link_https_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    out->cert_pem = NULL;
    /* No malloc inside a critical section: size the PEM first, then copy all
     * three in one go from whatever pairing is current by then, if it fits. */
    taskENTER_CRITICAL(&s_lock);
    size_t size = s_pair.pem ? strlen(s_pair.pem) + 1 : 0;
    taskEXIT_CRITICAL(&s_lock);
    char *pem = size ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!pem) {
        return size ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_lock);
    bool ready = s_pair.pem && strlen(s_pair.pem) < size && s_base_url[0];
    if (ready) {
        strcpy(pem, s_pair.pem);
        memcpy(out->base_url, s_base_url, sizeof(s_base_url));
        memcpy(out->authorization, s_auth, sizeof(s_auth));
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!ready) {
        free(pem);
        return ESP_ERR_INVALID_STATE;
    }
    out->cert_pem = pem;
    return ESP_OK;
}
