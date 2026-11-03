#pragma once

/*
 * The JSON of docs/protocol.md ("Link"): envelopes, plus the bodies orb_link
 * builds or reads itself (pair.begin, hello, pair.done, state.report). Plain C
 * and cJSON, no ESP-IDF, so the host tests can hold it against the spec.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/* "Messages are at most 16 KB and nest at most 32 levels of arrays and
 * objects, the envelope included." */
#define LINK_MAX_MSG (16 * 1024)
#define LINK_MAX_DEPTH 32

typedef enum {
    LINK_REQ,
    LINK_RES,
    LINK_EVT,
} link_kind_t;

typedef struct {
    link_kind_t kind;
    int id;             /* REQ, RES */
    const char *name;   /* REQ: method, EVT: event */
    bool ok;            /* RES */
    const cJSON *body;  /* REQ, EVT: "p"; RES: "r" or "e". An absent "p" or "r" reads as {}. */
    const char *code;   /* RES with ok false: "e"."code" */
    bool invalid;      /* dispatch parser only: routeable malformed application body */
} link_msg_t;

/*
 * Parses one text frame. Returns the tree that `out` points into (free it with
 * cJSON_Delete), or NULL when the frame isn't a usable envelope. A request
 * comes back as long as it has an id, so it can be answered: `name` or `body`
 * is NULL when "m" or "p" is wrong, and the answer is bad_request. Frames over
 * LINK_MAX_MSG or nested deeper than LINK_MAX_DEPTH are dropped unparsed.
 */
cJSON *link_parse(const char *text, size_t len, link_msg_t *out);
/* Same transport validation, but preserve a routeable event/response with an
 * invalid body so optional applications can reject it without ending the link.
 * Other consumers must still reject out->invalid. */
cJSON *link_parse_dispatch(const char *text, size_t len, link_msg_t *out);

/*
 * Envelopes. Each takes ownership of the body (NULL means {}) and returns a
 * string to free with cJSON_free, or NULL when out of memory or the message
 * would be longer than LINK_MAX_MSG.
 */
char *link_encode_req(int id, const char *method, cJSON *params);
char *link_encode_res(int id, cJSON *result);
char *link_encode_err(int id, const char *code, const char *msg);
char *link_encode_evt(const char *name, cJSON *params);

/* Lowercase hex of n bytes; `out` holds 2n + 1. */
void link_hex(const uint8_t *in, size_t n, char *out);
/* Exactly 2n hex digits (either case) to n bytes. */
bool link_unhex(const char *hex, uint8_t *out, size_t n);

/* What the device says about itself in pair.begin and hello. */
typedef struct {
    const char *device_id;
    const char *name;
    const char *model;
    const char *fw;
    const char *build;
    const char *idf;
    const char *boot_slot;
    const char *reset_reason;
    uint32_t uptime_s;
} link_device_t;

typedef struct {
    int battery_pct;   /* -1 when no battery */
    int battery_mv;    /* -1 when unknown */
    bool charging;
    bool usb;
    const char *display_state; /* "active", "dimmed", "always_on" or "sleep" */
} link_power_t;

/* Request and event bodies; NULL when out of memory. `power` may be NULL
 * (unknown), and is then left out. */
cJSON *link_pair_begin_params(const link_device_t *dev, const uint8_t commit_d[32]);
cJSON *link_pair_reveal_params(const uint8_t nonce_d[16]);
cJSON *link_hello_params(const link_device_t *dev, const char *token, const link_power_t *power, int rssi);
cJSON *link_state_report_params(const link_power_t *power, int rssi, uint32_t heap_internal, uint32_t heap_psram,
                                uint32_t uptime_s);

typedef struct {
    uint8_t nonce_h[16];
    char hub_name[64];
} link_pair_begin_result_t;

/* pair.begin's result; false without a valid nonce_h. */
bool link_read_pair_begin(const cJSON *result, link_pair_begin_result_t *out);

/* pair.done's token; false unless it's 64 hex digits. */
bool link_read_pair_done(const cJSON *params, char token[65]);

typedef struct {
    char hub_name[64];
    char hub_version[16];
    int64_t time_ms;   /* 0 when absent */
    bool have_hour24, hour24;
    char tz[48];
    char tz_posix[64];
    char device_name[32];
    char channel[16];
} link_hello_result_t;

/* hello's result; missing fields read as empty. */
void link_read_hello(const cJSON *result, link_hello_result_t *out);

/* Validate the whole batch before applying any child, preserving the ordinary
 * event-only route and prohibiting recursive batches. Unknown events may pass. */
bool link_batch_valid(const cJSON *params);
