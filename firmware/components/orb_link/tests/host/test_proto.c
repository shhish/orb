/* Envelopes and bodies against the shapes in docs/protocol.md, "Link". */
#include <stdint.h>
#include <stdlib.h>

#include "link_proto.h"
#include "test.h"

#define TOKEN "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

/* Checks an encoder's output, then frees it. */
static void check_json(char *got, const char *want)
{
    CHECK_STR(got, want);
    cJSON_free(got);
}

static cJSON *parse(const char *text, link_msg_t *msg)
{
    return link_parse(text, strlen(text), msg);
}

static void test_encode(void)
{
    check_json(link_encode_req(1, "ota.check", NULL), "{\"t\":\"req\",\"id\":1,\"m\":\"ota.check\",\"p\":{}}");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "uptime_s", 12);
    check_json(link_encode_res(7, r), "{\"t\":\"res\",\"id\":7,\"ok\":true,\"r\":{\"uptime_s\":12}}");

    check_json(link_encode_err(3, "unknown_method", "no such method"),
               "{\"t\":\"res\",\"id\":3,\"ok\":false,\"e\":{\"code\":\"unknown_method\",\"msg\":\"no such method\"}}");

    cJSON *p = cJSON_CreateObject();
    cJSON *lines = cJSON_AddArrayToObject(p, "lines");
    cJSON_AddItemToArray(lines, cJSON_CreateString("I (1234) main: \"quoted\""));
    check_json(link_encode_evt("log.batch", p),
               "{\"t\":\"evt\",\"m\":\"log.batch\",\"p\":{\"lines\":[\"I (1234) main: \\\"quoted\\\"\"]}}");

    /* 16 KB is the limit, inclusive. The envelope around an empty "s" is the overhead. */
    cJSON *e0 = cJSON_CreateObject();
    cJSON_AddStringToObject(e0, "s", "");
    char *empty = link_encode_evt("x", e0);
    size_t overhead = strlen(empty);
    cJSON_free(empty);

    char *fill = malloc(LINK_MAX_MSG + 2);
    memset(fill, 'a', LINK_MAX_MSG + 1);
    fill[LINK_MAX_MSG - overhead] = '\0';
    cJSON *at = cJSON_CreateObject();
    cJSON_AddStringToObject(at, "s", fill);
    char *s = link_encode_evt("x", at);
    CHECK(s && strlen(s) == LINK_MAX_MSG);
    cJSON_free(s);

    fill[LINK_MAX_MSG - overhead] = 'a';
    fill[LINK_MAX_MSG - overhead + 1] = '\0';
    cJSON *over = cJSON_CreateObject();
    cJSON_AddStringToObject(over, "s", fill);
    CHECK(link_encode_evt("x", over) == NULL);
    cJSON *req_over = cJSON_CreateObject();
    cJSON_AddStringToObject(req_over, "s", fill);
    CHECK(link_encode_req(1, "x", req_over) == NULL);
    free(fill);
}

static void test_parse(void)
{
    link_msg_t m;
    cJSON *root;

    root = parse("{\"t\":\"req\",\"id\":5,\"m\":\"device.ping\",\"p\":{}}", &m);
    CHECK(root && m.kind == LINK_REQ && m.id == 5 && cJSON_IsObject(m.body));
    CHECK_STR(m.name, "device.ping");
    cJSON_Delete(root);

    /* A missing "p" reads as {}. */
    root = parse("{\"t\":\"req\",\"id\":6,\"m\":\"device.reboot\"}", &m);
    CHECK(root && cJSON_IsObject(m.body) && cJSON_GetArraySize(m.body) == 0);
    cJSON_Delete(root);

    /* Answerable but bad: the caller replies bad_request to id 9. */
    root = parse("{\"t\":\"req\",\"id\":9,\"m\":\"device.config\",\"p\":[1]}", &m);
    CHECK(root && m.kind == LINK_REQ && m.id == 9 && m.body == NULL);
    cJSON_Delete(root);
    root = parse("{\"t\":\"req\",\"id\":9,\"p\":{}}", &m);
    CHECK(root && m.id == 9 && m.name == NULL);
    cJSON_Delete(root);

    /* No usable id: can't be answered. */
    CHECK(parse("{\"t\":\"req\",\"m\":\"device.ping\"}", &m) == NULL);
    CHECK(parse("{\"t\":\"req\",\"id\":\"5\",\"m\":\"device.ping\"}", &m) == NULL);
    CHECK(parse("{\"t\":\"req\",\"id\":-1,\"m\":\"device.ping\"}", &m) == NULL);
    CHECK(parse("{\"t\":\"req\",\"id\":1.5,\"m\":\"device.ping\"}", &m) == NULL);
    CHECK(parse("{\"t\":\"req\",\"id\":4294967296,\"m\":\"device.ping\"}", &m) == NULL);

    root = parse("{\"t\":\"res\",\"id\":1,\"ok\":true,\"r\":{\"nonce_h\":\"ab\"}}", &m);
    CHECK(root && m.kind == LINK_RES && m.id == 1 && m.ok && m.code == NULL);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(m.body, "nonce_h")), "ab");
    cJSON_Delete(root);

    root = parse("{\"t\":\"res\",\"id\":2,\"ok\":true}", &m);
    CHECK(root && m.ok && cJSON_IsObject(m.body));
    cJSON_Delete(root);

    root = parse("{\"t\":\"res\",\"id\":1,\"ok\":false,\"e\":{\"code\":\"unauthorized\",\"msg\":\"unknown device\"}}", &m);
    CHECK(root && m.kind == LINK_RES && !m.ok && cJSON_IsObject(m.body));
    CHECK_STR(m.code, "unauthorized");
    cJSON_Delete(root);

    CHECK(parse("{\"t\":\"res\",\"id\":1,\"ok\":false}", &m) == NULL);
    CHECK(parse("{\"t\":\"res\",\"id\":1,\"ok\":false,\"e\":{\"msg\":\"x\"}}", &m) == NULL);
    CHECK(parse("{\"t\":\"res\",\"id\":1,\"r\":{}}", &m) == NULL);
    CHECK(parse("{\"t\":\"res\",\"ok\":true,\"r\":{}}", &m) == NULL);

    root = parse("{\"t\":\"evt\",\"m\":\"pair.done\",\"p\":{\"token\":\"" TOKEN "\"}}", &m);
    CHECK(root && m.kind == LINK_EVT);
    CHECK_STR(m.name, "pair.done");
    cJSON_Delete(root);
    root = parse("{\"t\":\"evt\",\"m\":\"ota.available\"}", &m);
    CHECK(root && cJSON_IsObject(m.body));
    cJSON_Delete(root);
    CHECK(parse("{\"t\":\"evt\",\"p\":{}}", &m) == NULL);
    CHECK(parse("{\"t\":\"evt\",\"m\":\"x\",\"p\":\"y\"}", &m) == NULL);
    root = link_parse_dispatch("{\"t\":\"evt\",\"m\":\"home.snapshot\",\"p\":null}",
        strlen("{\"t\":\"evt\",\"m\":\"home.snapshot\",\"p\":null}"), &m);
    CHECK(root && m.kind == LINK_EVT && m.invalid && !m.body);
    cJSON_Delete(root);
    root = link_parse_dispatch("{\"t\":\"res\",\"id\":2,\"ok\":\"bad\"}",
        strlen("{\"t\":\"res\",\"id\":2,\"ok\":\"bad\"}"), &m);
    CHECK(root && m.kind == LINK_RES && m.id == 2 && m.invalid);
    cJSON_Delete(root);

    CHECK(parse("{\"t\":\"bin\",\"id\":1}", &m) == NULL);
    CHECK(parse("{\"id\":1}", &m) == NULL);
    CHECK(parse("[\"t\",\"req\"]", &m) == NULL);
    CHECK(parse("not json", &m) == NULL);
    CHECK(parse("", &m) == NULL);
    CHECK(parse("{\"t\":\"evt\",\"m\":\"x\"} garbage", &m) == NULL);
    CHECK(parse("{\"t\":\"evt\",\"m\":\"x\\u0000suffix\"}", &m) == NULL);
    const char escaped_nul[] = "{\"t\":\"evt\",\"m\":\"home.update\",\"p\":{\"id\":\"a\\u0000suffix\"}}";
    CHECK(link_parse(escaped_nul, sizeof(escaped_nul) - 1, &m) == NULL);
    root = link_parse_dispatch(escaped_nul, sizeof(escaped_nul) - 1, &m);
    CHECK(root && m.kind == LINK_EVT && m.invalid);
    cJSON_Delete(root);
    const char raw_nul[] = "{\"t\":\"evt\",\"m\":\"home.snapshot\0suffix\",\"p\":{}}";
    CHECK(link_parse(raw_nul, sizeof(raw_nul) - 1, &m) == NULL);
    CHECK(link_parse_dispatch(raw_nul, sizeof(raw_nul) - 1, &m) == NULL);
    CHECK(parse("{\"t\":\"evt\",\"m\":\"x\",\"p\":{\"id\":\"a\ntruncated\"}}", &m) == NULL);
    root = parse("{\"t\":\"evt\",\"m\":\"x\",\"p\":{\"text\":\"escaped\\nline\"}}", &m);
    CHECK(root != NULL);
    cJSON_Delete(root);
    root = parse("{\"t\":\"evt\",\"m\":\"literal\\\\u0000\"} \n", &m);
    CHECK(root != NULL);
    cJSON_Delete(root);

    /* Frames aren't NUL-terminated: only `len` bytes count. */
    const char *two = "{\"t\":\"evt\",\"m\":\"a\"}{\"t\":\"evt\",\"m\":\"b\"}";
    root = link_parse(two, 19, &m);
    CHECK(root && strcmp(m.name, "a") == 0);
    cJSON_Delete(root);

    /* Over 16 KB is dropped unread. */
    size_t big = LINK_MAX_MSG + 1;
    char *text = malloc(big);
    memset(text, ' ', big);
    memcpy(text, "{\"t\":\"evt\",\"m\":\"x\"}", 19);
    CHECK(link_parse(text, big, &m) == NULL);
    root = link_parse(text, LINK_MAX_MSG, &m);
    CHECK(root != NULL);
    cJSON_Delete(root);
    free(text);
}

#define EVT_HEAD "{\"t\":\"evt\",\"m\":\"x\",\"p\":{\"a\":"

/* An event whose "p" holds `levels` nested brackets, so the frame nests levels + 2 deep. */
static char *nested(int levels, char open, char close)
{
    size_t head = strlen(EVT_HEAD), n = head + 2 * (size_t)levels;
    char *s = malloc(n + 3);
    memcpy(s, EVT_HEAD, head);
    memset(s + head, open, levels);
    memset(s + head + levels, close, levels);
    memcpy(s + n, "}}", 3);
    return s;
}

/* cJSON recurses per level; deeper frames would overflow the link task's stack. */
static void test_depth(void)
{
    link_msg_t m;
    cJSON *root;

    char *at = nested(LINK_MAX_DEPTH - 2, '[', ']');
    root = parse(at, &m);
    CHECK(root && m.kind == LINK_EVT);
    cJSON_Delete(root);
    free(at);

    char *over = nested(LINK_MAX_DEPTH - 1, '[', ']');
    CHECK(parse(over, &m) == NULL);
    free(over);

    /* 900 levels, which cJSON on its own accepts (its limit is 1000). */
    char *deep = nested(900, '[', ']');
    CHECK(parse(deep, &m) == NULL);
    free(deep);

    /* Brackets in a string aren't nesting, also after an escaped quote, and
     * an escaped backslash doesn't escape the closing quote. */
    char text[256];
    int n = snprintf(text, sizeof(text), EVT_HEAD "\"\\\"");
    memset(text + n, '[', 2 * LINK_MAX_DEPTH);
    snprintf(text + n + 2 * LINK_MAX_DEPTH, sizeof(text) - n - 2 * LINK_MAX_DEPTH, "\\\\\"}}");
    root = parse(text, &m);
    CHECK(root && strlen(cJSON_GetStringValue(cJSON_GetObjectItem(m.body, "a"))) == 2 * LINK_MAX_DEPTH + 2);
    cJSON_Delete(root);

    /* Only `len` bytes count, as for the parse itself. */
    const char *tail = "{\"t\":\"evt\",\"m\":\"x\"}[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[";
    root = link_parse(tail, 19, &m);
    CHECK(root != NULL);
    cJSON_Delete(root);
}

static void test_duplicate_envelopes(void)
{
    const char *invalid[] = {
        "{\"t\":\"evt\",\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{}}",
        "{\"t\":\"evt\",\"m\":\"weather.state\",\"m\":\"ota.available\",\"p\":{}}",
        "{\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{},\"p\":{}}",
        "{\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{},\"id\":1,\"id\":2}",
        "{\"t\":\"evt\",\"m\":\"weather.state\",\"\\u006d\":\"weather.state\",\"p\":{}}",
        "{\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{},\"future\":1,\"future\":1}",
        "{\"t\":\"res\",\"id\":1,\"id\":2,\"ok\":true,\"r\":{}}",
        "{\"t\":\"res\",\"id\":1,\"ok\":true,\"r\":{},\"r\":{}}"
    };
    link_msg_t message;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        CHECK(parse(invalid[i], &message) == NULL);
        CHECK(link_parse_dispatch(invalid[i], strlen(invalid[i]), &message) == NULL);
    }
    const char *additive = "{\"t\":\"evt\",\"m\":\"weather.state\",\"p\":{},\"future\":{\"version\":2}}";
    cJSON *root = parse(additive, &message);
    CHECK(root && message.kind == LINK_EVT && !message.invalid);
    CHECK_STR(message.name, "weather.state");
    cJSON_Delete(root);
    root = link_parse_dispatch(additive, strlen(additive), &message);
    CHECK(root && message.kind == LINK_EVT && !message.invalid);
    cJSON_Delete(root);
}

static const link_device_t s_dev = {
    .device_id = "orb-020000abcdef",
    .name = "Orb",
    .model = "waveshare-s3-amoled-1.75c",
    .fw = "0.2.0",
    .build = "d7b73f7",
    .idf = "v6.1",
    .boot_slot = "ota_0",
    .reset_reason = "poweron",
    .uptime_s = 12,
};

static void check_body(cJSON *body, const char *want)
{
    char *s = cJSON_PrintUnformatted(body);
    CHECK_STR(s, want);
    cJSON_free(s);
    cJSON_Delete(body);
}

static void test_bodies(void)
{
    static const uint8_t nonce_d[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    uint8_t commit_d[32];
    CHECK(link_unhex("be45cb2605bf36bebde684841a28f0fd43c69850a3dce5fedba69928ee3a8991", commit_d, 32));
    check_body(link_pair_begin_params(&s_dev, commit_d),
               "{\"device_id\":\"orb-020000abcdef\",\"name\":\"Orb\",\"model\":\"waveshare-s3-amoled-1.75c\","
               "\"fw\":\"0.2.0\",\"commit_d\":\"be45cb2605bf36bebde684841a28f0fd43c69850a3dce5fedba69928ee3a8991\"}");
    check_body(link_pair_reveal_params(nonce_d), "{\"nonce_d\":\"000102030405060708090a0b0c0d0e0f\"}");

    const link_power_t power = {
        .battery_pct = 100, .battery_mv = 4020, .display_state = "active", .charging = false, .usb = true };
    check_body(link_hello_params(&s_dev, TOKEN, &power, -54),
               "{\"device_id\":\"orb-020000abcdef\",\"token\":\"" TOKEN "\",\"fw\":\"0.2.0\",\"build\":\"d7b73f7\","
               "\"idf\":\"v6.1\",\"boot_slot\":\"ota_0\",\"reset_reason\":\"poweron\",\"uptime_s\":12,"
               "\"power\":{\"battery_pct\":100,\"battery_mv\":4020,\"display_state\":\"active\",\"charging\":false,"
               "\"usb\":true},\"rssi\":-54,\"caps\":[\"cards\",\"voice\",\"usage\",\"nowplaying\",\"weather\"]}");
    check_body(link_hello_params(&s_dev, TOKEN, NULL, -54),
               "{\"device_id\":\"orb-020000abcdef\",\"token\":\"" TOKEN "\",\"fw\":\"0.2.0\",\"build\":\"d7b73f7\","
               "\"idf\":\"v6.1\",\"boot_slot\":\"ota_0\",\"reset_reason\":\"poweron\",\"uptime_s\":12,\"rssi\":-54,\"caps\":[\"cards\",\"voice\",\"usage\",\"nowplaying\",\"weather\"]}");

    const link_power_t battery = {
        .battery_pct = 41, .battery_mv = 3900, .display_state = "dimmed", .charging = true, .usb = true };
    check_body(link_state_report_params(&battery, -61, 78000, 7900000, 3600),
               "{\"power\":{\"battery_pct\":41,\"battery_mv\":3900,\"display_state\":\"dimmed\",\"charging\":true,"
               "\"usb\":true},\"rssi\":-61,\"heap_internal\":78000,\"heap_psram\":7900000,\"uptime_s\":3600}");
    check_body(link_state_report_params(NULL, 0, 1, 2, 3),
               "{\"rssi\":0,\"heap_internal\":1,\"heap_psram\":2,\"uptime_s\":3}");
}

static void test_results(void)
{
    link_msg_t m;
    cJSON *root;

    link_pair_begin_result_t pb;
    root = parse("{\"t\":\"res\",\"id\":1,\"ok\":true,\"r\":{\"nonce_h\":\"101112131415161718191A1B1C1D1E1F\","
                 "\"hub_name\":\"Mac mini\"}}",
                 &m);
    CHECK(link_read_pair_begin(m.body, &pb));
    CHECK(pb.nonce_h[0] == 0x10 && pb.nonce_h[15] == 0x1f);
    CHECK_STR(pb.hub_name, "Mac mini");
    cJSON_Delete(root);
    /* A name longer than its field ends at a word with one ellipsis (polish #10). */
    root = parse("{\"t\":\"res\",\"id\":1,\"ok\":true,\"r\":{\"nonce_h\":\"101112131415161718191A1B1C1D1E1F\","
                 "\"hub_name\":\"A Mac computer with a maximum length name and many complete words\"}}",
                 &m);
    CHECK(link_read_pair_begin(m.body, &pb));
    CHECK_STR(pb.hub_name, "A Mac computer with a maximum length name and many complete\xE2\x80\xA6");
    cJSON_Delete(root);
    root = parse("{\"t\":\"res\",\"id\":1,\"ok\":true,\"r\":{\"nonce_h\":\"101112131415161718191A1B1C1D1E1F\","
                 "\"hub_name\":\"\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9"
                 "\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\"}}",
                 &m);
    CHECK(link_read_pair_begin(m.body, &pb));
    CHECK(strlen(pb.hub_name) < sizeof(pb.hub_name) && !strcmp(pb.hub_name + strlen(pb.hub_name) - 3, "\xE2\x80\xA6"));
    CHECK(((unsigned char)pb.hub_name[strlen(pb.hub_name) - 4] & 0xC0) == 0x80); /* ends after a whole é */
    cJSON_Delete(root);
    root = parse("{\"t\":\"res\",\"id\":1,\"ok\":true,\"r\":{\"nonce_h\":\"10111213\"}}", &m);
    CHECK(!link_read_pair_begin(m.body, &pb));
    cJSON_Delete(root);

    char token[65];
    root = parse("{\"t\":\"evt\",\"m\":\"pair.done\",\"p\":{\"token\":\"" TOKEN "\"}}", &m);
    CHECK(link_read_pair_done(m.body, token));
    CHECK_STR(token, TOKEN);
    cJSON_Delete(root);
    root = parse("{\"t\":\"evt\",\"m\":\"pair.done\",\"p\":{\"token\":\"0011\"}}", &m);
    CHECK(!link_read_pair_done(m.body, token));
    cJSON_Delete(root);
    root = parse("{\"t\":\"evt\",\"m\":\"pair.done\",\"p\":{\"token\":\"zz112233445566778899aabbccddeeff00112233445566778899"
                 "aabbccddeeff\"}}",
                 &m);
    CHECK(!link_read_pair_done(m.body, token));
    cJSON_Delete(root);
    root = parse("{\"t\":\"evt\",\"m\":\"pair.done\",\"p\":{}}", &m);
    CHECK(!link_read_pair_done(m.body, token));
    cJSON_Delete(root);

    link_hello_result_t h;
    root = parse("{\"t\":\"res\",\"id\":1,\"ok\":true,\"r\":{\"hub_name\":\"Mac mini\",\"hub_version\":\"0.1.0\","
                 "\"time_ms\":1793050000000,\"hour24\":false,\"tz\":\"America/New_York\","
                 "\"tz_posix\":\"EST5EDT,M3.2.0,M11.1.0\",\"device\":{\"name\":\"Orb\",\"channel\":\"dev\"}}}",
                 &m);
    link_read_hello(m.body, &h);
    CHECK_STR(h.hub_name, "Mac mini");
    CHECK_STR(h.hub_version, "0.1.0");
    CHECK(h.time_ms == 1793050000000LL);
    CHECK(h.have_hour24 && !h.hour24);
    CHECK_STR(h.tz, "America/New_York");
    CHECK_STR(h.tz_posix, "EST5EDT,M3.2.0,M11.1.0");
    CHECK_STR(h.device_name, "Orb");
    CHECK_STR(h.channel, "dev");
    cJSON_Delete(root);
    root = parse("{\"t\":\"res\",\"id\":1,\"ok\":true,\"r\":{}}", &m);
    link_read_hello(m.body, &h);
    CHECK(h.time_ms == 0 && h.tz[0] == '\0' && h.tz_posix[0] == '\0' && h.device_name[0] == '\0');
    CHECK(!h.have_hour24);
    cJSON_Delete(root);
    static const char *const hour_fields[] = {"true", "1", "\"false\"", "null"};
    for (unsigned i = 0; i < sizeof(hour_fields) / sizeof(hour_fields[0]); ++i) {
        char json[96];
        snprintf(json, sizeof(json), "{\"t\":\"res\",\"id\":1,\"ok\":true,\"r\":{\"hour24\":%s}}", hour_fields[i]);
        root = parse(json, &m); link_read_hello(m.body, &h);
        CHECK(h.have_hour24 == (i == 0));
        CHECK(h.hour24 == (i == 0));
        cJSON_Delete(root);
    }
}

static void test_hex(void)
{
    uint8_t b[4];
    char s[9];
    CHECK(link_unhex("00fFa5", b, 3) && b[0] == 0 && b[1] == 0xff && b[2] == 0xa5);
    link_hex(b, 3, s);
    CHECK_STR(s, "00ffa5");
    CHECK(!link_unhex("00f", b, 2));
    CHECK(!link_unhex("00fg", b, 2));
    CHECK(!link_unhex(NULL, b, 2));
}

void test_proto(void)
{
    test_encode();
    test_parse();
    test_duplicate_envelopes();
    test_depth();
    test_bodies();
    test_results();
    test_hex();
}
