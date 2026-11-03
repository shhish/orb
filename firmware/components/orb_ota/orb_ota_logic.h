#pragma once

/*
 * The parts of orb_ota that don't touch ESP-IDF: reading offers, deciding what
 * to do with them, and the bookkeeping behind hello's last_ota. Kept free of
 * ESP-IDF so tests/host runs them on the Mac.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define ORB_OTA_SHA_HEX 64
/* No app slot is bigger; anything larger is a broken offer. */
#define ORB_OTA_MAX_IMAGE (16 * 1024 * 1024)

typedef enum {
    ORB_OTA_INSTALL_NOW,
    ORB_OTA_INSTALL_IDLE,
    ORB_OTA_INSTALL_ASK,
} orb_ota_install_t;

typedef struct {
    bool available;
    bool assets;                /* absent kind is the unchanged app channel */
    char version[32];           /* esp_app_desc_t.version is 32 bytes too */
    char build[41];
    uint32_t size;
    char sha256[ORB_OTA_SHA_HEX + 1];   /* lowercase hex of the whole .bin */
    char path[160];             /* the url's path: downloads always go to the paired hub */
    orb_ota_install_t install;
    char notes[128];
} orb_ota_offer_t;

/* Reads an offer (ota.available, or the ota.check result). False with a short
 * reason in err when anything is missing, the wrong type or out of range. */
bool orb_ota_offer_parse(const cJSON *p, orb_ota_offer_t *out, char *err, size_t err_len);

typedef enum {
    ORB_OTA_IGNORE,    /* nothing to do: not available, already running it, or busy */
    ORB_OTA_HOLD,      /* keep it as the waiting offer */
    ORB_OTA_INSTALL,
} orb_ota_action_t;

typedef struct {
    const char *running_version;
    const char *bad_sha;   /* last image that failed its checks or rolled back; "" if none */
    bool idle;             /* charging and untouched (orb_ota_set_idle) */
    bool low_battery;      /* orb_ota_low_battery */
    bool busy;             /* downloading, verifying or ready */
    bool probation;        /* this image isn't confirmed yet, and ESP-IDF refuses OTA until it is */
    bool tapped;           /* the user asked to install */
} orb_ota_ctx_t;

orb_ota_action_t orb_ota_decide(const orb_ota_offer_t *offer, const orb_ota_ctx_t *ctx);

/* Below this, and not charging, installs wait for a tap: a brownout on probation
 * would roll a good image back. */
#define ORB_OTA_MIN_BATTERY 30

/* battery_pct < 0: unknown or no battery, which never holds an install back. */
bool orb_ota_low_battery(int battery_pct, bool charging);

typedef enum {
    ORB_OTA_RESULT_NONE,   /* no attempt to report, or not decided yet */
    ORB_OTA_RESULT_STALE,  /* the attempt never took effect: drop it, report nothing */
    ORB_OTA_RESULT_OK,
    ORB_OTA_RESULT_ROLLED_BACK,
    ORB_OTA_RESULT_FAILED,
} orb_ota_result_t;

/* "ok", "rolled_back", "failed" (hello's last_ota.result); NULL for NONE and STALE. */
const char *orb_ota_result_name(orb_ota_result_t r);

/* What a boot knows about the last install. Slots are partition labels, "" for none. */
typedef struct {
    const char *try_slot;         /* the slot it was written to; "" when no attempt is recorded */
    const char *try_version;      /* the version it installed */
    const char *running_slot;
    const char *running_version;
    const char *boot_slot;        /* the slot otadata selects (esp_ota_get_boot_partition) */
    const char *invalid_slot;     /* ESP-IDF's last invalid (rolled back or aborted) slot */
    bool probation;
} orb_ota_boot_t;

orb_ota_result_t orb_ota_boot_outcome(const orb_ota_boot_t *b);

/*
 * Whether a result counts against the image, so that its SHA-256 then waits for
 * a tap. deadline_missed: orb_ota itself rolled it back. external_reset: this
 * boot follows a power loss, brownout or a reset from outside (pin, USB, JTAG),
 * which aborts an image on probation through no fault of its own.
 */
bool orb_ota_image_at_fault(orb_ota_result_t r, bool deadline_missed, bool external_reset);

/* The downloaded image's app descriptor must be this project and the offered version. */
bool orb_ota_image_ok(const char *offer_version, const char *running_project, const char *project,
                      const char *version, char *err, size_t err_len);

bool orb_ota_is_sha_hex(const char *s);
/* Case-insensitive; false when either isn't 64 hex digits. */
bool orb_ota_sha_equal(const char *a, const char *b);
bool orb_ota_digest_matches(const uint8_t digest[32], const char *hex);

/* Copies at most cap - 1 bytes without cutting a UTF-8 character in half. */
size_t orb_ota_copy_utf8(char *dst, size_t cap, const char *src);

/* 0-100; 0 when total is unknown. */
int orb_ota_percent(int64_t done, int64_t total);

/* Notices a download that stopped moving (a hub that hangs with the connection
 * still up never times it out). Start with { .read = -1 }. */
typedef struct {
    int64_t read;       /* bytes read when it last moved */
    int64_t since_us;   /* and when */
} orb_ota_watch_t;

/* True once read has stayed the same for limit_us. */
bool orb_ota_stalled(orb_ota_watch_t *w, int64_t read, int64_t now_us, int64_t limit_us);

/* hello's last_ota; error may be NULL or "" (then left out). */
void orb_ota_add_last_ota(cJSON *hello, const char *version, const char *result, const char *error);

/* ota.report params; error may be NULL or "" (then left out). NULL when out of memory. */
cJSON *orb_ota_report(const char *version, const char *state, int progress, const char *error);
