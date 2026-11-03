/*
 * Host tests for orb_net's plain logic (orb_net_util.c, orb_net_tz.c).
 * Run with `make` in this directory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "orb_net_util.h"

static void test_names(void)
{
    /* Synthetic locally administered MAC; never a physical board identity. */
    const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0xab, 0xcd, 0xef };
    char id[ORB_NET_DEVICE_ID_LEN], service[ORB_NET_SHORT_NAME_LEN], host[ORB_NET_SHORT_NAME_LEN];
    orb_net_names(mac, id, service, host);
    CHECK_STR(id, "orb-020000abcdef");
    CHECK_STR(service, "Orb-CDEF");
    CHECK_STR(host, "orb-cdef");

    const uint8_t edge[6] = { 0x00, 0x01, 0x0a, 0x10, 0xff, 0x0f };
    orb_net_names(edge, id, service, host);
    CHECK_STR(id, "orb-00010a10ff0f");
    CHECK_STR(service, "Orb-FF0F");
    CHECK_STR(host, "orb-ff0f");
}

static const uint32_t *s_rnd;
static int s_rnd_calls;

static uint32_t fake_rnd(void)
{
    return s_rnd[s_rnd_calls++];
}

static void pop_from(const uint32_t *seq, char out[ORB_NET_POP_LEN])
{
    s_rnd = seq;
    s_rnd_calls = 0;
    orb_net_pop(fake_rnd, out);
}

static void test_pop(void)
{
    char pop[ORB_NET_POP_LEN];

    pop_from((const uint32_t[]){ 123 }, pop);
    CHECK_STR(pop, "00000123");
    CHECK(s_rnd_calls == 1);

    pop_from((const uint32_t[]){ 0 }, pop);
    CHECK_STR(pop, "00000000");

    /* The top of the range that's still unbiased. */
    pop_from((const uint32_t[]){ 4199999999u }, pop);
    CHECK_STR(pop, "99999999");

    /* Values from 4.2e9 up would favour low codes, so they're drawn again. */
    pop_from((const uint32_t[]){ 4200000000u, 0xFFFFFFFFu, 4212345678u, 87654321u }, pop);
    CHECK_STR(pop, "87654321");
    CHECK(s_rnd_calls == 4);

    pop_from((const uint32_t[]){ 1234567890u }, pop);
    CHECK_STR(pop, "34567890");
    CHECK(strlen(pop) == 8);
}

static void test_backoff(void)
{
    CHECK(orb_net_backoff_ms(0, 0) == 0);
    CHECK(orb_net_backoff_ms(0, 0xFFFFFFFFu) == 0);

    const uint32_t base[] = { 0, 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000 };
    for (unsigned n = 1; n < sizeof(base) / sizeof(base[0]); n++) {
        CHECK(orb_net_backoff_ms(n, 0) == base[n]);
        /* Jitter stays within 20 % whatever the random value. */
        const uint32_t rnds[] = { 1, 199, 200, 201, 6000, 123456789u, 0xFFFFFFFFu };
        for (size_t i = 0; i < sizeof(rnds) / sizeof(rnds[0]); i++) {
            uint32_t d = orb_net_backoff_ms(n, rnds[i]);
            CHECK(d >= base[n] && d <= base[n] + base[n] / 5);
        }
    }
    CHECK(orb_net_backoff_ms(1, 200) == 1200);   /* the full 20 % is reachable */
    CHECK(orb_net_backoff_ms(1000, 0) == 30000);
    CHECK(orb_net_backoff_ms(0xFFFFFFFFu, 0xFFFFFFFFu) <= 36000);
}

typedef struct {
    const char *key, *value;
} kv_t;

static orb_hub_txt_t txt_of(const kv_t *items, size_t n)
{
    orb_hub_txt_t t = { 0 };
    for (size_t i = 0; i < n; i++) {
        orb_hub_txt_add(&t, items[i].key, items[i].value, items[i].value ? strlen(items[i].value) : 0);
    }
    return t;
}

#define TXT(...) txt_of((const kv_t[]){ __VA_ARGS__ }, sizeof((const kv_t[]){ __VA_ARGS__ }) / sizeof(kv_t))

static void test_txt(void)
{
    /* As the hub advertises it (docs/protocol.md, "Discovery"). */
    orb_hub_txt_t t = TXT({ "v", "1" }, { "pair", "8080" }, { "fp", "ac978e63f273ba9e" });
    CHECK(orb_hub_txt_usable(&t));
    CHECK(t.version == 1);
    CHECK(t.pair_port == 8080);
    CHECK_STR(t.fp, "ac978e63f273ba9e");

    /* Keys are case-insensitive, order doesn't matter, the fingerprint is
     * lowercased, unknown keys are ignored. */
    t = TXT({ "FP", "AC978E63F273BA9E" }, { "board", "x" }, { "Pair", "18080" }, { "V", "1" });
    CHECK(orb_hub_txt_usable(&t));
    CHECK(t.pair_port == 18080);
    CHECK_STR(t.fp, "ac978e63f273ba9e");

    /* The first of a repeated key wins (RFC 6763, 6.4). */
    t = TXT({ "v", "1" }, { "v", "2" }, { "pair", "8080" }, { "pair", "9090" }, { "fp", "0123456789abcdef" },
            { "fp", "ffffffffffffffff" });
    CHECK(t.version == 1 && t.pair_port == 8080);
    CHECK_STR(t.fp, "0123456789abcdef");
    t = TXT({ "pair", "x" }, { "pair", "8080" }, { "v", "1" }, { "fp", "0123456789abcdef" });
    CHECK(t.pair_port == 0);
    CHECK(!orb_hub_txt_usable(&t));

    /* Another protocol version is not for this firmware. */
    t = TXT({ "v", "2" }, { "pair", "8080" }, { "fp", "0123456789abcdef" });
    CHECK(t.version == 2);
    CHECK(!orb_hub_txt_usable(&t));

    /* Each field missing or malformed. */
    t = TXT({ "pair", "8080" }, { "fp", "0123456789abcdef" });
    CHECK(!orb_hub_txt_usable(&t));
    t = TXT({ "v", "1" }, { "fp", "0123456789abcdef" });
    CHECK(!orb_hub_txt_usable(&t));
    t = TXT({ "v", "1" }, { "pair", "8080" });
    CHECK(!orb_hub_txt_usable(&t));
    const char *bad_ports[] = { "0", "65536", "99999", "-1", "+80", " 80", "80 ", "8o80", "", "123456" };
    for (size_t i = 0; i < sizeof(bad_ports) / sizeof(bad_ports[0]); i++) {
        t = TXT({ "v", "1" }, { "pair", bad_ports[i] }, { "fp", "0123456789abcdef" });
        CHECK(t.pair_port == 0);
        CHECK(!orb_hub_txt_usable(&t));
    }
    t = TXT({ "pair", "65535" });
    CHECK(t.pair_port == 65535);
    const char *bad_fps[] = { "0123456789abcde", "0123456789abcdef0", "0123456789abcdeg", "0123456789abcde ", "" };
    for (size_t i = 0; i < sizeof(bad_fps) / sizeof(bad_fps[0]); i++) {
        t = TXT({ "v", "1" }, { "pair", "8080" }, { "fp", bad_fps[i] });
        CHECK(t.fp[0] == '\0');
        CHECK(!orb_hub_txt_usable(&t));
    }
    t = TXT({ "v", "01" });
    CHECK(t.version == 1);
    t = TXT({ "v", "1.0" });
    CHECK(t.version == 0);

    /* Keys without a value, a NULL key, and values that aren't NUL-terminated
     * (mDNS hands over lengths). */
    t = TXT({ "v", NULL }, { "pair", NULL }, { "fp", NULL });
    CHECK(t.version == 0 && t.pair_port == 0 && t.fp[0] == '\0');
    orb_hub_txt_t u = { 0 };
    orb_hub_txt_add(&u, NULL, "1", 1);
    CHECK(u.seen == 0);
    orb_hub_txt_add(&u, "v", "1junk", 1);
    orb_hub_txt_add(&u, "pair", "8080junk", 4);
    orb_hub_txt_add(&u, "fp", "fedcba9876543210junk", 16);
    CHECK(orb_hub_txt_usable(&u));
    CHECK(u.pair_port == 8080);
    CHECK_STR(u.fp, "fedcba9876543210");
}

/* The POSIX footer of a TZif file: its last line. */
static int zoneinfo_footer(const char *iana, char *out, size_t len)
{
    char path[256];
    snprintf(path, sizeof(path), "/usr/share/zoneinfo/%s", iana);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof(buf) - 1 ? size - (long)sizeof(buf) + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (n < 2 || buf[n - 1] != '\n') {
        return -1;
    }
    /* The file is binary up to there, so look for the newline by hand. */
    size_t start = n - 1;
    while (start > 0 && buf[start - 1] != '\n') {
        start--;
    }
    if (start == 0) {
        return -1;
    }
    buf[n - 1] = '\0';
    snprintf(out, len, "%s", buf + start);
    return 0;
}

#define REPLAY_STEP (30 * 60)

/* UTC offsets (and DST flags) for every half hour of 2027-2030 under TZ. */
static void replay(const char *tz, long *gmtoff, int *isdst, size_t n, time_t from)
{
    setenv("TZ", tz, 1);
    tzset();
    for (size_t i = 0; i < n; i++) {
        time_t t = from + (time_t)i * REPLAY_STEP;
        struct tm tm;
        localtime_r(&t, &tm);
        gmtoff[i] = tm.tm_gmtoff;
        isdst[i] = tm.tm_isdst;
    }
}

static void test_tz(void)
{
    CHECK(orb_tz_table_len >= 40);
    for (size_t i = 0; i < orb_tz_table_len; i++) {
        const orb_tz_entry_t *e = &orb_tz_table[i];
        CHECK(e->posix[0] != '\0');
        if (i > 0) {
            CHECK(strcmp(orb_tz_table[i - 1].iana, e->iana) < 0);   /* sorted, no duplicates */
        }
        CHECK(orb_net_posix_tz(e->iana) == e->posix);
    }
    CHECK_STR(orb_net_posix_tz("America/New_York"), "EST5EDT,M3.2.0,M11.1.0");
    CHECK_STR(orb_net_posix_tz("Asia/Kolkata"), "IST-5:30");
    CHECK_STR(orb_net_posix_tz("Asia/Calcutta"), "IST-5:30");
    CHECK_STR(orb_net_posix_tz("Europe/London"), "GMT0BST,M3.5.0/1,M10.5.0");
    CHECK_STR(orb_net_posix_tz("Australia/Sydney"), "AEST-10AEDT,M10.1.0,M4.1.0/3");
    CHECK(orb_net_posix_tz("Mars/Olympus_Mons") == NULL);
    CHECK(orb_net_posix_tz("america/new_york") == NULL);   /* IANA names are case-sensitive */
    CHECK(orb_net_posix_tz("") == NULL);
    CHECK(orb_net_posix_tz(NULL) == NULL);

    /* The zones the brief asked for by region are all there. */
    const char *must[] = {
        "America/New_York", "America/Chicago", "America/Denver", "America/Phoenix", "America/Los_Angeles",
        "America/Anchorage", "Pacific/Honolulu", "America/Toronto", "America/Vancouver", "America/Halifax",
        "America/St_Johns", "Europe/London", "Europe/Paris", "Europe/Berlin", "Europe/Madrid", "Europe/Rome",
        "Europe/Amsterdam", "Europe/Stockholm", "Europe/Athens", "Europe/Helsinki", "Europe/Istanbul",
        "Europe/Moscow", "Asia/Kolkata", "Asia/Shanghai", "Asia/Hong_Kong", "Asia/Taipei", "Asia/Tokyo",
        "Asia/Seoul", "Asia/Singapore", "Asia/Bangkok", "Asia/Jakarta", "Asia/Manila", "Asia/Dubai",
        "Australia/Sydney", "Australia/Melbourne", "Australia/Brisbane", "Australia/Adelaide",
        "Australia/Perth", "Pacific/Auckland", "America/Sao_Paulo", "America/Mexico_City",
        "America/Argentina/Buenos_Aires", "Africa/Johannesburg", "UTC",
    };
    for (size_t i = 0; i < sizeof(must) / sizeof(must[0]); i++) {
        if (!orb_net_posix_tz(must[i])) {
            fprintf(stderr, "missing zone %s\n", must[i]);
        }
        CHECK(orb_net_posix_tz(must[i]) != NULL);
    }

    /* Against the Mac's tzdata: the same footer, and the same offsets for
     * every half hour of 2027-2030 when the POSIX rule replaces the zone file. */
    const time_t from = 1798761600;   /* 2027-01-01T00:00:00Z */
    const size_t n = (size_t)(4 * 365 + 1) * 24 * 3600 / REPLAY_STEP;
    long *off_posix = malloc(n * sizeof(long)), *off_zone = malloc(n * sizeof(long));
    int *dst_posix = malloc(n * sizeof(int)), *dst_zone = malloc(n * sizeof(int));
    int compared = 0;
    for (size_t i = 0; i < orb_tz_table_len; i++) {
        const orb_tz_entry_t *e = &orb_tz_table[i];
        char footer[128];
        if (zoneinfo_footer(e->iana, footer, sizeof(footer)) != 0) {
            continue;
        }
        compared++;
        CHECK_STR(e->posix, footer);
        replay(e->posix, off_posix, dst_posix, n, from);
        replay(e->iana, off_zone, dst_zone, n, from);
        size_t bad = 0;
        for (size_t k = 0; k < n; k++) {
            bad += off_posix[k] != off_zone[k] || dst_posix[k] != dst_zone[k];
        }
        if (bad) {
            fprintf(stderr, "%s: %zu half hours differ from tzdata\n", e->iana, bad);
        }
        CHECK(bad == 0);
    }
    free(off_posix);
    free(off_zone);
    free(dst_posix);
    free(dst_zone);
    unsetenv("TZ");
    tzset();
    if (compared == 0) {
        printf("note: no /usr/share/zoneinfo, skipped the tzdata comparison\n");
    } else {
        printf("compared %d zones with /usr/share/zoneinfo\n", compared);
    }
}

static void test_tz_resolve(void)
{
    /* Validity helper */
    CHECK(orb_net_posix_tz_valid("EST5EDT,M3.2.0,M11.1.0"));
    CHECK(orb_net_posix_tz_valid("UTC0"));
    CHECK(orb_net_posix_tz_valid("+1245+1345,M9.5.0/2:45,M4.1.0/3:45"));
    CHECK(!orb_net_posix_tz_valid(NULL));
    CHECK(!orb_net_posix_tz_valid(""));
    CHECK(!orb_net_posix_tz_valid("EST5EDT\n"));
    CHECK(!orb_net_posix_tz_valid("EST\x01"));
    char long_tz[65];
    memset(long_tz, 'A', 64);
    long_tz[64] = '\0';
    CHECK(!orb_net_posix_tz_valid(long_tz));
    long_tz[63] = '\0';
    CHECK(orb_net_posix_tz_valid(long_tz));

    /* Resolution order: table, then posix, then UTC */
    /* 1. Zone in table: built-in rule wins, even if posix rule is provided or differs */
    CHECK_STR(orb_net_resolve_tz("America/New_York", NULL), "EST5EDT,M3.2.0,M11.1.0");
    CHECK_STR(orb_net_resolve_tz("America/New_York", "CUSTOM_POSIX_IGNORED"), "EST5EDT,M3.2.0,M11.1.0");

    /* 2. Zone not in table: valid posix rule is used */
    CHECK_STR(orb_net_resolve_tz("Pacific/Kanton", "+13"), "+13");
    CHECK_STR(orb_net_resolve_tz("Custom/Zone", "CST6CDT,M3.2.0,M11.1.0"), "CST6CDT,M3.2.0,M11.1.0");

    /* 3. Zone not in table, invalid or NULL posix rule: falls back to UTC0 */
    CHECK_STR(orb_net_resolve_tz("Pacific/Kanton", NULL), "UTC0");
    CHECK_STR(orb_net_resolve_tz("Pacific/Kanton", ""), "UTC0");
    CHECK_STR(orb_net_resolve_tz("Pacific/Kanton", "INVALID\nRULE"), "UTC0");
    CHECK_STR(orb_net_resolve_tz(NULL, NULL), "UTC0");
    CHECK_STR(orb_net_resolve_tz(NULL, "+0530"), "+0530");
}

int main(void)
{
    test_names();
    test_pop();
    test_backoff();
    test_txt();
    test_tz();
    test_tz_resolve();
    return check_report();
}
