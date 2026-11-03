/*
 * orb_fonts' platform hooks on the device: the font pack is read from the assets
 * partition with the flash driver (esp_partition_read, which takes the flash lock, so a
 * concurrent OTA write can't corrupt what it returns), inflated with the ROM's miniz and
 * kept in PSRAM. Raw records instead lease signed flash bitmaps through the
 * assets port; its update lock protects the complete LVGL decode.
 */

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "miniz.h"
#include "orb_assets.h"
#include "orb_fs.h"
#include "orb_fonts_private.h"
#include "orb_memory.h"
#include "sdkconfig.h"

/* Keep room for a 128 KiB TLS handshake and further network payload growth.
 * This is physical free heap, in addition to the existing owner ceiling. */
#define SWITCH_FLOOR (384u * 1024u)
_Static_assert(sizeof(tinfl_decompressor) == 10992, "update tools/fonts/budget.py for this inflater");

void orb_fonts_port_prepare(void)
{
    /* Only the locked UI may call eviction callbacks. Leased entries survive. */
    lv_obj_raster_cache_release_frame();
    orb_memory_cache_trim(SIZE_MAX);
}

void orb_fonts_port_preferences_load(orb_content_size_t *size, bool *bold)
{
    uint8_t saved[3];
    size_t bytes = 0;
    if (orb_fs_ready() && orb_fs_read("system", "text", saved, sizeof(saved), &bytes) == ESP_OK &&
        bytes == sizeof(saved) && saved[0] == 1 && saved[1] < ORB_CONTENT_SIZE_COUNT && saved[2] <= 1) {
        *size = (orb_content_size_t)saved[1];
        *bold = saved[2];
    }
}

bool orb_fonts_port_preferences_save(orb_content_size_t size, bool bold)
{
    /* Reuse the system namespace: five games, time and faces already bring
     * the eight-app filesystem registry to its limit. */
    const uint8_t saved[] = {1, (uint8_t)size, bold};
    return orb_fs_ready() && orb_fs_app_prepare("system") == ESP_OK &&
        orb_fs_write("system", "text", saved, sizeof(saved)) == ESP_OK;
}

bool orb_fonts_port_read(uint32_t offset, void *dst, size_t size)
{
    return orb_assets_read_payload(offset, dst, size);
}

bool orb_fonts_port_inflate(void *dst, size_t dst_size, const void *src, size_t src_size)
{
    /* The roughly 11 KB workspace follows the font blobs' PSRAM-first policy.
     * An 8BIT-only request prefers internal RAM, competing with BLE/DMA even
     * when malloc's external-memory threshold is lowered. */
    tinfl_decompressor *d = orb_fonts_port_alloc(sizeof(*d));
    if (!d) {
        return false;
    }
    tinfl_init(d);
    size_t in = src_size, out = dst_size;
    tinfl_status st = tinfl_decompress(d, src, &in, dst, dst, &out,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    orb_fonts_port_free(d);
    return st == TINFL_STATUS_DONE && out == dst_size; /* DONE includes a matching Adler-32 */
}

bool orb_fonts_port_read_inflate(void *dst, size_t dst_size, uint32_t offset, size_t stored)
{
    tinfl_decompressor *d = orb_fonts_port_alloc(sizeof(*d));
    uint8_t *input = d ? orb_fonts_port_alloc(1024) : NULL;
    bool ok = false;
    if (!input) goto done;
    tinfl_init(d);
    size_t produced = 0;
    while (stored) {
        size_t chunk = stored < 1024 ? stored : 1024;
        if (!orb_fonts_port_read(offset, input, chunk)) break;
        size_t consumed = chunk, output = dst_size - produced;
        tinfl_status status = tinfl_decompress(d, input, &consumed, dst,
            (uint8_t *)dst + produced, &output, TINFL_FLAG_PARSE_ZLIB_HEADER |
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (stored > chunk ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        produced += output;
        if (status == TINFL_STATUS_DONE) {
            ok = consumed == chunk && stored == chunk && produced == dst_size;
            break;
        }
        if (status != TINFL_STATUS_NEEDS_MORE_INPUT || consumed != chunk) break;
        stored -= chunk;
        offset += chunk;
    }
done:
    orb_fonts_port_free(input);
    orb_fonts_port_free(d);
    return ok;
}

void *orb_fonts_port_alloc(size_t size)
{
    /* A failed font load must use rescue typography, never consume networking or
     * DMA's internal reserve. Parsed metadata follows the same policy. */
    size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    /* The owner header is 16 bytes on Xtensa. Leave an extra 16 for the heap
     * backend's alignment/header rather than consuming the TLS margin. */
    if (size > SIZE_MAX - SWITCH_FLOOR - 32 || available < size + SWITCH_FLOOR + 32) return NULL;
    return orb_memory_alloc(ORB_MEMORY_FONTS, size);
}

void orb_fonts_port_free(void *ptr)
{
    orb_memory_free(ptr);
}
