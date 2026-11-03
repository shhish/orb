/* Pinned HTTP and flash transactions; status, locking and NVS stay with
 * orb_ota.c so splitting the adapter does not add another state owner. */
#include "orb_ota_download.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_secure_boot.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orb_assets.h"
#include "orb_link.h"
#include "orb_mem.h"
#include "orb_ota_assets.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

static const char *s_tag = "ota";

#define HTTP_TIMEOUT_MS 15000
#define STALL_US (30 * 1000000LL) /* no byte for this long: give up on the download */
#define CHUNK 4096              /* HTTP reads and flash writes a sector at a time */

static esp_err_t add_auth(esp_http_client_handle_t client)
{
    void *data = NULL;
    ESP_RETURN_ON_ERROR(esp_http_client_get_user_data(client, &data), s_tag, "HTTPS snapshot");
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_STATE, s_tag, "no HTTPS snapshot");
    const orb_link_https_t *https = data;
    return esp_http_client_set_header(client, "Authorization", https->authorization);
}

/* SHA-256 of what landed in the slot, against the offer. */
static esp_err_t check_sha(const esp_partition_t *slot, const orb_ota_offer_t *o)
{
    uint8_t *buf = malloc(CHUNK);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, s_tag, "sha buffer");
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    esp_err_t err = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    for (uint32_t off = 0; err == ESP_OK && off < o->size; off += CHUNK) {
        size_t n = MIN(CHUNK, o->size - off);
        err = esp_partition_read(slot, off, buf, n);
        if (err == ESP_OK && psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            err = ESP_FAIL;
        }
    }
    uint8_t digest[32];
    size_t len;
    if (err == ESP_OK && psa_hash_finish(&op, digest, sizeof(digest), &len) != PSA_SUCCESS) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        psa_hash_abort(&op);
    }
    free(buf);
    if (err == ESP_OK && !orb_ota_digest_matches(digest, o->sha256)) {
        err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

#if CONFIG_ORB_DEVTOOLS
/* The central monitor preserves this attempt's minima while link/voice windows
 * overlap it; direct IDF start/stop calls would reset their global monitor. */
static bool dev_heap_start(void)
{
    orb_mem_begin(ORB_MEM_OTA);
    return true;
}

static void dev_heap_phase(const char *phase, bool monitoring)
{
    if (!monitoring) {
        return;
    }
    multi_heap_info_t internal, dma, psram;
    orb_mem_phase_info(ORB_MEM_OTA, &internal, MALLOC_CAP_INTERNAL);
    orb_mem_phase_info(ORB_MEM_OTA, &dma, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    orb_mem_phase_info(ORB_MEM_OTA, &psram, MALLOC_CAP_SPIRAM);
    /* Each triple is current free / largest block / attempt minimum, in bytes.
     * Internal includes all internal capabilities, matching the central phase
     * report. DMA overlaps internal; these figures must not be added. */
    ESP_LOGI(s_tag, "heap %s free/largest/min: internal=%u/%u/%u dma=%u/%u/%u psram=%u/%u/%u stack_free=%u",
             phase, (unsigned)internal.total_free_bytes, (unsigned)internal.largest_free_block,
             (unsigned)internal.minimum_free_bytes, (unsigned)dma.total_free_bytes,
             (unsigned)dma.largest_free_block, (unsigned)dma.minimum_free_bytes,
             (unsigned)psram.total_free_bytes, (unsigned)psram.largest_free_block,
             (unsigned)psram.minimum_free_bytes, (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void dev_heap_stop(bool monitoring)
{
    if (monitoring) {
        dev_heap_phase("cleanup", true);
        orb_mem_end(ORB_MEM_OTA);
    }
}
#else
#define dev_heap_start() false
#define dev_heap_phase(phase, monitoring) ((void)(monitoring))
#define dev_heap_stop(monitoring) ((void)(monitoring))
#endif

static esp_err_t image_header(esp_https_ota_handle_t h, const orb_ota_offer_t *o,
                              char *err, size_t err_len, bool *bad)
{
    esp_app_desc_t desc;
    esp_err_t ret = esp_https_ota_get_img_desc(h, &desc);
    if (ret != ESP_OK) {
        snprintf(err, err_len, "image header: %s", esp_err_to_name(ret));
    } else if (!orb_ota_image_ok(o->version, esp_app_get_description()->project_name, desc.project_name,
                                 desc.version, err, err_len)) {
        ret = ESP_ERR_INVALID_VERSION;
        *bad = true;
    } else if (esp_https_ota_get_image_size(h) != (int)o->size) {
        int len = esp_https_ota_get_image_size(h);
        snprintf(err, err_len, "%d bytes, offer said %" PRIu32, len, o->size);
        ret = ESP_ERR_INVALID_SIZE;
        *bad = len >= 0;   /* -1: no Content-Length, the hub's fault rather than the image's */
    }

    return ret;
}

/* Download, check and switch slot. err gets a short reason on failure. */
esp_err_t orb_ota_download_app(const orb_ota_offer_t *o, char *err, size_t err_len)
{
    bool heap_monitoring = dev_heap_start();
    dev_heap_phase("start", heap_monitoring);
    /* Pairing may change while downloading. Own one coherent snapshot until
     * the HTTP client is finished with both the pin and authorization header. */
    orb_link_https_t https = { 0 };
    esp_err_t ret = orb_link_hub_https(&https);
    if (ret != ESP_OK) {
        strlcpy(err, ret == ESP_ERR_INVALID_STATE ? "not paired" : "can't read hub pairing", err_len);
        goto done;
    }
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot || o->size > slot->size) {
        strlcpy(err, slot ? "image too big for the slot" : "no update slot", err_len);
        ret = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    /* The update is checked against the key in this image's signature block.
     * Without one every image fails that check through no fault of its own, so
     * don't fetch one only to blame it. */
    esp_image_sig_public_key_digests_t keys = { 0 };
    ret = esp_secure_boot_get_signature_blocks_for_running_app(false, &keys);
    if (ret != ESP_OK) {
        strlcpy(err, ret == ESP_ERR_NOT_FOUND ? "running image is unsigned" : "can't read own signature", err_len);
        goto done;
    }

    /* Always the paired hub's address: the token goes nowhere else, whatever
     * host the offer's url names. */
    char url[200];
    if (snprintf(url, sizeof(url), "%s%s", https.base_url, o->path) >= (int)sizeof(url)) {
        strlcpy(err, "url too long", err_len);
        ret = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    const esp_http_client_config_t http = {
        .url = url,
        /* The hub's own certificate, the only one trusted. That pin alone
         * identifies the hub, so its names aren't checked: the URL holds the
         * hub's current IP, and the IPs in the certificate are the ones it had
         * when it was made, which a new DHCP lease leaves behind. esp-tls logs
         * a warning about the skipped check. */
        .cert_pem = https.cert_pem,
        .user_data = &https,
        .skip_cert_common_name_check = true,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = CHUNK,
        .keep_alive_enable = true,
        .max_authorization_retries = -1,  /* a 401 is final: the token is the only credential */
    };
    const esp_https_ota_config_t cfg = {
        .http_config = &http,
        .http_client_init_cb = add_auth,
        .partition.staging = slot,
    };
    esp_https_ota_handle_t h;
    ret = esp_https_ota_begin(&cfg, &h);
    dev_heap_phase("connect", heap_monitoring);
    if (ret != ESP_OK) {
        snprintf(err, err_len, "download: %s", esp_err_to_name(ret));
        goto done;
    }

    bool bad = false;   /* the image itself is wrong, not the network */
    ret = image_header(h, o, err, err_len, &bad);

    if (ret == ESP_OK) {
        int reported = -ORB_OTA_REPORT_STEP;
        orb_ota_watch_t watch = { .read = -1 };
        bool stalled = false;
        do {
            ret = esp_https_ota_perform(h);
            int done = esp_https_ota_get_image_len_read(h);
            orb_ota_download_progress(o, orb_ota_percent(done, o->size), &reported);
            /* A read that times out comes back as IN_PROGRESS, so a hub that
             * stops sending with the connection still up would hold this loop,
             * and every later offer, forever. */
            stalled = ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS &&
                      orb_ota_stalled(&watch, done, esp_timer_get_time(), STALL_US);
        } while (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS && !stalled);
        if (stalled) {
            ret = ESP_ERR_TIMEOUT;
            strlcpy(err, "download stalled", err_len);
        } else {
            if (ret == ESP_OK && !esp_https_ota_is_complete_data_received(h)) {
                ret = ESP_FAIL;
            }
            if (ret != ESP_OK) {
                snprintf(err, err_len, "download: %s", esp_err_to_name(ret));
                bad = ret == ESP_ERR_OTA_VALIDATE_FAILED || ret == ESP_ERR_INVALID_VERSION;
            }
        }
    }

    dev_heap_phase("download", heap_monitoring);

    if (ret == ESP_OK) {
        orb_ota_download_state(ORB_OTA_VERIFYING, 100, NULL);
        orb_ota_download_report(o, "verifying", 100, NULL);
        ret = check_sha(slot, o);
        dev_heap_phase("sha", heap_monitoring);
        if (ret != ESP_OK) {
            strlcpy(err, ret == ESP_ERR_INVALID_CRC ? "SHA-256 mismatch" : "SHA-256 check failed", err_len);
            bad = ret == ESP_ERR_INVALID_CRC;
        }
    }

    if (ret != ESP_OK) {
        esp_https_ota_abort(h);
    } else {
        orb_ota_download_record_attempt(o, slot);
        /* esp_ota_end checks the signature; only then does the boot slot switch. */
        ret = esp_https_ota_finish(h);
        dev_heap_phase("signature", heap_monitoring);
        if (ret != ESP_OK) {
            orb_ota_download_clear_attempt();
            bad = ret == ESP_ERR_OTA_VALIDATE_FAILED;
            if (bad) {
                strlcpy(err, "signature or image check failed", err_len);
            } else {
                snprintf(err, err_len, "finish: %s", esp_err_to_name(ret));
            }
        }
    }
    if (bad) {
        orb_ota_download_mark_bad(o->sha256);
    }
done:
    free(https.cert_pem);
    dev_heap_stop(heap_monitoring);
    return ret;
}

typedef struct {
    esp_http_client_handle_t client;
    const esp_partition_t *partition;
    const orb_ota_offer_t *offer;
    int reported;
} assets_tx_t;

static int assets_begin(void *ctx)
{
    return orb_assets_begin_update();
}

static int assets_erase(void *ctx, size_t offset, size_t size)
{
    assets_tx_t *tx = ctx;
    esp_err_t err = esp_partition_erase_range(tx->partition, offset, size);
    if (err != ESP_OK) {
        ESP_LOGE(s_tag, "assets erase: %s", esp_err_to_name(err));
    }
    return err;
}

static int assets_write(void *ctx, size_t offset, const void *data, size_t size)
{
    return esp_partition_write(((assets_tx_t *)ctx)->partition, offset, data, size);
}

static int assets_read(void *ctx, void *data, size_t size)
{
    int count = esp_http_client_read(((assets_tx_t *)ctx)->client, data, (int)size);
    return count == -ESP_ERR_HTTP_EAGAIN ? 0 : count;
}

static bool assets_complete(void *ctx)
{
    return esp_http_client_is_complete_data_received(((assets_tx_t *)ctx)->client);
}

static int assets_check_sha(void *ctx)
{
    assets_tx_t *tx = ctx;
    return check_sha(tx->partition, tx->offer);
}

static int assets_verify(void *ctx)
{
    assets_tx_t *tx = ctx;
    orb_assets_info_t info;
    esp_err_t ret = orb_assets_verify_partition(tx->partition, tx->offer->size, &info);
    if (ret == ESP_OK) {
        char version[11];
        snprintf(version, sizeof(version), "%" PRIu32, info.content_version);
        if (strcmp(version, tx->offer->version) != 0) {
            ret = ESP_ERR_INVALID_VERSION;
        }
    }
    return ret;
}

static void assets_progress(void *ctx, size_t done, bool verifying)
{
    assets_tx_t *tx = ctx;
    if (verifying) {
        orb_ota_download_state(ORB_OTA_VERIFYING, 100, NULL);
        orb_ota_download_report(tx->offer, "verifying", 100, NULL);
    } else {
        orb_ota_download_progress(tx->offer, orb_ota_percent((int64_t)done, tx->offer->size), &tx->reported);
    }
}

static int64_t assets_now(void *ctx)
{
    return esp_timer_get_time();
}

static void assets_wait(void *ctx)
{
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void assets_end(void *ctx)
{
    orb_assets_end_update();
}

/* Assets are data, so esp_https_ota cannot write them. The same pinned HTTPS
 * snapshot and whole-image digest apply, followed by the independent signed
 * envelope check. Font reads stop before the first erase; power loss then boots
 * with rescue fonts. No boot selection or app probation metadata is touched. */
esp_err_t orb_ota_download_assets(const orb_ota_offer_t *o, char *error, size_t error_size)
{
    orb_link_https_t https = { 0 };
    esp_http_client_handle_t client = NULL;
    uint8_t *buffer = NULL;
    esp_err_t ret = orb_link_hub_https(&https);
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                          ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (ret != ESP_OK || !part || o->size > part->size) {
        ret = ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
        goto done;
    }
    char url[200];
    if (snprintf(url, sizeof(url), "%s%s", https.base_url, o->path) >= (int)sizeof(url)) {
        ret = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    buffer = malloc(CHUNK);
    if (!buffer) {
        ret = ESP_ERR_NO_MEM;
        goto done;
    }
    const esp_http_client_config_t config = {
        .url = url,
        .cert_pem = https.cert_pem,
        .skip_cert_common_name_check = true,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = CHUNK,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
    };
    client = esp_http_client_init(&config);
    if (!client) {
        ret = ESP_ERR_NO_MEM;
        goto done;
    }
    ret = esp_http_client_set_header(client, "Authorization", https.authorization);
    if (ret == ESP_OK) {
        ret = esp_http_client_open(client, 0);
    }
    if (ret != ESP_OK) {
        goto done;
    }
    int64_t length = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200 || length != o->size) {
        ret = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    assets_tx_t transaction = { .client = client, .partition = part, .offer = o, .reported = -ORB_OTA_REPORT_STEP };
    const orb_assets_tx_port_t port = {
        .ctx = &transaction, .begin = assets_begin, .erase = assets_erase, .write = assets_write,
        .read = assets_read, .complete = assets_complete, .check_sha = assets_check_sha,
        .verify = assets_verify, .progress = assets_progress, .now = assets_now,
        .wait = assets_wait, .end = assets_end,
    };
    ret = orb_assets_tx_apply(&port, o->size, part->size, buffer, CHUNK);
    if (ret == ORB_ASSETS_TX_SIZE) {
        ret = ESP_ERR_INVALID_SIZE;
    } else if (ret == ORB_ASSETS_TX_READ) {
        ret = ESP_FAIL;
    } else if (ret == ORB_ASSETS_TX_STALLED) {
        ret = ESP_ERR_TIMEOUT;
    }
done:
    if (client) {
        esp_http_client_cleanup(client);
    }
    free(buffer);
    free(https.cert_pem);
    if (ret != ESP_OK) {
        snprintf(error, error_size, "assets: %s", esp_err_to_name(ret));
    }
    orb_ota_download_assets_result(o, ret);
    return ret;
}

