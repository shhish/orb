/*
 * Updates from the hub (docs/protocol.md, "Firmware downloads"). An offer is
 * acted on by its install policy; the image streams over pinned HTTPS into the
 * inactive slot, its SHA-256 is checked against the offer, and ESP-IDF checks
 * its signature in esp_ota_end (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT)
 * before the boot slot switches. The key ESP-IDF trusts is the one in the
 * running image's own signature block, so every image the device runs, USB
 * flashed ones included, must be signed with the same key (scripts/sign-app.sh).
 * The build refuses a configuration where ESP-IDF would skip that check.
 *
 * A new image boots on probation (ESP_OTA_IMG_PENDING_VERIFY) and has 60 s to
 * be marked healthy, or it is marked invalid and the previous slot boots. What
 * became of an install is kept in NVS until a hello the hub accepted carried it
 * as last_ota.
 */
#include "orb_ota.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "orb_link.h"
#include "orb_assets.h"
#include "orb_ota_logic.h"
#include "orb_ota_download.h"
#include "sdkconfig.h"

/* Fail closed. Without signature checks on update, esp_ota_end accepts any
 * image whose SHA-256 matches the hub's own offer; without rollback there is
 * no probation. sdkconfig.defaults doesn't override an existing sdkconfig, so
 * a merged README line alone isn't proof. */
#if !defined(CONFIG_SECURE_SIGNED_ON_UPDATE) || !defined(CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME) || \
    !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
#error "orb_ota needs RSA app signatures checked on update and app rollback (README, Required sdkconfig)"
#endif

static const char *TAG = "ota";

#define PROBATION_S 60
#define NVS_NS "orb_ota"
#define NET_CORE 0
#define INSTALL_STACK 8192      /* TLS handshake, then the RSA-3072 check in esp_ota_end */
#define INSTALL_PRIORITY 5
#define ROLLBACK_STACK 6144     /* marking invalid re-verifies the previous image */
#define ROLLBACK_PRIORITY 10
#define IMAGE_ID_LEN 16         /* hex digits of an image's ELF SHA-256 kept with bad_sha */
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* An install's outcome, waiting for a hello to carry it. */
typedef struct {
    char version[32];
    char result[16];
    char error[64];
} last_ota_t;

static SemaphoreHandle_t s_lock;
static orb_ota_status_t s_status;
static orb_ota_offer_t s_offer;
static bool s_have_offer;
static bool s_idle;
static bool s_low_battery;
static char s_bad_sha[ORB_OTA_SHA_HEX + 1];
static char s_bad_assets_sha[ORB_OTA_SHA_HEX + 1];
static last_ota_t s_last;
static bool s_last_sent;   /* a hello carried s_last (or this image's "ok") */
static orb_ota_cb_t s_cb;
static void *s_cb_ctx;
static esp_timer_handle_t s_deadline;

/* s_lock is never held while calling into orb_link, whose callbacks take it. */
static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static esp_err_t init_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static void notify(const orb_ota_status_t *st)
{
    if (s_cb) {
        s_cb(st, s_cb_ctx);
    }
}

/* ---- NVS: best effort, since a lost report must never block an update or a rollback ---- */

typedef struct {
    const char *key;
    const char *val;   /* NULL: erase */
} kv_t;

static void store(const kv_t *kv, size_t n)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        for (size_t i = 0; err == ESP_OK && i < n; i++) {
            if (kv[i].val) {
                err = nvs_set_str(h, kv[i].key, kv[i].val);
            } else if ((err = nvs_erase_key(h, kv[i].key)) == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs: %s", esp_err_to_name(err));
    }
}

static void load(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        out[0] = '\0';
    }
}

/* ---- Probation ---- */

static void rollback_task(void *arg)
{
    lock();
    if (!s_status.pending_verify) {
        /* orb_ota_mark_healthy got there first. */
        unlock();
        vTaskDelete(NULL);
    }
    const kv_t kv[] = { { "try_err", "not healthy within 60 s" } };
    store(kv, COUNT(kv));
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Only returns when ESP-IDF won't vouch for the previous slot (it checks that
     * image against this one's key). This image is still pending verify, so the
     * bootloader aborts it on restart and boots the previous slot anyway. */
    ESP_LOGE(TAG, "rollback: %s; restarting", esp_err_to_name(err));
    esp_restart();
}

static void deadline_cb(void *arg)
{
    ESP_LOGE(TAG, "not healthy within %d s: rolling back", PROBATION_S);
    /* Too much work for the esp_timer task's stack. */
    if (xTaskCreatePinnedToCore(rollback_task, "ota_rollback", ROLLBACK_STACK, NULL, ROLLBACK_PRIORITY, NULL,
                                NET_CORE) != pdPASS) {
        esp_restart();
    }
}

static void image_id(char out[IMAGE_ID_LEN + 1])
{
    esp_app_get_elf_sha256(out, IMAGE_ID_LEN + 1);
}

void orb_ota_download_mark_bad(const char *sha)
{
    char image[IMAGE_ID_LEN + 1];
    image_id(image);
    lock();
    strlcpy(s_bad_sha, sha, sizeof(s_bad_sha));
    unlock();
    const kv_t kv[] = { { "bad_sha", sha }, { "bad_on", image } };
    store(kv, COUNT(kv));
}

/* Resets that cut a probation short through no fault of the image. */
static bool external_reset(esp_reset_reason_t why)
{
    switch (why) {
    case ESP_RST_POWERON:
    case ESP_RST_EXT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
    case ESP_RST_USB:
    case ESP_RST_JTAG:
        return true;
    default:
        return false;
    }
}

/* Works out what became of the last orb_ota_download_app (orb_ota_boot_outcome) and loads
 * the report still waiting for a hello. */
static void settle_last_install(const esp_partition_t *running)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {   /* NOT_FOUND: nothing stored yet */
            ESP_LOGW(TAG, "nvs: %s", esp_err_to_name(err));
        }
        return;
    }
    char slot[17], version[32], sha[ORB_OTA_SHA_HEX + 1], why[64], bad_on[IMAGE_ID_LEN + 1];
    load(h, "try_slot", slot, sizeof(slot));
    load(h, "try_ver", version, sizeof(version));
    load(h, "try_sha", sha, sizeof(sha));
    load(h, "try_err", why, sizeof(why));
    load(h, "last_ver", s_last.version, sizeof(s_last.version));
    load(h, "last_res", s_last.result, sizeof(s_last.result));
    load(h, "last_err", s_last.error, sizeof(s_last.error));
    load(h, "bad_sha", s_bad_sha, sizeof(s_bad_sha));
    load(h, "assets_bad", s_bad_assets_sha, sizeof(s_bad_assets_sha));
    load(h, "bad_on", bad_on, sizeof(bad_on));
    nvs_close(h);

    char image[IMAGE_ID_LEN + 1];
    image_id(image);
    if (s_bad_sha[0] && strcmp(bad_on, image) != 0) {
        /* It failed under another image, such as an unsigned one since replaced
         * over USB, which may have been the cause: offers of it may install again. */
        s_bad_sha[0] = '\0';
        const kv_t kv[] = { { "bad_sha", NULL }, { "bad_on", NULL } };
        store(kv, COUNT(kv));
    }
    if (s_status.pending_verify && s_last.result[0]) {
        /* A new image: how it does supersedes any older report. */
        memset(&s_last, 0, sizeof(s_last));
        const kv_t kv[] = { { "last_ver", NULL }, { "last_res", NULL }, { "last_err", NULL } };
        store(kv, COUNT(kv));
    }

    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    const orb_ota_boot_t b = {
        .try_slot = slot,
        .try_version = version,
        .running_slot = running->label,
        .running_version = esp_app_get_description()->version,
        .boot_slot = boot ? boot->label : "",
        .invalid_slot = invalid ? invalid->label : "",
        .probation = s_status.pending_verify,
    };
    orb_ota_result_t r = orb_ota_boot_outcome(&b);
    if (r == ORB_OTA_RESULT_NONE) {
        return;
    }
    const kv_t forget[] = { { "try_slot", NULL }, { "try_ver", NULL }, { "try_sha", NULL }, { "try_err", NULL } };
    if (r == ORB_OTA_RESULT_STALE) {
        /* Nothing to report, and nothing against the image. */
        ESP_LOGW(TAG, "update to %s never took effect", version);
        store(forget, COUNT(forget));
        return;
    }

    bool external = external_reset(esp_reset_reason());
    const char *error = r == ORB_OTA_RESULT_OK            ? ""
                        : r == ORB_OTA_RESULT_FAILED      ? "did not start"
                        : why[0]                          ? why
                        : external                        ? "power lost or reset before it was healthy"
                                                          : "restarted before it was healthy";
    if (orb_ota_image_at_fault(r, why[0] != '\0', external)) {
        orb_ota_download_mark_bad(sha);
    }
    strlcpy(s_last.version, version, sizeof(s_last.version));
    strlcpy(s_last.result, orb_ota_result_name(r), sizeof(s_last.result));
    strlcpy(s_last.error, error, sizeof(s_last.error));
    const kv_t kv[] = {
        { "last_ver", s_last.version }, { "last_res", s_last.result }, { "last_err", s_last.error },
    };
    store(kv, COUNT(kv));
    store(forget, COUNT(forget));
    ESP_LOGW(TAG, "update to %s: %s%s%s", s_last.version, s_last.result, error[0] ? ", " : "", error);
}

esp_err_t orb_ota_boot_check(void)
{
    ESP_RETURN_ON_ERROR(init_lock(), TAG, "lock");
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_RETURN_ON_FALSE(running, ESP_ERR_NOT_FOUND, TAG, "running partition");

    esp_ota_img_states_t state;
    s_status.pending_verify =
        esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    if (s_status.pending_verify && !s_deadline) {
        const esp_timer_create_args_t args = { .callback = deadline_cb, .name = "ota_probation" };
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_deadline), TAG, "deadline");
        ESP_RETURN_ON_ERROR(esp_timer_start_once(s_deadline, PROBATION_S * 1000000LL), TAG, "deadline");
        ESP_LOGW(TAG, "%s %s on probation: %d s to come up healthy", running->label,
                 esp_app_get_description()->version, PROBATION_S);
    }
    settle_last_install(running);
    return ESP_OK;
}

/* ---- Offers ---- */

static bool busy_locked(void)
{
    return s_status.state == ORB_OTA_DOWNLOADING || s_status.state == ORB_OTA_VERIFYING ||
           s_status.state == ORB_OTA_READY;
}

static orb_ota_action_t decide_locked(const orb_ota_offer_t *o, bool tapped)
{
    char assets_version[11] = "";
    uint32_t version = orb_assets_version();
    if (version) {
        snprintf(assets_version, sizeof(assets_version), "%" PRIu32, version);
    }
    const orb_ota_ctx_t ctx = {
        .running_version = o->assets ? assets_version : esp_app_get_description()->version,
        .bad_sha = o->assets ? s_bad_assets_sha : s_bad_sha,
        .idle = s_idle,
        .busy = busy_locked(),
        .low_battery = s_low_battery,
        .probation = s_status.pending_verify,
        .tapped = tapped,
    };
    return orb_ota_decide(o, &ctx);
}

static void set_offer_state_locked(orb_ota_state_t state)
{
    s_status.state = state;
    s_status.assets = s_offer.assets;
    s_status.progress = 0;
    s_status.error[0] = '\0';
    strlcpy(s_status.version, s_offer.version, sizeof(s_status.version));
    strlcpy(s_status.notes, s_offer.notes, sizeof(s_status.notes));
}

void orb_ota_download_assets_result(const orb_ota_offer_t *o, esp_err_t ret)
{
    if (ret != ESP_OK) {
        lock();
        strlcpy(s_bad_assets_sha, o->sha256, sizeof(s_bad_assets_sha));
        unlock();
        const kv_t failed[] = { { "assets_bad", o->sha256 } };
        store(failed, COUNT(failed));
    } else {
        const kv_t clear[] = { { "assets_bad", NULL } };
        store(clear, COUNT(clear));
    }
}

static void install_task(void *arg);

static esp_err_t start_install_locked(void)
{
    set_offer_state_locked(ORB_OTA_DOWNLOADING);
    if (xTaskCreatePinnedToCore(install_task, "ota", INSTALL_STACK, NULL, INSTALL_PRIORITY, NULL, NET_CORE) !=
        pdPASS) {
        s_status.state = ORB_OTA_FAILED;
        strlcpy(s_status.error, "no memory to download", sizeof(s_status.error));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* The waiting offer, after a tap, idle, or the end of probation. True if the state changed. */
static bool consider_held_locked(bool tapped, esp_err_t *err)
{
    bool waiting = s_status.state == ORB_OTA_OFFERED || s_status.state == ORB_OTA_FAILED;
    if (!s_have_offer || !waiting || decide_locked(&s_offer, tapped) != ORB_OTA_INSTALL) {
        *err = ESP_ERR_INVALID_STATE;
        return false;
    }
    *err = start_install_locked();
    return true;
}

static void handle_offer(const cJSON *p)
{
    orb_ota_offer_t offer;
    char err[64];
    if (!orb_ota_offer_parse(p, &offer, err, sizeof(err))) {
        ESP_LOGW(TAG, "%s", err);
        return;
    }

    lock();
    bool changed = false;
    if (!offer.available) {
        /* The hub has nothing (any more): forget a waiting offer. */
        if (s_have_offer && offer.assets == s_offer.assets && !busy_locked()) {
            s_have_offer = false;
            s_status.state = ORB_OTA_IDLE;
            s_status.progress = 0;
            s_status.version[0] = s_status.notes[0] = s_status.error[0] = '\0';
            changed = true;
        }
    } else {
        orb_ota_action_t act = decide_locked(&offer, false);
        bool same = s_have_offer && strcmp(offer.sha256, s_offer.sha256) == 0;
        if (act == ORB_OTA_INSTALL) {
            s_offer = offer;
            s_have_offer = true;
            start_install_locked();
            changed = true;
        } else if (act == ORB_OTA_HOLD && !(same && s_status.state == ORB_OTA_OFFERED)) {
            s_offer = offer;
            s_have_offer = true;
            set_offer_state_locked(ORB_OTA_OFFERED);
            changed = true;
        }
        ESP_LOGI(TAG, "offer %s (%s, %" PRIu32 " bytes): %s", offer.version, offer.build, offer.size,
                 act == ORB_OTA_INSTALL ? "installing" : act == ORB_OTA_HOLD ? "waiting" : "ignored");
    }
    orb_ota_status_t st = s_status;
    unlock();
    if (changed) {
        notify(&st);
    }
}

static void on_offer_event(const char *name, const cJSON *params, void *ctx)
{
    handle_offer(params);
}

static void on_check_reply(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota.check: %s", esp_err_to_name(err));
        return;
    }
    handle_offer(result);
}

static void on_hello(cJSON *hello, void *ctx)
{
    lock();
    if (s_last.result[0]) {
        orb_ota_add_last_ota(hello, s_last.version, s_last.result, s_last.error);
        s_last_sent = true;
    } else if (s_status.pending_verify) {
        /* Its acceptance is what makes this image healthy, so this hello can
         * carry the good news itself. */
        orb_ota_add_last_ota(hello, esp_app_get_description()->version, "ok", NULL);
        s_last_sent = true;
    }
    unlock();
    char version[11] = "";
    uint32_t current = orb_assets_version();
    if (current) {
        snprintf(version, sizeof(version), "%" PRIu32, current);
    }
    cJSON_AddStringToObject(hello, "assets", version);
}

/* ---- Install ---- */

void orb_ota_download_state(orb_ota_state_t state, int progress, const char *error)
{
    lock();
    s_status.state = state;
    s_status.progress = progress;
    strlcpy(s_status.error, error ? error : "", sizeof(s_status.error));
    orb_ota_status_t st = s_status;
    unlock();
    notify(&st);
}

void orb_ota_download_report(const orb_ota_offer_t *o, const char *state, int progress, const char *error)
{
    cJSON *p = orb_ota_report(o->version, state, progress, error);
    if (p) {
        if (o->assets) {
            cJSON_AddStringToObject(p, "kind", "assets");
        }
        orb_link_notify("ota.report", p);
    }
}

/* Before the boot slot switches: what the next boot reports on. An older report
 * still waiting for a hello stays until that boot finds the new image running. */
void orb_ota_download_record_attempt(const orb_ota_offer_t *o, const esp_partition_t *slot)
{
    const kv_t kv[] = {
        { "try_ver", o->version }, { "try_sha", o->sha256 }, { "try_slot", slot->label }, { "try_err", NULL },
    };
    store(kv, COUNT(kv));
}

void orb_ota_download_clear_attempt(void)
{
    const kv_t kv[] = { { "try_ver", NULL }, { "try_sha", NULL }, { "try_slot", NULL } };
    store(kv, COUNT(kv));
}

void orb_ota_download_progress(const orb_ota_offer_t *o, int pct, int *reported)
{
    lock();
    bool changed = s_status.progress != pct;
    s_status.progress = pct;
    orb_ota_status_t st = s_status;
    unlock();
    if (changed) {
        notify(&st);
    }
    if (pct >= *reported + ORB_OTA_REPORT_STEP) {
        *reported = pct - pct % ORB_OTA_REPORT_STEP;
        orb_ota_download_report(o, "downloading", *reported, NULL);
    }
}

static void install_task(void *arg)
{
    lock();
    orb_ota_offer_t offer = s_offer;
    unlock();

    ESP_LOGI(TAG, "installing %s from %s", offer.version, offer.path);
    orb_ota_download_report(&offer, "downloading", 0, NULL);
    char err[64] = "";
    esp_err_t result = offer.assets ? orb_ota_download_assets(&offer, err, sizeof(err)) : orb_ota_download_app(&offer, err, sizeof(err));
    if (result == ESP_OK) {
        orb_ota_download_state(ORB_OTA_READY, 100, NULL);
        orb_ota_download_report(&offer, "ready", 100, NULL);
        ESP_LOGI(TAG, "%s ready; restarting", offer.version);
        /* Time for the report to go out and for the UI to say so. */
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    ESP_LOGE(TAG, "update to %s failed: %s", offer.version, err);
    lock();
    int progress = s_status.progress;
    unlock();
    orb_ota_download_state(ORB_OTA_FAILED, progress, err);
    orb_ota_download_report(&offer, "failed", progress, err);
    vTaskDelete(NULL);
}

/* ---- API ---- */

void orb_ota_mark_healthy(void)
{
    if (!s_lock) {
        return;
    }
    lock();
    bool confirmed = false;
    if (s_status.pending_verify) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            /* The deadline keeps running on failure: an image that can't be
             * confirmed rolls back like an unhealthy one. */
            if (s_deadline) {
                esp_timer_stop(s_deadline);
            }
            s_status.pending_verify = false;
            confirmed = true;
            ESP_LOGI(TAG, "%s confirmed", esp_app_get_description()->version);
        } else {
            ESP_LOGE(TAG, "mark valid: %s", esp_err_to_name(err));
        }
    }
    /* The hello that carried last_ota was accepted (that's when the app calls
     * this), so the report is delivered. */
    bool sent = s_last_sent;
    if (sent) {
        memset(&s_last, 0, sizeof(s_last));
        s_last_sent = false;
    } else if (confirmed) {
        memset(&s_last, 0, sizeof(s_last));
        strlcpy(s_last.version, esp_app_get_description()->version, sizeof(s_last.version));
        strlcpy(s_last.result, "ok", sizeof(s_last.result));
    }
    /* Stored before a waiting offer can start installing and record its own attempt. */
    if (confirmed) {
        const kv_t kv[] = { { "try_ver", NULL }, { "try_sha", NULL }, { "try_slot", NULL }, { "try_err", NULL } };
        store(kv, COUNT(kv));
    }
    if (sent || confirmed) {
        bool keep = s_last.result[0];
        const kv_t kv[] = {
            { "last_ver", keep ? s_last.version : NULL },
            { "last_res", keep ? s_last.result : NULL },
            { "last_err", keep ? s_last.error : NULL },
        };
        store(kv, COUNT(kv));
    }
    esp_err_t install_err;
    bool installing = confirmed && consider_held_locked(false, &install_err);
    orb_ota_status_t st = s_status;
    unlock();

    if (confirmed || installing) {
        notify(&st);
    }
}

esp_err_t orb_ota_start(orb_ota_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_ERROR(init_lock(), TAG, "lock");
    s_cb = cb;
    s_cb_ctx = ctx;
    ESP_RETURN_ON_ERROR(orb_link_on_event("ota.available", on_offer_event, NULL), TAG, "ota.available");
    ESP_RETURN_ON_ERROR(orb_link_on_hello(on_hello, NULL), TAG, "hello");
    return ESP_OK;
}

esp_err_t orb_ota_status(orb_ota_status_t *out)
{
    ESP_RETURN_ON_FALSE(s_lock && out, ESP_ERR_INVALID_STATE, TAG, "not started");
    lock();
    *out = s_status;
    unlock();
    return ESP_OK;
}

esp_err_t orb_ota_check_now(void)
{
    return orb_link_request("ota.check", NULL, 10000, on_check_reply, NULL);
}

esp_err_t orb_ota_install(void)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    esp_err_t err;
    lock();
    bool changed = consider_held_locked(true, &err);
    orb_ota_status_t st = s_status;
    unlock();
    if (changed) {
        notify(&st);
    }
    return err;
}

void orb_ota_set_power(int battery_pct, bool charging)
{
    if (!s_lock) {
        return;
    }
    esp_err_t err;
    lock();
    bool was_low = s_low_battery;
    s_low_battery = orb_ota_low_battery(battery_pct, charging);
    /* Only on the way up: a failed download isn't retried on every percent. */
    bool changed = was_low && !s_low_battery && consider_held_locked(false, &err);
    orb_ota_status_t st = s_status;
    unlock();
    if (changed) {
        notify(&st);
    }
}

void orb_ota_set_idle(bool idle)
{
    if (!s_lock) {
        return;
    }
    esp_err_t err;
    lock();
    s_idle = idle;
    bool changed = idle && consider_held_locked(false, &err);
    orb_ota_status_t st = s_status;
    unlock();
    if (changed) {
        notify(&st);
    }
}
