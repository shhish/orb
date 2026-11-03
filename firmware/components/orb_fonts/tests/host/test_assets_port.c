/* Real device port with fake flash, signature/hash engine and task scheduler.
 * test_storage_artifacts.py additionally exercises IDF trust/RSA on built images. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_fonts_private.h"
#include "assets_test_platform.h"
#include "../../port/orb_assets_esp.c"

static uint8_t s_flash[8192];
static esp_partition_t s_partition = {.size = sizeof(s_flash)};
static bool s_signature_valid = true;
static bool s_read_fail;
static bool s_hash_fail;
static bool s_task_fail;
static bool s_alloc_fail;
static unsigned s_allocations;
static unsigned s_workers;
static unsigned s_joined;
static unsigned s_signatures;
static unsigned s_tasks;
static unsigned s_maps;
static bool s_map_fail;
static bool s_mapped[16];
static bool s_wait_for_switch;
static unsigned s_switch_waits;
static size_t s_font_live, s_font_requests, s_font_fail_request;
static size_t s_flash_backing;
const orb_fonts_table_t orb_fonts_table = {.pack_size = 48};

typedef union { max_align_t alignment; size_t size; } font_allocation_t;

void orb_memory_cache_set_flash_backing(size_t bytes)
{
    assert(s_lock && s_lock->available == 0);
    s_flash_backing = bytes;
}

void *orb_fonts_port_alloc(size_t size)
{
    s_font_requests++;
    if (s_font_requests == s_font_fail_request) return NULL;
    font_allocation_t *allocation = malloc(sizeof(*allocation) + size);
    assert(allocation);
    allocation->size = size;
    s_font_live += size;
    return allocation + 1;
}

void orb_fonts_port_free(void *pointer)
{
    if (!pointer) return;
    font_allocation_t *allocation = (font_allocation_t *)pointer - 1;
    assert(s_font_live >= allocation->size);
    s_font_live -= allocation->size;
    free(allocation);
}

esp_err_t esp_partition_mmap(const esp_partition_t *part, size_t offset, size_t size,
                            int flags, const void **data, esp_partition_mmap_handle_t *handle)
{
    assert(part == &s_partition && flags == ESP_PARTITION_MMAP_DATA);
    assert(offset <= sizeof(s_flash) && size <= sizeof(s_flash) - offset);
    assert(s_lock && s_lock->available == 0);
    if (s_map_fail) return ESP_FAIL;
    unsigned slot = 1;
    while (slot < 16 && s_mapped[slot]) slot++;
    assert(slot < 16);
    s_mapped[slot] = true;
    s_maps++;
    *handle = slot;
    *data = s_flash + offset;
    return ESP_OK;
}

void esp_partition_munmap(esp_partition_mmap_handle_t handle)
{
    assert(handle < 16 && s_mapped[handle] && s_maps);
    assert(s_lock && s_lock->available == 0);
    s_mapped[handle] = false;
    s_maps--;
}

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    (void)type; (void)subtype;
    assert(strcmp(label, "assets") == 0);
    return &s_partition;
}

esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *dst, size_t size)
{
    assert(part == &s_partition);
    if (s_read_fail || offset > sizeof(s_flash) || size > sizeof(s_flash) - offset) return ESP_FAIL;
    memcpy(dst, s_flash + offset, size);
    return ESP_OK;
}

esp_err_t esp_secure_boot_verify_sbv2_signature_block(const ets_secure_boot_signature_t *signature,
                                                     const uint8_t *digest, uint8_t *verified)
{
    assert(memcmp(signature, s_flash + 4096, sizeof(*signature)) == 0);
    assert(digest && !verified);
    s_signatures++;
    return s_signature_valid ? ESP_OK : ESP_FAIL;
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(size == 4096 || size == 24);
    if (s_alloc_fail) return NULL;
    s_allocations++;
    return malloc(size);
}
void heap_caps_free(void *pointer)
{
    assert(pointer && s_allocations == 1);
    s_allocations--;
    free(pointer);
}
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; assert(!s_workers); return 65536; }
size_t heap_caps_get_largest_free_block(unsigned caps) { return heap_caps_get_free_size(caps); }
size_t heap_caps_get_minimum_free_size(unsigned caps) { return heap_caps_get_free_size(caps); }
void assets_test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t handle) { assert(!handle && s_workers == 1); return 4096; }

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    buffer->available = 1;
    return buffer;
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *buffer)
{
    buffer->available = 0;
    return buffer;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned timeout)
{
    (void)timeout;
    if (semaphore == s_switch_lock) {
        assert(s_lock->available == 1); /* no inversion with per-read locking */
        if (!semaphore->available && s_wait_for_switch) {
            assert(!s_updating && s_trusted);
            s_wait_for_switch = false;
            s_switch_waits++;
            orb_fonts_port_end_switch(); /* deterministic blocked-switch completion */
        }
    }
    assert(semaphore->available == 1);
    semaphore->available = 0;
    return 1;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore->available == 0);
    semaphore->available = 1;
    return 1;
}

int xTaskCreateWithCaps(void (*function)(void *), const char *name, unsigned stack,
                        void *context, unsigned priority, TaskHandle_t *handle, unsigned caps)
{
    (void)name; (void)priority;
    assert(stack == 8192 && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_tasks++;
    if (s_task_fail) return 0;
    assert(s_workers == 0);
    s_workers++;
    *handle = &s_workers;
    function(context);
    return pdPASS;
}

void vTaskSuspend(TaskHandle_t handle) { assert(!handle); }
void vTaskDeleteWithCaps(TaskHandle_t handle)
{
    assert(handle == &s_workers && s_workers == 1);
    assert(!s_allocations);
    s_workers--;
    s_joined++;
}
int psa_hash_setup(psa_hash_operation_t *hash, int algorithm)
{
    assert(algorithm == PSA_ALG_SHA_256);
    hash->active = 1;
    return PSA_SUCCESS;
}
int psa_hash_update(psa_hash_operation_t *hash, const void *data, size_t size)
{
    (void)data; (void)size;
    assert(hash->active);
    return s_hash_fail ? -1 : PSA_SUCCESS;
}
int psa_hash_finish(psa_hash_operation_t *hash, void *digest, size_t size, size_t *actual)
{
    assert(hash->active && size == 32);
    hash->active = 0;
    memset(digest, 0, 32);
    *actual = 32;
    return PSA_SUCCESS;
}
int psa_hash_abort(psa_hash_operation_t *hash) { hash->active = 0; return PSA_SUCCESS; }

static void write32(uint8_t *p, uint32_t n)
{
    for (int i = 0; i < 4; i++) p[i] = n >> (8 * i);
}

static void test_bitmap_updates(void)
{
    uint8_t saved[48];
    memcpy(saved, s_flash + 64, sizeof(saved));
    assert(!orb_fonts_port_map(49, 1) && !orb_fonts_port_map(47, 2));
    assert(!orb_fonts_port_map(0, 0));
    s_partition.address = 0xffffff;
    assert(!orb_fonts_port_map(8, 8)); /* cannot cross the cache's 16 MiB limit */
    s_partition.address = 0;
    s_map_fail = true;
    assert(!orb_fonts_port_map(8, 8) && !s_font_live && !s_maps);
    s_map_fail = false;
    s_font_fail_request = s_font_requests + 1;
    assert(!orb_fonts_port_map(8, 8) && !s_font_live && !s_maps);
    s_font_fail_request = 0;
    orb_fonts_bitmap_t *first = orb_fonts_port_map(8, 8);
    orb_fonts_bitmap_t *second = orb_fonts_port_map(24, 16);
    assert(first && second && s_maps == 1); /* one shared window */
    /* Retiring one face must not take a neighbour's flash pages with it. */
    orb_fonts_bitmap_t *retired = orb_fonts_port_map(16, 8);
    assert(retired && s_maps == 1);
    orb_fonts_port_unmap(retired);
    assert(s_maps == 1 && orb_fonts_port_bitmap_acquire(second));
    orb_fonts_port_bitmap_release();
    assert(s_flash_backing == 24);
    size_t metadata = s_font_live, mapped, copied;
    orb_fonts_port_bitmap_stats(&mapped, &copied);
    assert(mapped == 24 && copied == 0);
    const uint8_t *bytes = orb_fonts_port_bitmap_acquire(first);
    assert(bytes == s_flash + 72 && s_lock->available == 0);
    orb_fonts_port_bitmap_release();
    assert(s_lock->available == 1);

    /* The second copy fails after the first succeeds: abort before invalidating
     * trust or unmapping either span, and free the partial staging allocation. */
    s_font_fail_request = s_font_requests + 2;
    assert(orb_assets_begin_update() == ESP_ERR_NO_MEM);
    assert(s_font_live == metadata && s_maps == 1 && orb_assets_version() == 7);
    assert(s_flash_backing == 24);
    uint8_t payload[48];
    assert(orb_assets_read_payload(0, payload, sizeof(payload)));
    orb_fonts_port_bitmap_stats(&mapped, &copied);
    assert(mapped == 24 && copied == 0);
    s_font_fail_request = 0;
    assert(orb_fonts_port_begin_switch());
    assert(s_switch_lock->available == 0);
    assert(orb_assets_read_payload(0, payload, sizeof(payload))); /* reads remain independent */
    s_wait_for_switch = true;
    assert(orb_assets_begin_update() == ESP_OK);
    assert(s_switch_waits == 1 && s_switch_lock->available == 1);
    assert(!orb_fonts_port_begin_switch()); /* never retire fonts during OTA */
    assert(!s_maps && s_font_live == metadata + 24);
    assert(!s_flash_backing);
    assert(orb_assets_version() == 0 && !orb_fonts_port_map(0, 8));
    orb_fonts_port_bitmap_stats(&mapped, &copied);
    assert(mapped == 0 && copied == 24);
    memset(s_flash + 64, 0xa5, 48); /* destructive single-slot write */
    bytes = orb_fonts_port_bitmap_acquire(first);
    assert(!memcmp(bytes, saved + 8, 8));
    orb_fonts_port_bitmap_release();
    bytes = orb_fonts_port_bitmap_acquire(second);
    assert(!memcmp(bytes, saved + 24, 16));
    orb_fonts_port_bitmap_release();
    orb_assets_end_update();
    assert(!orb_fonts_port_begin_switch()); /* updated image needs reboot/verification */
    size_t requests = s_font_requests;
    assert(orb_assets_begin_update() == ESP_OK); /* copied leases survive a retry */
    assert(s_font_requests == requests);
    orb_assets_end_update();
    orb_fonts_port_unmap(second);
    orb_fonts_port_unmap(first);
    orb_fonts_port_bitmap_stats(&mapped, &copied);
    assert(!mapped && !copied && !s_font_live && !s_maps);
    assert(!s_flash_backing);

    memcpy(s_flash + 64, saved, sizeof(saved));
    s_checked = s_trusted = true; /* restore the fixture's original verified boot */
    first = orb_fonts_port_map(8, 8);
    assert(first && s_maps == 1 && s_flash_backing == 8);
    orb_fonts_port_unmap(first); /* normal font retirement unmaps without copying */
    assert(!s_maps && !s_font_live);
    assert(!s_flash_backing);
}

int main(void)
{
    unsigned checks = 0;
#define CHECK(expression) do { assert(expression); checks++; } while (0)
    memcpy(s_flash, "ORBASSET", 8);
    write32(s_flash + 8, 1); write32(s_flash + 12, 64);
    write32(s_flash + 16, 48); write32(s_flash + 20, 4096);
    write32(s_flash + 24, 7); write32(s_flash + 28, 1);
    memcpy(s_flash + 64, "ORBFONTS", 8);
    write32(s_flash + 72, 1); write32(s_flash + 96, 48);
    uint8_t payload[48];
    CHECK(orb_assets_version() == 0);
    CHECK(!orb_fonts_port_map(0, 8));
    CHECK(!orb_assets_read_payload(0, payload, 48));
    CHECK(s_signatures == 0 && s_tasks == 0); /* callbacks never verify */
    CHECK(orb_assets_verify_partition(NULL, 8192, NULL) != ESP_OK);
    CHECK(orb_assets_verify_partition(&s_partition, 8191, NULL) != ESP_OK);
    s_signature_valid = false;
    CHECK(orb_assets_verify_partition(&s_partition, 8192, NULL) != ESP_OK);
    s_signature_valid = true;
    s_hash_fail = true;
    CHECK(orb_assets_verify_partition(&s_partition, 8192, NULL) != ESP_OK);
    s_hash_fail = false;
    s_alloc_fail = true;
    CHECK(orb_assets_verify_partition(&s_partition, 8192, NULL) == ESP_ERR_NO_MEM);
    CHECK(s_allocations == 0);
    s_alloc_fail = false;
    s_task_fail = true;
    CHECK(orb_assets_init() == ESP_ERR_NO_MEM);
    CHECK(!s_verifying && s_checked && !s_trusted);
    CHECK(orb_assets_version() == 0);
    s_task_fail = false;
    s_checked = false; /* simulate a fresh boot */
    CHECK(orb_assets_init() == ESP_OK);
    CHECK(s_tasks == 2);
    CHECK(s_workers == 0 && s_joined == 1 && s_allocations == 0);
    unsigned verified = s_signatures;
    CHECK(orb_assets_init() == ESP_OK);
    CHECK(orb_assets_version() == 7);
    CHECK(orb_assets_read_payload(0, payload, 48));
    CHECK(memcmp(payload, s_flash + 64, 48) == 0);
    CHECK(!orb_assets_read_payload(49, payload, 1));
    CHECK(!orb_assets_read_payload(47, payload, 2));
    CHECK(s_signatures == verified && s_tasks == 2);
    test_bitmap_updates();
    CHECK(orb_assets_begin_update() == ESP_OK);
    CHECK(orb_assets_begin_update() == ESP_ERR_INVALID_STATE);
    CHECK(orb_assets_version() == 0);
    CHECK(!orb_assets_read_payload(0, payload, 48));
    orb_assets_end_update();
    CHECK(orb_assets_version() == 0);
    CHECK(!orb_assets_read_payload(0, payload, 48));
    CHECK(s_signatures == verified && s_tasks == 2); /* no crypto on UI stack after update */
    CHECK(orb_assets_begin_update() == ESP_OK); /* failed update can be retried */
    orb_assets_end_update();
    s_checked = false;
    CHECK(orb_assets_init() == ESP_OK);
    s_flash[80] = 1; /* signed but incompatible pack id must not use compiled offsets */
    CHECK(!orb_assets_read_payload(0, payload, 48));
    void *sounds = NULL; size_t sound_bytes = 0;
    CHECK(orb_assets_copy_sounds(&sounds, &sound_bytes) != ESP_OK && !sounds && !sound_bytes);
    memset(s_flash + 64, 0, 128);
    write32(s_flash + 8, 2); write32(s_flash + 28, 2); write32(s_flash + 16, 104);
    memcpy(s_flash + 64, "ORBBNDL2", 8);
    write32(s_flash + 72, 2); write32(s_flash + 76, 32); write32(s_flash + 80, 48);
    write32(s_flash + 84, 80); write32(s_flash + 88, 24); write32(s_flash + 92, 104);
    memcpy(s_flash + 96, "ORBFONTS", 8); write32(s_flash + 104, 1); write32(s_flash + 128, 48);
    memcpy(s_flash + 144, "ORBSOUND", 8);
    s_checked = false;
    CHECK(orb_assets_init() == ESP_OK);
    CHECK(orb_assets_read_payload(0, payload, 48));
    CHECK(!memcmp(payload, s_flash + 96, 48));
    CHECK(orb_assets_copy_sounds(&sounds, &sound_bytes) == ESP_OK && sounds && sound_bytes == 24);
    CHECK(!memcmp(sounds, s_flash + 144, 24));
    heap_caps_free(sounds); sounds = NULL;
    s_alloc_fail = true;
    CHECK(orb_assets_copy_sounds(&sounds, &sound_bytes) != ESP_OK && !sounds && !sound_bytes);
    s_alloc_fail = false;
    write32(s_flash + 84, 76); /* signed but overlapping sections */
    CHECK(!orb_assets_read_payload(0, payload, 48));
    CHECK(orb_assets_copy_sounds(&sounds, &sound_bytes) != ESP_OK);
    s_read_fail = true;
    CHECK(orb_assets_verify_partition(&s_partition, 8192, NULL) != ESP_OK);
    printf("assets port: %u checks passed\n", checks);
    return 0;
}
