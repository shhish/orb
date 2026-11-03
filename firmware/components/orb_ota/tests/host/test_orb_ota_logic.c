/* Host tests for orb_ota_logic.c: run with `make` in this directory. */
#include <stdio.h>
#include <string.h>

#include "orb_ota_logic.h"

static int s_pass, s_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            s_pass++;                                                   \
        } else {                                                        \
            s_fail++;                                                   \
            fprintf(stderr, "%s:%d: FAILED %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define SHA_A "ac978e63f273ba9ee1132bfecc103c767c52512fd5b065cc0ee224f9ec49d6a2"
#define SHA_A_UPPER "AC978E63F273BA9EE1132BFECC103C767C52512FD5B065CC0EE224F9EC49D6A2"
#define SHA_B "acbc0186c13758df7488335a8e95f3eba38e7818a36dad14e3be0940ebc3f554"

static const char *GOOD =
    "{\"available\":true,\"version\":\"0.2.1\",\"build\":\"e1f2a3b\",\"size\":823456,"
    "\"sha256\":\"" SHA_A_UPPER "\",\"url\":\"https://orb.local:8443/v1/firmware/" SHA_A ".bin\","
    "\"install\":\"idle\",\"notes\":\"Faster glass\"}";

/* Parses GOOD with one field replaced (value is raw JSON) or removed (value NULL). */
static bool parse_with(const char *field, const char *value, orb_ota_offer_t *o, char *err)
{
    cJSON *p = cJSON_Parse(GOOD);
    if (field) {
        cJSON_DeleteItemFromObjectCaseSensitive(p, field);
        if (value) {
            cJSON_AddItemToObject(p, field, cJSON_Parse(value));
        }
    }
    bool ok = orb_ota_offer_parse(p, o, err, 64);
    cJSON_Delete(p);
    return ok;
}

static void test_parse(void)
{
    orb_ota_offer_t o;
    char err[64];

    CHECK(parse_with(NULL, NULL, &o, err));
    CHECK(o.available);
    CHECK(strcmp(o.version, "0.2.1") == 0);
    CHECK(strcmp(o.build, "e1f2a3b") == 0);
    CHECK(o.size == 823456);
    CHECK(strcmp(o.sha256, SHA_A) == 0);   /* lowercased */
    CHECK(strcmp(o.path, "/v1/firmware/" SHA_A ".bin") == 0);
    CHECK(o.install == ORB_OTA_INSTALL_IDLE);
    CHECK(strcmp(o.notes, "Faster glass") == 0);

    CHECK(parse_with("install", "\"now\"", &o, err) && o.install == ORB_OTA_INSTALL_NOW);
    CHECK(parse_with("install", "\"ask\"", &o, err) && o.install == ORB_OTA_INSTALL_ASK);
    CHECK(parse_with("url", "\"https://127.0.0.5:8443/v1/firmware/x.bin?t=1\"", &o, err) &&
          strcmp(o.path, "/v1/firmware/x.bin?t=1") == 0);
    CHECK(parse_with("build", NULL, &o, err) && o.build[0] == '\0');
    CHECK(parse_with("build", "null", &o, err) && o.build[0] == '\0');
    CHECK(parse_with("notes", NULL, &o, err) && o.notes[0] == '\0');
    CHECK(parse_with("size", "16777216", &o, err) && o.size == ORB_OTA_MAX_IMAGE);

    /* "available":false needs nothing else. */
    cJSON *none = cJSON_Parse("{\"available\":false}");
    CHECK(orb_ota_offer_parse(none, &o, err, sizeof(err)) && !o.available);
    cJSON_Delete(none);

    cJSON *arr = cJSON_Parse("[]");
    CHECK(!orb_ota_offer_parse(arr, &o, err, sizeof(err)) && strcmp(err, "bad offer: not an object") == 0);
    cJSON_Delete(arr);
    CHECK(!orb_ota_offer_parse(NULL, &o, err, sizeof(err)));

    CHECK(!parse_with("available", NULL, &o, err) && strcmp(err, "bad offer: available") == 0);
    CHECK(!parse_with("available", "1", &o, err));
    CHECK(!parse_with("version", NULL, &o, err) && strcmp(err, "bad offer: version") == 0);
    CHECK(!parse_with("version", "\"\"", &o, err));
    CHECK(!parse_with("version", "\"0123456789012345678901234567890123\"", &o, err));
    CHECK(parse_with("version", "\"0123456789012345678901234567890\"", &o, err));   /* 31: fits */
    CHECK(!parse_with("version", "\"0.2\\n1\"", &o, err));
    CHECK(!parse_with("version", "21", &o, err));
    CHECK(!parse_with("build", "7", &o, err) && strcmp(err, "bad offer: build") == 0);
    CHECK(!parse_with("size", NULL, &o, err) && strcmp(err, "bad offer: size") == 0);
    CHECK(!parse_with("size", "0", &o, err));
    CHECK(!parse_with("size", "-5", &o, err));
    CHECK(!parse_with("size", "1.5", &o, err));
    CHECK(!parse_with("size", "16777217", &o, err));
    CHECK(!parse_with("size", "\"823456\"", &o, err));
    CHECK(!parse_with("sha256", NULL, &o, err) && strcmp(err, "bad offer: sha256") == 0);
    CHECK(!parse_with("sha256", "\"" SHA_A "0\"", &o, err));
    CHECK(!parse_with("sha256", "\"ac978e63f273ba9ee1132bfecc103c767c52512fd5b065cc0ee224f9ec49d6a\"", &o, err));
    CHECK(!parse_with("sha256", "\"zc978e63f273ba9ee1132bfecc103c767c52512fd5b065cc0ee224f9ec49d6a2\"", &o, err));
    CHECK(!parse_with("url", NULL, &o, err) && strcmp(err, "bad offer: url") == 0);
    CHECK(!parse_with("url", "\"http://hub:8443/v1/firmware/x.bin\"", &o, err));
    CHECK(!parse_with("url", "\"https://hub:8443\"", &o, err));
    CHECK(!parse_with("url", "\"https:///v1/firmware/x.bin\"", &o, err));
    CHECK(!parse_with("url", "\"https://hub/v1/firm ware.bin\"", &o, err));
    CHECK(!parse_with("install", NULL, &o, err) && strcmp(err, "bad offer: install") == 0);
    CHECK(!parse_with("install", "\"later\"", &o, err));
    CHECK(!parse_with("install", "\"NOW\"", &o, err));

    /* Long notes are cut, never mid-character. */
    char notes[200] = "\"";
    for (int i = 0; i < 126; i++) {
        strcat(notes, "a");
    }
    strcat(notes, "\xc3\xa9\"");   /* "é" would straddle the 127-byte limit */
    CHECK(parse_with("notes", notes, &o, err) && strlen(o.notes) == 126);
    CHECK(parse_with("notes", "\"line one\\nline two\"", &o, err) && strcmp(o.notes, "line one\nline two") == 0);
    CHECK(!parse_with("notes", "5", &o, err) && strcmp(err, "bad offer: notes") == 0);
}

static void test_assets_offer(void)
{
    orb_ota_offer_t offer;
    char error[64];
    CHECK(parse_with(NULL, NULL, &offer, error) && !offer.assets);
    CHECK(parse_with("kind", "\"app\"", &offer, error) && !offer.assets);
    CHECK(!parse_with("kind", "null", &offer, error));
    CHECK(!parse_with("kind", "1", &offer, error));
    CHECK(!parse_with("kind", "\"other\"", &offer, error));
    cJSON *params = cJSON_Parse(GOOD);
    cJSON_AddStringToObject(params, "kind", "assets");
    cJSON_ReplaceItemInObjectCaseSensitive(params, "size", cJSON_CreateNumber(8192));
    CHECK(orb_ota_offer_parse(params, &offer, error, sizeof(error)) && offer.assets);
    cJSON_ReplaceItemInObjectCaseSensitive(params, "size", cJSON_CreateNumber(8388608));
    CHECK(orb_ota_offer_parse(params, &offer, error, sizeof(error)));
    cJSON_ReplaceItemInObjectCaseSensitive(params, "size", cJSON_CreateNumber(8388609));
    CHECK(!orb_ota_offer_parse(params, &offer, error, sizeof(error)));
    cJSON_ReplaceItemInObjectCaseSensitive(params, "size", cJSON_CreateNumber(8193));
    CHECK(!orb_ota_offer_parse(params, &offer, error, sizeof(error)));
    cJSON_ReplaceItemInObjectCaseSensitive(params, "size", cJSON_CreateNumber(4096));
    CHECK(!orb_ota_offer_parse(params, &offer, error, sizeof(error)));
    cJSON_Delete(params);
}

static void test_decide(void)
{
    orb_ota_offer_t o;
    char err[64];
    CHECK(parse_with(NULL, NULL, &o, err));
    orb_ota_ctx_t c = { .running_version = "0.2.0", .bad_sha = "" };

    o.install = ORB_OTA_INSTALL_NOW;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_INSTALL);
    o.install = ORB_OTA_INSTALL_ASK;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_HOLD);
    c.idle = true;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_HOLD);
    c.tapped = true;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_INSTALL);
    c.tapped = false;

    o.install = ORB_OTA_INSTALL_IDLE;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_INSTALL);
    c.idle = false;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_HOLD);
    c.tapped = true;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_INSTALL);
    c.tapped = false;

    /* An image that failed waits for a tap, whatever the policy. */
    c.bad_sha = SHA_A_UPPER;
    o.install = ORB_OTA_INSTALL_NOW;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_HOLD);
    c.tapped = true;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_INSTALL);
    c.tapped = false;
    c.bad_sha = SHA_B;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_INSTALL);

    /* On probation ESP-IDF refuses OTA: hold, even when tapped. */
    c.probation = true;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_HOLD);
    c.tapped = true;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_HOLD);
    c.tapped = false;
    c.probation = false;

    /* Busy, already running it, or nothing on offer: ignore. */
    c.busy = true;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_IGNORE);
    c.tapped = true;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_IGNORE);
    c.tapped = false;
    c.busy = false;
    c.running_version = "0.2.1";
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_IGNORE);
    c.running_version = "0.2.0";
    o.available = false;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_IGNORE);
    o.available = true;

    /* A low battery holds unattended installs, but not a tap. */
    c.low_battery = true;
    o.install = ORB_OTA_INSTALL_NOW;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_HOLD);
    o.install = ORB_OTA_INSTALL_IDLE;
    c.idle = true;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_HOLD);
    c.idle = false;
    c.tapped = true;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_INSTALL);
    c.tapped = false;
    c.low_battery = false;
    o.install = ORB_OTA_INSTALL_NOW;
    CHECK(orb_ota_decide(&o, &c) == ORB_OTA_INSTALL);
}

static void test_battery(void)
{
    CHECK(orb_ota_low_battery(ORB_OTA_MIN_BATTERY - 1, false));
    CHECK(orb_ota_low_battery(0, false));
    CHECK(!orb_ota_low_battery(ORB_OTA_MIN_BATTERY - 1, true));   /* charging */
    CHECK(!orb_ota_low_battery(ORB_OTA_MIN_BATTERY, false));
    CHECK(!orb_ota_low_battery(100, false));
    CHECK(!orb_ota_low_battery(-1, false));   /* unknown, or no battery */
}

static void test_outcome(void)
{
    /* Installed to ota_1, which runs and was confirmed, but the record wasn't cleared before a restart. */
    orb_ota_boot_t b = { .try_slot = "ota_1", .try_version = "0.2.1", .running_slot = "ota_1",
                         .running_version = "0.2.1", .boot_slot = "ota_1", .invalid_slot = "" };
    CHECK(orb_ota_boot_outcome(&b) == ORB_OTA_RESULT_OK);
    b.probation = true;   /* not decided yet */
    CHECK(orb_ota_boot_outcome(&b) == ORB_OTA_RESULT_NONE);
    b.probation = false;
    b.running_version = "0.3.0";   /* same slot, another version: USB-flashed since */
    CHECK(orb_ota_boot_outcome(&b) == ORB_OTA_RESULT_STALE);
    b.try_slot = "";   /* nothing recorded */
    CHECK(orb_ota_boot_outcome(&b) == ORB_OTA_RESULT_NONE);

    /* Back on ota_0. */
    b = (orb_ota_boot_t){ .try_slot = "ota_1", .try_version = "0.2.1", .running_slot = "ota_0",
                          .running_version = "0.2.0", .boot_slot = "ota_0", .invalid_slot = "ota_1" };
    CHECK(orb_ota_boot_outcome(&b) == ORB_OTA_RESULT_ROLLED_BACK);
    b.invalid_slot = "";
    b.boot_slot = "ota_1";   /* otadata picks ota_1, but it didn't load */
    CHECK(orb_ota_boot_outcome(&b) == ORB_OTA_RESULT_FAILED);
    b.boot_slot = "ota_0";   /* the boot slot never switched: reset in finish, or a USB flash */
    CHECK(orb_ota_boot_outcome(&b) == ORB_OTA_RESULT_STALE);
    b.boot_slot = "";        /* no otadata at all */
    CHECK(orb_ota_boot_outcome(&b) == ORB_OTA_RESULT_STALE);
    b.invalid_slot = "ota_0";   /* an invalid slot that isn't the attempt's */
    CHECK(orb_ota_boot_outcome(&b) == ORB_OTA_RESULT_STALE);
    b.probation = true;
    CHECK(orb_ota_boot_outcome(&b) == ORB_OTA_RESULT_NONE);

    CHECK(strcmp(orb_ota_result_name(ORB_OTA_RESULT_OK), "ok") == 0);
    CHECK(strcmp(orb_ota_result_name(ORB_OTA_RESULT_ROLLED_BACK), "rolled_back") == 0);
    CHECK(strcmp(orb_ota_result_name(ORB_OTA_RESULT_FAILED), "failed") == 0);
    CHECK(orb_ota_result_name(ORB_OTA_RESULT_NONE) == NULL);
    CHECK(orb_ota_result_name(ORB_OTA_RESULT_STALE) == NULL);

    /* What counts against the image. */
    CHECK(orb_ota_image_at_fault(ORB_OTA_RESULT_FAILED, false, false));
    CHECK(orb_ota_image_at_fault(ORB_OTA_RESULT_FAILED, false, true));
    CHECK(orb_ota_image_at_fault(ORB_OTA_RESULT_ROLLED_BACK, true, false));   /* missed the deadline */
    CHECK(orb_ota_image_at_fault(ORB_OTA_RESULT_ROLLED_BACK, true, true));
    CHECK(orb_ota_image_at_fault(ORB_OTA_RESULT_ROLLED_BACK, false, false));  /* crashed on probation */
    CHECK(!orb_ota_image_at_fault(ORB_OTA_RESULT_ROLLED_BACK, false, true));  /* power lost on probation */
    CHECK(!orb_ota_image_at_fault(ORB_OTA_RESULT_OK, false, false));
    CHECK(!orb_ota_image_at_fault(ORB_OTA_RESULT_STALE, false, false));
    CHECK(!orb_ota_image_at_fault(ORB_OTA_RESULT_NONE, false, false));
}

static void test_image(void)
{
    char err[64];
    CHECK(orb_ota_image_ok("0.2.1", "orb", "orb", "0.2.1", err, sizeof(err)));
    CHECK(!orb_ota_image_ok("0.2.1", "orb", "xiaozhi", "0.2.1", err, sizeof(err)) &&
          strcmp(err, "image is xiaozhi, not orb") == 0);
    CHECK(!orb_ota_image_ok("0.2.1", "orb", "orb", "0.2.2", err, sizeof(err)) &&
          strcmp(err, "image is 0.2.2, offer said 0.2.1") == 0);
}

static void test_sha(void)
{
    CHECK(orb_ota_is_sha_hex(SHA_A));
    CHECK(orb_ota_is_sha_hex(SHA_A_UPPER));
    CHECK(!orb_ota_is_sha_hex(""));
    CHECK(!orb_ota_is_sha_hex(NULL));
    CHECK(!orb_ota_is_sha_hex(SHA_A "a"));
    CHECK(orb_ota_sha_equal(SHA_A, SHA_A_UPPER));
    CHECK(!orb_ota_sha_equal(SHA_A, SHA_B));
    CHECK(!orb_ota_sha_equal("", ""));
    CHECK(!orb_ota_sha_equal(SHA_A, ""));

    /* SHA-256("test-cert"), the protocol's pairing test vector. */
    const uint8_t digest[32] = {
        0xac, 0x97, 0x8e, 0x63, 0xf2, 0x73, 0xba, 0x9e, 0xe1, 0x13, 0x2b, 0xfe, 0xcc, 0x10, 0x3c, 0x76,
        0x7c, 0x52, 0x51, 0x2f, 0xd5, 0xb0, 0x65, 0xcc, 0x0e, 0xe2, 0x24, 0xf9, 0xec, 0x49, 0xd6, 0xa2,
    };
    CHECK(orb_ota_digest_matches(digest, SHA_A));
    CHECK(orb_ota_digest_matches(digest, SHA_A_UPPER));
    CHECK(!orb_ota_digest_matches(digest, SHA_B));
    CHECK(!orb_ota_digest_matches(digest, "ac97"));
}

static void test_utf8(void)
{
    char buf[8];
    CHECK(orb_ota_copy_utf8(buf, sizeof(buf), "abc") == 3 && strcmp(buf, "abc") == 0);
    CHECK(orb_ota_copy_utf8(buf, sizeof(buf), "abcdefg") == 7 && strcmp(buf, "abcdefg") == 0);
    CHECK(orb_ota_copy_utf8(buf, sizeof(buf), "abcdefgh") == 7 && strcmp(buf, "abcdefg") == 0);
    CHECK(orb_ota_copy_utf8(buf, sizeof(buf), "abcde\xe2\x80\x94") == 5);           /* em dash cut */
    CHECK(orb_ota_copy_utf8(buf, sizeof(buf), "abcd\xe2\x80\x94") == 7);            /* fits exactly */
    CHECK(orb_ota_copy_utf8(buf, sizeof(buf), "abcd\xf0\x9f\x98\x80") == 4 && strcmp(buf, "abcd") == 0);
    CHECK(orb_ota_copy_utf8(buf, 1, "abc") == 0 && buf[0] == '\0');
    CHECK(orb_ota_copy_utf8(buf, 0, "abc") == 0);
}

static void test_percent(void)
{
    CHECK(orb_ota_percent(0, 100) == 0);
    CHECK(orb_ota_percent(50, 100) == 50);
    CHECK(orb_ota_percent(823455, 823456) == 99);
    CHECK(orb_ota_percent(823456, 823456) == 100);
    CHECK(orb_ota_percent(900000, 823456) == 100);
    CHECK(orb_ota_percent(10, 0) == 0);
    CHECK(orb_ota_percent(-1, 100) == 0);
    CHECK(orb_ota_percent(4000000000LL, 4100000000LL) == 97);
}

static void test_stalled(void)
{
    const int64_t limit = 30000000;
    orb_ota_watch_t w = { .read = -1 };
    CHECK(!orb_ota_stalled(&w, 0, 1000, limit));   /* the first look starts the clock */
    CHECK(!orb_ota_stalled(&w, 0, 1000 + limit - 1, limit));
    CHECK(orb_ota_stalled(&w, 0, 1000 + limit, limit));
    CHECK(!orb_ota_stalled(&w, 4096, 2000 + limit, limit));   /* any progress restarts it */
    CHECK(!orb_ota_stalled(&w, 4096, 2000 + 2 * limit - 1, limit));
    CHECK(orb_ota_stalled(&w, 4096, 2000 + 2 * limit, limit));
}

static void test_json(void)
{
    cJSON *hello = cJSON_CreateObject();
    orb_ota_add_last_ota(hello, "0.2.1", "rolled_back", "not healthy within 60 s");
    char *text = cJSON_PrintUnformatted(hello);
    CHECK(strcmp(text, "{\"last_ota\":{\"version\":\"0.2.1\",\"result\":\"rolled_back\","
                       "\"error\":\"not healthy within 60 s\"}}") == 0);
    cJSON_free(text);
    cJSON_Delete(hello);

    hello = cJSON_CreateObject();
    orb_ota_add_last_ota(hello, "0.2.1", "ok", NULL);
    text = cJSON_PrintUnformatted(hello);
    CHECK(strcmp(text, "{\"last_ota\":{\"version\":\"0.2.1\",\"result\":\"ok\"}}") == 0);
    cJSON_free(text);
    cJSON_Delete(hello);

    cJSON *r = orb_ota_report("0.2.1", "downloading", 40, "");
    text = cJSON_PrintUnformatted(r);
    CHECK(strcmp(text, "{\"version\":\"0.2.1\",\"state\":\"downloading\",\"progress\":40}") == 0);
    cJSON_free(text);
    cJSON_Delete(r);

    r = orb_ota_report("0.2.1", "failed", 100, "SHA-256 mismatch");
    text = cJSON_PrintUnformatted(r);
    CHECK(strcmp(text, "{\"version\":\"0.2.1\",\"state\":\"failed\",\"progress\":100,"
                       "\"error\":\"SHA-256 mismatch\"}") == 0);
    cJSON_free(text);
    cJSON_Delete(r);
}

int main(void)
{
    test_parse();
    test_assets_offer();
    test_decide();
    test_battery();
    test_outcome();
    test_image();
    test_sha();
    test_utf8();
    test_percent();
    test_stalled();
    test_json();
    printf("orb_ota_logic: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
