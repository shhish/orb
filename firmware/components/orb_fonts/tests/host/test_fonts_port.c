/* Device-port integration with bounded physical heap and atomic storage faults.
 * Compression is real zlib through a tinfl-shaped host adapter. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_fonts.h"
#define ESP_PLATFORM 1
#include "orb_fonts_private.h"
#include "orb_assets.h"
#include "orb_fs.h"
#include "orb_memory.h"
#include "miniz.h"

enum { FLOOR = 384 * 1024, OVERHEAD = 32, PREFIX = 37 };
static bool s_ready = true, s_exists;
static uint8_t s_saved[8], s_payload[10000];
static size_t s_saved_size, s_payload_size, s_free, s_live, s_peak, s_max_read;
static unsigned s_allocations, s_fail_allocation, s_reads, s_fs_reads, s_writes, s_prepares, s_prepare_order;
static esp_err_t s_prepare_result = ESP_OK, s_write_result = ESP_OK;
static uint32_t s_fail_read = UINT32_MAX;
typedef union { max_align_t align; size_t size; } allocation_t;

bool orb_fs_ready(void) { return s_ready; }
esp_err_t orb_fs_app_prepare(const char *id)
{
    static const char *const occupied[] = {
        "marble", "beads", "orbit", "ring-snake", "pop", "system", "faces", "time",
    };
    _Static_assert(sizeof(occupied) / sizeof(*occupied) == ORB_FS_MAX_APPS, "fixture fills every namespace");
    s_prepares++;
    if (s_prepare_result != ESP_OK) return s_prepare_result;
    for (unsigned i = 0; i < ORB_FS_MAX_APPS; i++)
        if (!strcmp(id, occupied[i])) return ESP_OK;
    return ESP_ERR_NO_MEM;
}
esp_err_t orb_fs_read(const char *id, const char *key, void *data, size_t capacity, size_t *size)
{
    assert(!strcmp(id, "system") && !strcmp(key, "text") && capacity == 3);
    s_fs_reads++;
    if (!s_exists) return ESP_ERR_NOT_FOUND;
    *size = s_saved_size;
    if (s_saved_size > capacity) return ESP_ERR_INVALID_SIZE;
    memcpy(data, s_saved, s_saved_size);
    return ESP_OK;
}
esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size)
{
    assert(!strcmp(id, "system") && !strcmp(key, "text") && size == 3);
    s_writes++;
    if (s_write_result != ESP_OK) return s_write_result;
    memcpy(s_saved, data, size);
    s_saved_size = size;
    s_exists = true;
    return ESP_OK;
}

size_t heap_caps_get_free_size(unsigned caps)
{
    assert(caps == MALLOC_CAP_SPIRAM);
    return s_free;
}
void *orb_memory_alloc(orb_memory_owner_t owner, size_t size)
{
    assert(owner == ORB_MEMORY_FONTS);
    s_allocations++;
    if (s_allocations == s_fail_allocation) return NULL;
    assert(size <= SIZE_MAX - OVERHEAD && s_free >= size + OVERHEAD);
    allocation_t *allocation = calloc(1, sizeof(*allocation) + size);
    assert(allocation);
    allocation->size = size;
    s_free -= size + OVERHEAD;
    s_live += size + OVERHEAD;
    if (s_live > s_peak) s_peak = s_live;
    return allocation + 1;
}
void orb_memory_free(void *pointer)
{
    if (!pointer) return;
    allocation_t *allocation = (allocation_t *)pointer - 1;
    if (allocation->size == sizeof(tinfl_decompressor)) tinfl_test_cleanup(pointer);
    s_live -= allocation->size + OVERHEAD;
    s_free += allocation->size + OVERHEAD;
    free(allocation);
}
void lv_obj_raster_cache_release_frame(void) { assert(s_prepare_order++ == 0); }
size_t orb_memory_cache_trim(size_t bytes)
{
    assert(s_prepare_order++ == 1 && bytes == SIZE_MAX);
    return 0;
}
bool orb_assets_read_payload(uint32_t offset, void *data, size_t size)
{
    s_reads++;
    if (size > s_max_read) s_max_read = size;
    if (offset == s_fail_read || offset > s_payload_size || size > s_payload_size - offset) return false;
    memcpy(data, s_payload + offset, size);
    return true;
}

static void preferences(void)
{
    /* Five games plus System, Faces and Time already fill all eight slots.
     * Accessibility must reuse System and remain writable in that state. */
    assert(orb_fs_app_prepare("accessibility") == ESP_ERR_NO_MEM);
    for (unsigned size = 0; size < ORB_CONTENT_SIZE_COUNT; size++) {
        for (unsigned bold = 0; bold < 2; bold++) {
            assert(orb_fonts_port_preferences_save(size, bold));
            assert(s_saved_size == 3 && s_saved[0] == 1 && s_saved[1] == size && s_saved[2] == bold);
            orb_content_size_t actual = ORB_CONTENT_SIZE_DEFAULT;
            bool actual_bold = !bold;
            orb_fonts_port_preferences_load(&actual, &actual_bold);
            assert(actual == size && actual_bold == bold);
        }
    }
    static const uint8_t invalid[][3] = {{0, 0, 0}, {2, 0, 0}, {1, 3, 0}, {1, 255, 0}, {1, 0, 2}};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid) + 5; i++) {
        s_exists = true;
        s_saved_size = 3;
        memcpy(s_saved, (uint8_t[]){1, 0, 0}, 3);
        if (i < sizeof(invalid) / sizeof(*invalid)) memcpy(s_saved, invalid[i], 3);
        else if (i == 5) s_exists = false;
        else s_saved_size = i - 6; /* zero, one and two bytes, then oversized */
        if (i == 9) s_saved_size = 4;
        orb_content_size_t size = ORB_CONTENT_SIZE_AX3;
        bool bold = true;
        orb_fonts_port_preferences_load(&size, &bold);
        assert(size == ORB_CONTENT_SIZE_AX3 && bold);
    }
    assert(orb_fonts_port_preferences_save(ORB_CONTENT_SIZE_DEFAULT, false));
    uint8_t old[3];
    memcpy(old, s_saved, 3);
    s_write_result = ESP_FAIL;
    assert(!orb_fonts_port_preferences_save(ORB_CONTENT_SIZE_AX3, true));
    assert(!memcmp(old, s_saved, 3));
    s_write_result = ESP_OK;
    s_prepare_result = ESP_FAIL;
    unsigned writes = s_writes;
    assert(!orb_fonts_port_preferences_save(ORB_CONTENT_SIZE_AX3, true));
    assert(s_writes == writes && !memcmp(old, s_saved, 3));
    s_prepare_result = ESP_OK;
    s_ready = false;
    unsigned reads = s_fs_reads, prepares = s_prepares;
    orb_content_size_t size = ORB_CONTENT_SIZE_AX3;
    bool bold = true;
    orb_fonts_port_preferences_load(&size, &bold);
    assert(size == ORB_CONTENT_SIZE_AX3 && bold && s_fs_reads == reads);
    assert(!orb_fonts_port_preferences_save(ORB_CONTENT_SIZE_AX3, true));
    assert(s_prepares == prepares && s_writes == writes);
    s_ready = true;
}

static void floor_boundary(void)
{
    s_free = FLOOR + 128 + OVERHEAD - 1;
    unsigned allocations = s_allocations;
    assert(!orb_fonts_port_alloc(128) && s_allocations == allocations);
    s_free++;
    void *pointer = orb_fonts_port_alloc(128);
    assert(pointer && s_free == FLOOR);
    assert(!orb_fonts_port_alloc(1));
    orb_fonts_port_free(pointer);
    assert(!s_live && s_free == FLOOR + 128 + OVERHEAD);
    s_free = SIZE_MAX;
    assert(!orb_fonts_port_alloc(SIZE_MAX));
    orb_fonts_port_prepare();
    assert(s_prepare_order == 2);
}

static void streaming(void)
{
    uint8_t original[8192], output[sizeof(original) + 1];
    uint32_t random = 0x918dab23;
    for (unsigned i = 0; i < sizeof(original); i++) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        original[i] = random;
    }
    uLongf stored = sizeof(s_payload) - PREFIX - 1;
    assert(compress2(s_payload + PREFIX, &stored, original, sizeof(original), 1) == Z_OK);
    assert(stored > 3 * 1024);
    s_payload_size = PREFIX + stored;
    s_free = FLOOR + 30000;
    size_t before = s_free;
    s_peak = s_max_read = s_reads = 0;
    assert(orb_fonts_port_read_inflate(output, sizeof(original), PREFIX, stored));
    assert(!memcmp(output, original, sizeof(original)) && s_reads > 3 && s_max_read <= 1024);
    assert(!s_live && s_free == before && s_peak <= 10992 + 1024 + 2 * OVERHEAD);
    assert(orb_fonts_port_inflate(output, sizeof(original), s_payload + PREFIX, stored));
    assert(!memcmp(output, original, sizeof(original)) && !s_live && s_free == before);
    assert(!orb_fonts_port_read_inflate(output, sizeof(original), PREFIX, stored - 1));
    s_payload[s_payload_size - 1] ^= 1;
    assert(!orb_fonts_port_read_inflate(output, sizeof(original), PREFIX, stored));
    assert(!orb_fonts_port_inflate(output, sizeof(original), s_payload + PREFIX, stored));
    s_payload[s_payload_size - 1] ^= 1;
    assert(!orb_fonts_port_read_inflate(output, sizeof(original) - 1, PREFIX, stored));
    assert(!orb_fonts_port_read_inflate(output, sizeof(output), PREFIX, stored));
    s_payload[s_payload_size++] = 0;
    assert(!orb_fonts_port_read_inflate(output, sizeof(original), PREFIX, stored + 1));
    s_payload_size--;
    s_fail_read = PREFIX + 1024;
    assert(!orb_fonts_port_read_inflate(output, sizeof(original), PREFIX, stored));
    s_fail_read = UINT32_MAX;
    s_payload_size--;
    assert(!orb_fonts_port_read_inflate(output, sizeof(original), PREFIX, stored));
    s_payload_size++;
    for (unsigned fail = 1; fail <= 2; fail++) {
        s_fail_allocation = s_allocations + fail;
        assert(!orb_fonts_port_read_inflate(output, sizeof(original), PREFIX, stored));
        assert(!s_live && s_free == before);
    }
    s_fail_allocation = 0;
    assert(!s_live && s_free == before);
}

int main(void)
{
    preferences();
    floor_boundary();
    streaming();
    puts("font port: saved pairs, invalid storage, atomic save failure, 384 KiB floor and checked 1 KiB streaming passed");
    return 0;
}
