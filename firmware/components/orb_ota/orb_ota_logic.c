#include "orb_ota_logic.h"

#include <stdio.h>
#include <string.h>

static bool fail(char *err, size_t err_len, const char *what)
{
    if (err && err_len) {
        snprintf(err, err_len, "bad offer: %s", what);
    }
    return false;
}

/* Offer strings end up in logs, on screen and in NVS: no control characters. */
static bool printable(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || *s == 0x7f) {
            return false;
        }
    }
    return true;
}

/* A member that must be a string when present; NULL when missing or null.
 * *bad is set when it is there with another type. */
static const char *str_field(const cJSON *p, const char *name, bool *bad)
{
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(p, name);
    *bad = f && !cJSON_IsNull(f) && !cJSON_IsString(f);
    return cJSON_IsString(f) ? f->valuestring : NULL;
}

static bool copy_field(char *dst, size_t cap, const char *src, bool required)
{
    dst[0] = '\0';
    if (!src) {
        return !required;
    }
    size_t n = strlen(src);
    if ((required && n == 0) || n >= cap || !printable(src)) {
        return false;
    }
    memcpy(dst, src, n + 1);
    return true;
}

static int hex_val(char c)
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

bool orb_ota_is_sha_hex(const char *s)
{
    if (!s) {
        return false;
    }
    size_t i = 0;
    for (; s[i]; i++) {
        if (i >= ORB_OTA_SHA_HEX || hex_val(s[i]) < 0) {
            return false;
        }
    }
    return i == ORB_OTA_SHA_HEX;
}

bool orb_ota_sha_equal(const char *a, const char *b)
{
    if (!orb_ota_is_sha_hex(a) || !orb_ota_is_sha_hex(b)) {
        return false;
    }
    for (int i = 0; i < ORB_OTA_SHA_HEX; i++) {
        if (hex_val(a[i]) != hex_val(b[i])) {
            return false;
        }
    }
    return true;
}

bool orb_ota_digest_matches(const uint8_t digest[32], const char *hex)
{
    if (!orb_ota_is_sha_hex(hex)) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        if (((hex_val(hex[2 * i]) << 4) | hex_val(hex[2 * i + 1])) != digest[i]) {
            return false;
        }
    }
    return true;
}

size_t orb_ota_copy_utf8(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return 0;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        /* src[n] is the first byte left out; if it continues a character, drop
         * that character's first bytes too. */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

/* "https://host[:port]/path" → "/path". */
static bool url_path(const char *url, char *out, size_t cap)
{
    static const char scheme[] = "https://";
    if (!url || strncmp(url, scheme, sizeof(scheme) - 1) != 0) {
        return false;
    }
    const char *host = url + sizeof(scheme) - 1;
    const char *path = strchr(host, '/');
    if (!path || path == host || strchr(path, ' ')) {
        return false;
    }
    return copy_field(out, cap, path, true);
}

bool orb_ota_offer_parse(const cJSON *p, orb_ota_offer_t *o, char *err, size_t err_len)
{
    memset(o, 0, sizeof(*o));
    if (!cJSON_IsObject(p)) {
        return fail(err, err_len, "not an object");
    }
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(p, "kind");
    if (kind && (!cJSON_IsString(kind) ||
                 (strcmp(kind->valuestring, "app") != 0 && strcmp(kind->valuestring, "assets") != 0))) {
        return fail(err, err_len, "kind");
    }
    o->assets = kind && strcmp(kind->valuestring, "assets") == 0;
    const cJSON *available = cJSON_GetObjectItemCaseSensitive(p, "available");
    if (!cJSON_IsBool(available)) {
        return fail(err, err_len, "available");
    }
    o->available = cJSON_IsTrue(available);
    if (!o->available) {
        return true;
    }

    bool bad;
    const char *version = str_field(p, "version", &bad);
    if (bad || !copy_field(o->version, sizeof(o->version), version, true)) {
        return fail(err, err_len, "version");
    }
    const char *build = str_field(p, "build", &bad);
    if (bad || !copy_field(o->build, sizeof(o->build), build, false)) {
        return fail(err, err_len, "build");
    }

    const cJSON *size = cJSON_GetObjectItemCaseSensitive(p, "size");
    if (!cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > ORB_OTA_MAX_IMAGE ||
        size->valuedouble != (double)(uint32_t)size->valuedouble) {
        return fail(err, err_len, "size");
    }
    o->size = (uint32_t)size->valuedouble;
    if (o->assets && (o->size > 8 * 1024 * 1024 || o->size < 8192 || o->size % 4096 != 0)) {
        return fail(err, err_len, "assets size");
    }

    const char *sha = str_field(p, "sha256", &bad);
    if (!orb_ota_is_sha_hex(sha)) {
        return fail(err, err_len, "sha256");
    }
    for (int i = 0; i <= ORB_OTA_SHA_HEX; i++) {
        o->sha256[i] = (sha[i] >= 'A' && sha[i] <= 'F') ? (char)(sha[i] - 'A' + 'a') : sha[i];
    }

    if (!url_path(str_field(p, "url", &bad), o->path, sizeof(o->path))) {
        return fail(err, err_len, "url");
    }

    const char *install = str_field(p, "install", &bad);
    if (install && strcmp(install, "now") == 0) {
        o->install = ORB_OTA_INSTALL_NOW;
    } else if (install && strcmp(install, "idle") == 0) {
        o->install = ORB_OTA_INSTALL_IDLE;
    } else if (install && strcmp(install, "ask") == 0) {
        o->install = ORB_OTA_INSTALL_ASK;
    } else {
        return fail(err, err_len, "install");
    }

    /* Notes are only shown (line breaks allowed), so long ones are cut rather than refused. */
    const char *notes = str_field(p, "notes", &bad);
    if (bad) {
        return fail(err, err_len, "notes");
    }
    if (notes) {
        orb_ota_copy_utf8(o->notes, sizeof(o->notes), notes);
    }
    return true;
}

orb_ota_action_t orb_ota_decide(const orb_ota_offer_t *o, const orb_ota_ctx_t *c)
{
    if (!o->available || c->busy || strcmp(o->version, c->running_version) == 0) {
        return ORB_OTA_IGNORE;
    }
    if (c->probation) {
        return ORB_OTA_HOLD;
    }
    if (c->tapped) {
        return ORB_OTA_INSTALL;
    }
    /* An image that already failed (or rolled back) waits for a tap, or the hub
     * would have the device fetch and fail it again after every hello. */
    if (orb_ota_sha_equal(o->sha256, c->bad_sha)) {
        return ORB_OTA_HOLD;
    }
    if (c->low_battery) {
        return ORB_OTA_HOLD;
    }
    switch (o->install) {
    case ORB_OTA_INSTALL_NOW:
        return ORB_OTA_INSTALL;
    case ORB_OTA_INSTALL_IDLE:
        return c->idle ? ORB_OTA_INSTALL : ORB_OTA_HOLD;
    case ORB_OTA_INSTALL_ASK:
    default:
        return ORB_OTA_HOLD;
    }
}

bool orb_ota_low_battery(int battery_pct, bool charging)
{
    return battery_pct >= 0 && battery_pct < ORB_OTA_MIN_BATTERY && !charging;
}

const char *orb_ota_result_name(orb_ota_result_t r)
{
    switch (r) {
    case ORB_OTA_RESULT_OK:
        return "ok";
    case ORB_OTA_RESULT_ROLLED_BACK:
        return "rolled_back";
    case ORB_OTA_RESULT_FAILED:
        return "failed";
    case ORB_OTA_RESULT_NONE:
    case ORB_OTA_RESULT_STALE:
    default:
        return NULL;
    }
}

orb_ota_result_t orb_ota_boot_outcome(const orb_ota_boot_t *b)
{
    if (!b->try_slot[0] || b->probation) {
        /* On probation the outcome is settled by mark_healthy or the deadline. */
        return ORB_OTA_RESULT_NONE;
    }
    if (strcmp(b->try_slot, b->running_slot) == 0) {
        /* Confirmed earlier, but the record wasn't cleared before a restart;
         * unless the slot has been flashed over USB since. */
        return strcmp(b->try_version, b->running_version) == 0 ? ORB_OTA_RESULT_OK : ORB_OTA_RESULT_STALE;
    }
    if (strcmp(b->try_slot, b->invalid_slot) == 0) {
        return ORB_OTA_RESULT_ROLLED_BACK;
    }
    if (strcmp(b->try_slot, b->boot_slot) == 0) {
        /* otadata selects it, yet the old slot runs: the bootloader couldn't
         * load it at all. */
        return ORB_OTA_RESULT_FAILED;
    }
    /* otadata never pointed at it (esp_ota_begin erased any older entry for
     * that slot): a reset before esp_https_ota_finish switched the boot slot,
     * or a USB flash that rewrote otadata. */
    return ORB_OTA_RESULT_STALE;
}

bool orb_ota_image_at_fault(orb_ota_result_t r, bool deadline_missed, bool external_reset)
{
    switch (r) {
    case ORB_OTA_RESULT_FAILED:
        return true;
    case ORB_OTA_RESULT_ROLLED_BACK:
        /* Without the deadline, the bootloader aborted it because the device
         * restarted on probation: its own crash or watchdog counts against it,
         * a power loss or a reset from outside doesn't. */
        return deadline_missed || !external_reset;
    default:
        return false;
    }
}

bool orb_ota_image_ok(const char *offer_version, const char *running_project, const char *project,
                      const char *version, char *err, size_t err_len)
{
    if (strcmp(project, running_project) != 0) {
        snprintf(err, err_len, "image is %.24s, not %.24s", project, running_project);
        return false;
    }
    if (strcmp(version, offer_version) != 0) {
        snprintf(err, err_len, "image is %.20s, offer said %.20s", version, offer_version);
        return false;
    }
    return true;
}

int orb_ota_percent(int64_t done, int64_t total)
{
    if (total <= 0 || done <= 0) {
        return 0;
    }
    if (done >= total) {
        return 100;
    }
    return (int)(done * 100 / total);
}

bool orb_ota_stalled(orb_ota_watch_t *w, int64_t read, int64_t now_us, int64_t limit_us)
{
    if (read != w->read) {
        w->read = read;
        w->since_us = now_us;
        return false;
    }
    return now_us - w->since_us >= limit_us;
}

void orb_ota_add_last_ota(cJSON *hello, const char *version, const char *result, const char *error)
{
    cJSON *last = cJSON_AddObjectToObject(hello, "last_ota");
    if (!last) {
        return;
    }
    cJSON_AddStringToObject(last, "version", version);
    cJSON_AddStringToObject(last, "result", result);
    if (error && error[0]) {
        cJSON_AddStringToObject(last, "error", error);
    }
}

cJSON *orb_ota_report(const char *version, const char *state, int progress, const char *error)
{
    cJSON *p = cJSON_CreateObject();
    if (!p) {
        return NULL;
    }
    cJSON_AddStringToObject(p, "version", version);
    cJSON_AddStringToObject(p, "state", state);
    cJSON_AddNumberToObject(p, "progress", progress);
    if (error && error[0]) {
        cJSON_AddStringToObject(p, "error", error);
    }
    return p;
}
