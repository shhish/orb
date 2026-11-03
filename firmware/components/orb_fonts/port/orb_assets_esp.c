#include "orb_assets.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_secure_boot.h"
#include "orb_fonts_private.h"
#include "orb_memory.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#if CONFIG_ORB_DEVTOOLS
#include <assert.h>
#include "esp_memory_utils.h"
#endif

#if !defined(CONFIG_SECURE_SIGNED_ON_UPDATE) || !defined(CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME)
#error "Orb assets require RSA Secure Boot v2 signature verification"
#endif
#if !defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) || !defined(CONFIG_SPIRAM_XIP_FROM_PSRAM)
#error "Orb assets verification requires PSRAM stacks and flash-safe PSRAM XIP"
#endif

static const char *TAG = "assets";
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_switch_lock;
static StaticSemaphore_t s_switch_lock_buffer;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_checked;
static bool s_trusted;
static bool s_updating;
static bool s_verifying;
static orb_assets_info_t s_info;

static inline void check_mapping_stack(void)
{
#if CONFIG_ORB_DEVTOOLS
    /* IDF freezes caches to update MMU mappings, even with PSRAM XIP. Only
     * main/UI/OTA internal stacks may map or retire font bitmap leases. */
    unsigned marker;
    assert(esp_ptr_internal(&marker));
#endif
}

struct orb_fonts_bitmap {
    struct orb_fonts_bitmap *next;
    const uint8_t *data;
    void *copy;
    size_t size;
};
static orb_fonts_bitmap_t *s_bitmaps;
/* One mapping for every raw record, kept until an assets update needs the
 * flash back. Per-record mappings are not safe: IDF answers a range enclosed by
 * an existing block with that block's address and no reference count, so
 * unmapping a retired face also unmaps every live face sharing its 64 KiB pages. */
static const uint8_t *s_window;
static esp_partition_mmap_handle_t s_window_handle;
static size_t s_window_start;

/* Publish only actual flash backing. Other content sizes/Bold Text and an OTA
 * migration must not pay the higher admission floor without its font saving. */
static void bitmap_backing(void)
{
    size_t bytes = 0;
    for (orb_fonts_bitmap_t *b = s_bitmaps; b; b = b->next) {
        if (!b->copy) bytes += b->size;
    }
    orb_memory_cache_set_flash_backing(bytes);
}

static void lock(void)
{
    portENTER_CRITICAL(&s_init_lock);
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
        s_switch_lock = xSemaphoreCreateMutexStatic(&s_switch_lock_buffer);
    }
    portEXIT_CRITICAL(&s_init_lock);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

bool orb_fonts_port_begin_switch(void)
{
    lock();
    unlock(); /* initialize both locks before waiting outside the assets lock */
    xSemaphoreTake(s_switch_lock, portMAX_DELAY);
    lock();
    bool ready = s_trusted && !s_updating && !s_verifying;
    unlock();
    if (!ready) xSemaphoreGive(s_switch_lock);
    return ready;
}

void orb_fonts_port_end_switch(void)
{
    xSemaphoreGive(s_switch_lock);
}

static const esp_partition_t *assets_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                     CONFIG_ORB_FONTS_PACK_PARTITION);
}

static esp_err_t hash_partition(const esp_partition_t *part, size_t offset, size_t length,
                                 uint8_t digest[32])
{
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) return ESP_FAIL;
    uint8_t chunk[1024];
    while (length) {
        size_t size = length > sizeof(chunk) ? sizeof(chunk) : length;
        esp_err_t err = esp_partition_read(part, offset, chunk, size);
        if (err != ESP_OK || psa_hash_update(&hash, chunk, size) != PSA_SUCCESS) {
            psa_hash_abort(&hash);
            return err == ESP_OK ? ESP_FAIL : err;
        }
        offset += size;
        length -= size;
    }
    size_t size;
    if (psa_hash_finish(&hash, digest, 32, &size) != PSA_SUCCESS || size != 32) {
        psa_hash_abort(&hash);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t orb_assets_verify_partition(const esp_partition_t *part, size_t image_size,
                                      orb_assets_info_t *out)
{
    uint8_t header[ORB_ASSETS_HEADER_SIZE];
    orb_assets_info_t info;
    if (!part || esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK ||
        !orb_assets_parse_header(header, sizeof(header), part->size, image_size, &info)) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* The pack crosses physical 16 MiB. The generic IDF verifier hashes via
     * flash mmap, which rejects that range with this board's 24-bit cache
     * configuration. Partition reads support all 32 MiB without changing the
     * bootloader. Keep assets reads/bounce buffers <=1 KiB; IDF's running-app
     * trust lookup and RSA have their own small temporary allocations. */
    uint8_t digest[32];
    esp_err_t err = hash_partition(part, 0, info.signed_size, digest);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "signed assets hash failed: %s", esp_err_to_name(err));
        return err;
    }
    ets_secure_boot_signature_t *signature = heap_caps_malloc(sizeof(*signature),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!signature) return ESP_ERR_NO_MEM;
    for (size_t offset = 0; offset < sizeof(*signature); offset += 1024) {
        size_t size = sizeof(*signature) - offset;
        if (size > 1024) size = 1024;
        err = esp_partition_read(part, info.signed_size + offset, (uint8_t *)signature + offset, size);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) {
        /* Preserve IDF's running-app/eFuse trust and block-zero RSA-PSS policy. */
        err = esp_secure_boot_verify_sbv2_signature_block(signature, digest, NULL);
    }
    heap_caps_free(signature);
    if (err != ESP_OK) return err;

    /* Check the authenticated payload identity independently of signing salt. */
    err = hash_partition(part, ORB_ASSETS_HEADER_SIZE, info.payload_size, digest);
    if (err != ESP_OK) return err;
    if (memcmp(digest, info.payload_sha256, sizeof(digest)) != 0) return ESP_ERR_INVALID_CRC;
    if (out) *out = info;
    return ESP_OK;
}

/* s_lock protects both the trust decision and every font flash read. An updater
 * cannot erase a record between checking trust and copying its compressed bytes. */
static bool check_locked(void)
{
    /* Font reads and hello callbacks must never hash megabytes or run RSA on
     * their small task stacks. Only the boot worker/OTA installer verifies. */
    return s_checked && s_trusted && !s_updating && !s_verifying;
}

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;
    unsigned stack_free;
} boot_job_t;

static void log_boot_heap(const char *phase)
{
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG, "boot %s: internal free=%u largest=%u historical-min=%u; DMA free=%u historical-min=%u",
             phase, (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

static void verify_at_boot(void *context)
{
    boot_job_t *job = context;
    orb_assets_info_t info;
    esp_err_t result = orb_assets_verify_partition(assets_partition(), 0, &info);
    lock();
    s_checked = true;
    s_verifying = false;
    s_trusted = result == ESP_OK;
    if (s_trusted) s_info = info;
    unlock();
    if (result != ESP_OK) ESP_LOGW(TAG, "signed assets unavailable (%s); using built-in rescue fonts",
                                  esp_err_to_name(result));
    SemaphoreHandle_t done = job->done;
    job->result = result;
    job->stack_free = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    /* The caller owns this context. Never access it after signaling. */
    xSemaphoreGive(done);
    /* The caller joins/deletes this worker, so its stack and TCB cannot linger
     * until an idle task runs while display/radio startup consumes internal RAM. */
    vTaskSuspend(NULL);
}

esp_err_t orb_assets_init(void)
{
    lock();
    if (s_checked || s_updating || s_verifying) {
        bool valid = check_locked();
        unlock();
        return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    s_verifying = true;
    unlock();
    log_boot_heap("verify start");
    /* app_main has a 3584-byte stack. Keep the RSA worker's 8 KiB in PSRAM,
     * with no fallback to scarce internal RAM. This board enables PSRAM XIP
     * and external task stacks; IDF keeps the TCB internal. */
    static StaticSemaphore_t done_buffer;
    boot_job_t job = {.done = xSemaphoreCreateBinaryStatic(&done_buffer), .result = ESP_FAIL};
    TaskHandle_t worker;
    if (xTaskCreateWithCaps(verify_at_boot, "assets_verify", 8192, &job, 5, &worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        lock();
        s_verifying = false;
        s_checked = true;
        s_trusted = false;
        unlock();
        log_boot_heap("verify allocation failed");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    vTaskDeleteWithCaps(worker);
    log_boot_heap("verify joined");
    ESP_LOGI(TAG, "boot verify: worker stack free=%u bytes", job.stack_free);
    return job.result;
}

uint32_t orb_assets_version(void)
{
    lock();
    uint32_t version = check_locked() ? s_info.content_version : 0;
    unlock();
    return version;
}

/* All-or-nothing migration under the same lock as bitmap decoding. Metadata
 * stays in PSRAM throughout; no published font/fallback pointer is rewritten.
 * OOM leaves the old signed assets readable and prevents the first erase. */
static bool copy_bitmaps(void)
{
    check_mapping_stack();
    for (orb_fonts_bitmap_t *b = s_bitmaps; b; b = b->next) {
        if (b->copy) continue;
        b->copy = orb_fonts_port_alloc(b->size);
        if (!b->copy) {
            for (orb_fonts_bitmap_t *undo = s_bitmaps; undo; undo = undo->next) {
                if (undo->copy && undo->data != undo->copy) {
                    orb_fonts_port_free(undo->copy);
                    undo->copy = NULL;
                }
            }
            return false;
        }
        memcpy(b->copy, b->data, b->size);
    }
    for (orb_fonts_bitmap_t *b = s_bitmaps; b; b = b->next) {
        b->data = b->copy;
    }
    if (s_window) {
        esp_partition_munmap(s_window_handle);
        s_window = NULL;
    }
    bitmap_backing();
    return true;
}

esp_err_t orb_assets_begin_update(void)
{
    lock();
    unlock();
    /* A font transaction may retire mappings and read several records. Keep
     * its rollback on one trusted image; OTA waits before copying or erasing. */
    xSemaphoreTake(s_switch_lock, portMAX_DELAY);
    lock();
    if (s_updating || s_verifying) {
        unlock();
        xSemaphoreGive(s_switch_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!copy_bitmaps()) {
        unlock();
        xSemaphoreGive(s_switch_lock);
        return ESP_ERR_NO_MEM;
    }
    s_updating = true;
    s_checked = s_trusted = false;
    unlock();
    xSemaphoreGive(s_switch_lock);
    return ESP_OK;
}

void orb_assets_end_update(void)
{
    lock();
    /* The installer verifies on its own large worker stack and reboots on
     * success. Never run a whole-pack hash/RSA check on an LVGL draw stack or
     * expose newly written records to a mixture of cached fonts before reboot. */
    s_checked = true;
    s_trusted = s_updating = false;
    unlock();
}

/* Reading straight into a PSRAM font buffer makes the flash driver allocate an
 * internal bounce buffer as large as the request (16 KiB measured on the board),
 * and font faces load when new text appears, often while Wi-Fi and TLS are busy.
 * One small buffer, guarded by the assets lock, bounds that to 1 KiB. */
static uint8_t s_bounce[1024];

static bool read_bounced(const esp_partition_t *part, size_t offset, void *dst, size_t size)
{
    uint8_t *out = dst;
    while (size) {
        size_t n = size < sizeof(s_bounce) ? size : sizeof(s_bounce);
        if (esp_partition_read(part, offset, s_bounce, n) != ESP_OK) {
            return false;
        }
        memcpy(out, s_bounce, n);
        out += n;
        offset += n;
        size -= n;
    }
    return true;
}

/* Called only under the assets lock after signature verification. */
static bool section_bounds(const esp_partition_t *part, uint32_t *fo, uint32_t *fs,
                            uint32_t *so, uint32_t *ss)
{
    if (s_info.schema == 1) {
        *fo = 0; *fs = s_info.payload_size; *so = *ss = 0;
        return true;
    }
    uint8_t header[32];
    return part && esp_partition_read(part, ORB_ASSETS_HEADER_SIZE, header, sizeof(header)) == ESP_OK &&
           orb_assets_parse_sections(header, sizeof(header), s_info.payload_size, fo, fs, so, ss);
}

bool orb_assets_read_payload(uint32_t offset, void *dst, size_t size)
{
    lock();
    const esp_partition_t *part = check_locked() ? assets_partition() : NULL;
    uint32_t fo, fs, so, ss;
    bool ok = part && section_bounds(part, &fo, &fs, &so, &ss) &&
              offset <= fs && size <= fs - offset;
    if (ok) {
        uint8_t header[48];
        ok = esp_partition_read(part, ORB_ASSETS_HEADER_SIZE + fo, header, sizeof(header)) == ESP_OK &&
             memcmp(header, "ORBFONTS", 8) == 0 && orb_assets_rd32(header + 8) == 1 &&
             orb_assets_rd32(header + 12) == orb_fonts_table.pack_count &&
             memcmp(header + 16, orb_fonts_table.pack_id, 16) == 0 &&
             orb_assets_rd32(header + 32) == orb_fonts_table.pack_size &&
             fs == orb_fonts_table.pack_size;
    }
    if (ok) ok = read_bounced(part, ORB_ASSETS_HEADER_SIZE + fo + offset, dst, size);
    unlock();
    return ok;
}

orb_fonts_bitmap_t *orb_fonts_port_map(uint32_t offset, size_t size)
{
    check_mapping_stack();
    lock();
    const esp_partition_t *part = check_locked() ? assets_partition() : NULL;
    uint32_t fo, fs, so, ss;
    bool ok = part && section_bounds(part, &fo, &fs, &so, &ss) && size &&
              offset <= fs && size <= fs - offset;
    size_t start = ORB_ASSETS_HEADER_SIZE + (ok ? fo : 0) + offset;
    /* This board's 24-bit flash cache cannot map across physical 16 MiB.
     * The generator places raw records first. Read/copy remains a safe fallback. */
    ok = ok && start <= 0x1000000u && part->address <= 0x1000000u - start &&
         size <= 0x1000000u - part->address - start;
    orb_fonts_bitmap_t *b = ok ? orb_fonts_port_alloc(sizeof(*b)) : NULL;
    if (b && !s_window) {
        size_t first = ORB_ASSETS_HEADER_SIZE + fo, last = first + fs, limit = 0x1000000u - part->address;
        if (last > limit) last = limit;
        const void *data;
        if (esp_partition_mmap(part, first, last - first, ESP_PARTITION_MMAP_DATA, &data, &s_window_handle) == ESP_OK) {
            s_window = data;
            s_window_start = first;
        } else {
            orb_fonts_port_free(b);
            b = NULL;
        }
    }
    if (b) {
        memset(b, 0, sizeof(*b));
        b->data = s_window + (start - s_window_start);
        b->size = size;
        b->next = s_bitmaps;
        s_bitmaps = b;
        bitmap_backing();
    }
    unlock();
    return b;
}

const uint8_t *orb_fonts_port_bitmap_acquire(orb_fonts_bitmap_t *bitmap)
{
    lock();
    return bitmap->data;
}

void orb_fonts_port_bitmap_release(void)
{
    unlock();
}

void orb_fonts_port_unmap(orb_fonts_bitmap_t *bitmap)
{
    check_mapping_stack();
    lock();
    orb_fonts_bitmap_t **link = &s_bitmaps;
    while (*link && *link != bitmap) link = &(*link)->next;
    if (*link) {
        *link = bitmap->next;
        if (bitmap->copy) orb_fonts_port_free(bitmap->copy);
        orb_fonts_port_free(bitmap);
        if (!s_bitmaps && s_window) {
            esp_partition_munmap(s_window_handle);
            s_window = NULL;
        }
        bitmap_backing();
    }
    unlock();
}

void orb_fonts_port_bitmap_stats(size_t *mapped, size_t *copied)
{
    *mapped = *copied = 0;
    lock();
    for (orb_fonts_bitmap_t *b = s_bitmaps; b; b = b->next) {
        if (b->copy) *copied += b->size;
        else *mapped += b->size;
    }
    unlock();
}

esp_err_t orb_assets_copy_sounds(void **data, size_t *size)
{
    if (!data || !size) return ESP_ERR_INVALID_ARG;
    *data = NULL; *size = 0;
    lock();
    const esp_partition_t *part = check_locked() ? assets_partition() : NULL;
    uint32_t fo, fs, so, ss;
    bool ok = part && section_bounds(part, &fo, &fs, &so, &ss) && ss && ss <= 1024 * 1024;
    void *copy = ok ? heap_caps_malloc(ss, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (copy) ok = read_bounced(part, ORB_ASSETS_HEADER_SIZE + so, copy, ss);
    else ok = false;
    unlock();
    if (!ok) {
        if (copy) heap_caps_free(copy);
        return ESP_ERR_NOT_FOUND;
    }
    *data = copy; *size = ss;
    return ESP_OK;
}
