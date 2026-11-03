#include "link_proto.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const cJSON *item(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

/* Envelope routing must be unambiguous even when a duplicate key has the same
 * value. Check decoded names so escaped spellings cannot alias m, p or id. */
static bool envelope_ok(const cJSON *root)
{
    if (!cJSON_IsObject(root)) return false;
    for (const cJSON *a = root->child; a; a = a->next) {
        if (!a->string) return false;
        for (const cJSON *b = a->next; b; b = b->next)
            if (!b->string || !strcmp(a->string, b->string)) return false;
    }
    return true;
}

bool link_batch_valid(const cJSON *params)
{
    if (!envelope_ok(params)) return false;
    const cJSON *events = item(params, "events"), *event;
    if (!cJSON_IsArray(events)) return false;
    cJSON_ArrayForEach(event, events) {
        if (!envelope_ok(event) || cJSON_GetArraySize(event) != 2) return false;
        const char *name = cJSON_GetStringValue(item(event, "m"));
        if (!name || !name[0] || !strcmp(name, "power.batch") || !cJSON_IsObject(item(event, "p"))) return false;
    }
    return true;
}

static void copy_str(const cJSON *obj, const char *key, char *out, size_t n)
{
    const char *s = cJSON_GetStringValue(item(obj, key));
    snprintf(out, n, "%s", s ? s : "");
}

/* Display names are content, not keys (polish review #10): one longer than its
 * field ends at a word with "…", never mid-word or inside a UTF-8 sequence. */
static void copy_name(const cJSON *obj, const char *key, char *out, size_t n)
{
    const char *s = cJSON_GetStringValue(item(obj, key));
    size_t length = s ? strlen(s) : 0, cut = n - 4; /* room for "…" and NUL */
    if (length < n) { snprintf(out, n, "%s", s ? s : ""); return; }
    while (cut && s[cut] != ' ') cut--;
    for (cut = cut ? cut : n - 4; cut && ((unsigned char)s[cut] & 0xC0) == 0x80; cut--) {}
    memcpy(out, s, cut);
    memcpy(out + cut, "\xE2\x80\xA6", 4);
}

/* Ids are the sender's own counter, so anything but a small whole number is garbage. */
static bool read_id(const cJSON *root, int *id)
{
    const cJSON *v = item(root, "id");
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > INT32_MAX ||
        v->valuedouble != (double)(int32_t)v->valuedouble) {
        return false;
    }
    *id = (int)v->valuedouble;
    return true;
}

/* "p" or "r": the object, {} when absent, NULL when it's something else. */
static const cJSON *read_body(cJSON *root, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!v) {
        return cJSON_AddObjectToObject(root, key);
    }
    return cJSON_IsObject(v) ? v : NULL;
}

/*
 * cJSON parses each nested array or object with a recursive call, about 80
 * bytes of stack a level on the device, and allows 1000 levels: a couple of
 * hundred bytes of brackets would overflow the link task's stack. So the
 * nesting is counted first, skipping strings as cJSON does. Up to where cJSON
 * stops, the text is valid JSON and this count is cJSON's depth.
 */
static bool depth_ok(const char *text, size_t len, bool *escaped_nul)
{
    int depth = 0;
    bool in_string = false, escaped = false;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (in_string) {
            /* cJSON accepts raw controls in strings, including embedded NUL.
             * Reject them before method/identity C strings can be truncated. */
            if ((unsigned char)c < 0x20) return false;
            if (escaped) {
                /* cJSON's C strings alias "id\\u0000suffix" to "id". The
                 * strict parser rejects it; dispatch may only classify the
                 * envelope and must never apply its truncated DOM strings. */
                if (c == 'u' && i + 4 < len && !memcmp(text + i + 1, "0000", 4)) {
                    if (!escaped_nul) return false;
                    *escaped_nul = true;
                }
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > LINK_MAX_DEPTH) {
                return false;
            }
        } else if (c == '}' || c == ']') {
            depth--;
        }
    }
    return true;
}

static cJSON *parse(const char *text, size_t len, link_msg_t *out, bool dispatch)
{
    memset(out, 0, sizeof(*out));
    bool escaped_nul = false;
    if (len > LINK_MAX_MSG || !depth_ok(text, len, dispatch ? &escaped_nul : NULL)) {
        return NULL;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, false);
    while (end && (size_t)(end - text) < len && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) ++end;
    if (!root || !end || (size_t)(end - text) != len || !envelope_ok(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    /* NULL unless root is an object with a string "t"; then nothing below matches. */
    const char *t = cJSON_GetStringValue(item(root, "t"));
    t = t ? t : "";
    bool valid = false;
    if (strcmp(t, "req") == 0) {
        out->kind = LINK_REQ;
        out->name = cJSON_GetStringValue(item(root, "m"));
        out->body = read_body(root, "p");
        valid = read_id(root, &out->id);
    } else if (strcmp(t, "res") == 0) {
        const cJSON *ok = item(root, "ok");
        out->kind = LINK_RES;
        out->ok = cJSON_IsTrue(ok);
        if (out->ok) {
            out->body = read_body(root, "r");
        } else {
            const cJSON *e = item(root, "e");
            out->body = cJSON_IsObject(e) ? e : NULL;
            out->code = cJSON_GetStringValue(item(e, "code"));
        }
        valid = read_id(root, &out->id);
        out->invalid = !cJSON_IsBool(ok) || !out->body || (!out->ok && !out->code);
        valid = valid && (dispatch || !out->invalid);
    } else if (strcmp(t, "evt") == 0) {
        out->kind = LINK_EVT;
        out->name = cJSON_GetStringValue(item(root, "m"));
        out->body = read_body(root, "p");
        out->invalid = !out->body;
        valid = out->name && (dispatch || !out->invalid);
    }
    out->invalid |= escaped_nul;
    if (!valid) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

cJSON *link_parse(const char *text, size_t len, link_msg_t *out)
{
    return parse(text, len, out, false);
}

cJSON *link_parse_dispatch(const char *text, size_t len, link_msg_t *out)
{
    return parse(text, len, out, true);
}

/* Adds the body under `key` and prints. `ok` says the envelope's other fields went in. */
static char *finish(cJSON *root, bool ok, const char *key, cJSON *body)
{
    if (!body) {
        body = cJSON_CreateObject();
    }
    if (!ok || !body || !cJSON_AddItemToObject(root, key, body)) {
        cJSON_Delete(body);
        cJSON_Delete(root);
        return NULL;
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s && strlen(s) > LINK_MAX_MSG) {
        cJSON_free(s);
        return NULL;
    }
    return s;
}

char *link_encode_req(int id, const char *method, cJSON *params)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = cJSON_AddStringToObject(root, "t", "req") && cJSON_AddNumberToObject(root, "id", id) &&
              cJSON_AddStringToObject(root, "m", method);
    return finish(root, ok, "p", params);
}

char *link_encode_res(int id, cJSON *result)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = cJSON_AddStringToObject(root, "t", "res") && cJSON_AddNumberToObject(root, "id", id) &&
              cJSON_AddTrueToObject(root, "ok");
    return finish(root, ok, "r", result);
}

char *link_encode_err(int id, const char *code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *e = cJSON_CreateObject();
    bool ok = cJSON_AddStringToObject(root, "t", "res") && cJSON_AddNumberToObject(root, "id", id) &&
              cJSON_AddFalseToObject(root, "ok") && cJSON_AddStringToObject(e, "code", code) &&
              cJSON_AddStringToObject(e, "msg", msg);
    return finish(root, ok, "e", e);
}

char *link_encode_evt(const char *name, cJSON *params)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = cJSON_AddStringToObject(root, "t", "evt") && cJSON_AddStringToObject(root, "m", name);
    return finish(root, ok, "p", params);
}

void link_hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xf];
    }
    out[2 * n] = '\0';
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool link_unhex(const char *hex, uint8_t *out, size_t n)
{
    if (!hex || strlen(hex) != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static bool add_power(cJSON *obj, const link_power_t *power)
{
    if (!power) {
        return true;
    }
    cJSON *p = cJSON_AddObjectToObject(obj, "power");
    return p && cJSON_AddNumberToObject(p, "battery_pct", power->battery_pct) &&
           cJSON_AddNumberToObject(p, "battery_mv", power->battery_mv) &&
           cJSON_AddStringToObject(p, "display_state", power->display_state ? power->display_state : "active") &&
           cJSON_AddBoolToObject(p, "charging", power->charging) && cJSON_AddBoolToObject(p, "usb", power->usb);
}

/* Builders chain cJSON_Add* with &&, so one failed allocation discards the whole body. */
static cJSON *checked(cJSON *obj, bool ok)
{
    if (!ok) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

cJSON *link_pair_begin_params(const link_device_t *dev, const uint8_t commit_d[32])
{
    char commit[65];
    link_hex(commit_d, 32, commit);
    cJSON *p = cJSON_CreateObject();
    bool ok = cJSON_AddStringToObject(p, "device_id", dev->device_id) && cJSON_AddStringToObject(p, "name", dev->name) &&
              cJSON_AddStringToObject(p, "model", dev->model) && cJSON_AddStringToObject(p, "fw", dev->fw) &&
              cJSON_AddStringToObject(p, "commit_d", commit);
    return checked(p, ok);
}

cJSON *link_pair_reveal_params(const uint8_t nonce_d[16])
{
    char nonce[33];
    link_hex(nonce_d, 16, nonce);
    cJSON *p = cJSON_CreateObject();
    return checked(p, cJSON_AddStringToObject(p, "nonce_d", nonce));
}

cJSON *link_hello_params(const link_device_t *dev, const char *token, const link_power_t *power, int rssi)
{
    cJSON *p = cJSON_CreateObject();
    bool ok = cJSON_AddStringToObject(p, "device_id", dev->device_id) && cJSON_AddStringToObject(p, "token", token) &&
              cJSON_AddStringToObject(p, "fw", dev->fw) && cJSON_AddStringToObject(p, "build", dev->build) &&
              cJSON_AddStringToObject(p, "idf", dev->idf) && cJSON_AddStringToObject(p, "boot_slot", dev->boot_slot) &&
              cJSON_AddStringToObject(p, "reset_reason", dev->reset_reason) &&
              cJSON_AddNumberToObject(p, "uptime_s", dev->uptime_s) && add_power(p, power) &&
              cJSON_AddNumberToObject(p, "rssi", rssi);
    cJSON *caps = ok ? cJSON_AddArrayToObject(p, "caps") : NULL;
    ok = caps && cJSON_AddItemToArray(caps, cJSON_CreateString("cards")) &&
         cJSON_AddItemToArray(caps, cJSON_CreateString("voice")) &&
         cJSON_AddItemToArray(caps, cJSON_CreateString("usage")) &&
         cJSON_AddItemToArray(caps, cJSON_CreateString("nowplaying")) &&
         cJSON_AddItemToArray(caps, cJSON_CreateString("weather"));
    return checked(p, ok);
}

cJSON *link_state_report_params(const link_power_t *power, int rssi, uint32_t heap_internal, uint32_t heap_psram,
                                uint32_t uptime_s)
{
    cJSON *p = cJSON_CreateObject();
    bool ok = add_power(p, power) && cJSON_AddNumberToObject(p, "rssi", rssi) &&
              cJSON_AddNumberToObject(p, "heap_internal", heap_internal) &&
              cJSON_AddNumberToObject(p, "heap_psram", heap_psram) && cJSON_AddNumberToObject(p, "uptime_s", uptime_s);
    return checked(p, ok);
}

bool link_read_pair_begin(const cJSON *result, link_pair_begin_result_t *out)
{
    copy_name(result, "hub_name", out->hub_name, sizeof(out->hub_name));
    return link_unhex(cJSON_GetStringValue(item(result, "nonce_h")), out->nonce_h, sizeof(out->nonce_h));
}

bool link_read_pair_done(const cJSON *params, char token[65])
{
    const char *t = cJSON_GetStringValue(item(params, "token"));
    uint8_t raw[32];
    if (!link_unhex(t, raw, sizeof(raw))) {
        return false;
    }
    memcpy(token, t, 65);
    return true;
}

void link_read_hello(const cJSON *result, link_hello_result_t *out)
{
    const cJSON *device = item(result, "device");
    const cJSON *time_ms = item(result, "time_ms");
    const cJSON *hour24 = item(result, "hour24");
    copy_name(result, "hub_name", out->hub_name, sizeof(out->hub_name));
    copy_str(result, "hub_version", out->hub_version, sizeof(out->hub_version));
    copy_str(result, "tz", out->tz, sizeof(out->tz));
    copy_str(result, "tz_posix", out->tz_posix, sizeof(out->tz_posix));
    copy_name(device, "name", out->device_name, sizeof(out->device_name));
    copy_str(device, "channel", out->channel, sizeof(out->channel));
    out->time_ms = cJSON_IsNumber(time_ms) ? (int64_t)time_ms->valuedouble : 0;
    out->have_hour24 = cJSON_IsBool(hour24);
    out->hour24 = cJSON_IsTrue(hour24);
}
