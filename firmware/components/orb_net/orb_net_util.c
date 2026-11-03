#include "orb_net_util.h"

#include <string.h>
#include <strings.h>

static void put_hex(char *out, const uint8_t *bytes, size_t n, const char *digits)
{
    for (size_t i = 0; i < n; i++) {
        *out++ = digits[bytes[i] >> 4];
        *out++ = digits[bytes[i] & 0xF];
    }
    *out = '\0';
}

void orb_net_names(const uint8_t mac[6], char device_id[ORB_NET_DEVICE_ID_LEN],
                   char service[ORB_NET_SHORT_NAME_LEN], char host[ORB_NET_SHORT_NAME_LEN])
{
    static const char lower[] = "0123456789abcdef", upper[] = "0123456789ABCDEF";
    memcpy(device_id, "orb-", 4);
    put_hex(device_id + 4, mac, 6, lower);
    memcpy(service, "Orb-", 4);
    put_hex(service + 4, mac + 4, 2, upper);
    memcpy(host, "orb-", 4);
    put_hex(host + 4, mac + 4, 2, lower);
}

void orb_net_pop(uint32_t (*rnd)(void), char out[ORB_NET_POP_LEN])
{
    /* 4.2e9 is the largest multiple of 10^8 that fits in 32 bits; drawing
     * again above it keeps every code equally likely. */
    uint32_t r;
    do {
        r = rnd();
    } while (r >= 4200000000u);
    r %= 100000000u;
    for (int i = 7; i >= 0; i--) {
        out[i] = (char)('0' + r % 10);
        r /= 10;
    }
    out[8] = '\0';
}

uint32_t orb_net_backoff_ms(unsigned failures, uint32_t rnd)
{
    if (failures == 0) {
        return 0;
    }
    uint32_t base = failures >= 6 ? 30000 : 1000u << (failures - 1);
    return base + rnd % (base / 5 + 1);
}

uint32_t orb_net_retry_ms(unsigned failures, unsigned absent, bool background, uint32_t rnd)
{
    if ((!background && absent < 8) || failures == 0) {
        return orb_net_backoff_ms(failures, rnd);
    }
    uint32_t base = failures >= 10 ? 300000 : 1000u << (failures - 1);
    uint32_t delay = base + rnd % (base / 5 + 1);
    return delay < 300000 ? delay : 300000;
}

bool orb_net_retry_prompt(orb_net_retry_t *retry, int64_t now_ms)
{
    if (retry->due_ms <= now_ms || (retry->prompted && now_ms - retry->prompt_ms < 30000)) {
        return false;
    }
    retry->due_ms = now_ms > 0 ? now_ms : 1;
    retry->prompt_ms = now_ms;
    retry->prompted = true;
    return true;
}

bool orb_net_power_background(bool background, bool provisioning, unsigned work)
{
    return background && !provisioning && work == 0;
}

enum { SEEN_V = 1, SEEN_PAIR = 2, SEEN_FP = 4 };

/* Plain decimal digits up to max, else -1. */
static long parse_uint(const char *s, size_t len, long max)
{
    if (!s || len == 0 || len > 5) {
        return -1;
    }
    long v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    return v <= max ? v : -1;
}

static bool take(orb_hub_txt_t *t, const char *key, const char *name, uint8_t bit)
{
    if ((t->seen & bit) || strcasecmp(key, name) != 0) {
        return false;
    }
    t->seen |= bit;
    return true;
}

void orb_hub_txt_add(orb_hub_txt_t *t, const char *key, const char *value, size_t len)
{
    if (!key) {
        return;
    }
    if (take(t, key, "v", SEEN_V)) {
        long v = parse_uint(value, len, 65535);
        t->version = v > 0 ? (int)v : 0;
    } else if (take(t, key, "pair", SEEN_PAIR)) {
        long port = parse_uint(value, len, 65535);
        t->pair_port = port > 0 ? (uint16_t)port : 0;
    } else if (take(t, key, "fp", SEEN_FP)) {
        if (!value || len != 16) {
            return;
        }
        char fp[17];
        for (size_t i = 0; i < 16; i++) {
            char c = value[i];
            if (c >= 'A' && c <= 'F') {
                c = (char)(c - 'A' + 'a');
            } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return;
            }
            fp[i] = c;
        }
        fp[16] = '\0';
        memcpy(t->fp, fp, sizeof(fp));
    }
}

bool orb_hub_txt_usable(const orb_hub_txt_t *t)
{
    return t->version == 1 && t->pair_port != 0 && t->fp[0] != '\0';
}

const char *orb_net_posix_tz(const char *iana)
{
    if (!iana) {
        return NULL;
    }
    size_t lo = 0, hi = orb_tz_table_len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(iana, orb_tz_table[mid].iana);
        if (c == 0) {
            return orb_tz_table[mid].posix;
        }
        if (c < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

bool orb_net_posix_tz_valid(const char *posix)
{
    size_t len = posix ? strnlen(posix, 64) : 0;
    if (len == 0 || len >= 64) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (posix[i] < 32 || posix[i] > 126) {
            return false;
        }
    }
    return true;
}

const char *orb_net_resolve_tz(const char *iana, const char *posix)
{
    const char *from_table = orb_net_posix_tz(iana);
    if (from_table) {
        return from_table;
    }
    if (orb_net_posix_tz_valid(posix)) {
        return posix;
    }
    return "UTC0";
}
