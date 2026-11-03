#pragma once

/*
 * The parts of orb_net that are plain logic: names, the provisioning code,
 * reconnect timing, hub TXT records and time zones. No ESP-IDF here, so
 * tests/host builds and runs them on the Mac.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORB_NET_DEVICE_ID_LEN 17   /* "orb-020000abcdef" */
#define ORB_NET_SHORT_NAME_LEN 9   /* "Orb-CDEF", "orb-cdef" */
#define ORB_NET_POP_LEN 9          /* 8 digits */

/* From the Wi-Fi station MAC: device_id "orb-" + 12 lowercase hex digits
 * (docs/protocol.md), the BLE service name "Orb-XXXX" and the mDNS/DHCP host
 * name "orb-xxxx" (both from the last 2 bytes). */
void orb_net_names(const uint8_t mac[6], char device_id[ORB_NET_DEVICE_ID_LEN],
                   char service[ORB_NET_SHORT_NAME_LEN], char host[ORB_NET_SHORT_NAME_LEN]);

/* Proof of possession for BLE provisioning: 8 decimal digits, uniform over
 * 00000000-99999999. rnd returns 32 random bits (esp_random). */
void orb_net_pop(uint32_t (*rnd)(void), char out[ORB_NET_POP_LEN]);

/* Wait before the next join after `failures` failed attempts in a row: 0 at
 * once (the network was just lost), then 1, 2, 4 ... 30 s, plus up to 20 %
 * jitter taken from rnd. Same shape as the link's backoff. */
uint32_t orb_net_backoff_ms(unsigned failures, uint32_t rnd);

/* Background and eight consecutive absent-network failures keep growing to
 * five minutes. The cap includes jitter; interactive's old schedule is kept. */
uint32_t orb_net_retry_ms(unsigned failures, unsigned absent, bool background, uint32_t rnd);

typedef struct {
    int64_t due_ms;         /* zero for no scheduled join */
    int64_t prompt_ms;
    bool prompted;
} orb_net_retry_t;

/* An INTERACTIVE transition can advance a pending join, once per 30 seconds. */
bool orb_net_retry_prompt(orb_net_retry_t *retry, int64_t now_ms);
bool orb_net_power_background(bool background, bool provisioning, unsigned work);

/* TXT of an _orb-hub._tcp answer (docs/protocol.md, "Discovery"), fed one
 * item at a time. Keys are case-insensitive and the first of a repeated key
 * wins (RFC 6763, 6.4). Start from a zeroed struct. */
typedef struct {
    uint8_t seen;          /* keys already taken */
    int version;           /* v=, 0 if absent or not a number */
    uint16_t pair_port;    /* pair=, 0 if absent or not a port */
    char fp[17];           /* fp=, lowercased; empty if not 16 hex digits */
} orb_hub_txt_t;

/* value may be NULL (a key without '='); it need not be NUL-terminated. */
void orb_hub_txt_add(orb_hub_txt_t *t, const char *key, const char *value, size_t len);

/* v=1 with a pairing port and a fingerprint: a hub this firmware can use. */
bool orb_hub_txt_usable(const orb_hub_txt_t *t);

/* POSIX TZ string for an IANA zone name, NULL if the table doesn't have it. */
const char *orb_net_posix_tz(const char *iana);

/* Validates a hub-supplied POSIX TZ string: printable ASCII, non-empty, and under 64 chars. */
bool orb_net_posix_tz_valid(const char *posix);

/*
 * Resolves the POSIX TZ rule to use.
 * Resolution order:
 * 1. orb_net_posix_tz(iana) if found in the built-in table.
 * 2. posix, if valid (orb_net_posix_tz_valid(posix) is true).
 * 3. Fall back to "UTC0".
 */
const char *orb_net_resolve_tz(const char *iana, const char *posix);

/* The table itself, sorted by name; for the host tests. */
typedef struct {
    const char *iana;
    const char *posix;
} orb_tz_entry_t;

extern const orb_tz_entry_t orb_tz_table[];
extern const size_t orb_tz_table_len;
