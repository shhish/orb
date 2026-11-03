#include "orb_sound_core.h"

#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "orb_assets.h"
#include "esp_heap_caps.h"
#else
#include <stdio.h>
#endif

static const uint8_t *s_pack;
static size_t s_size;
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool orb_sound_pcm_bind(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    const size_t directory_end = 24 + 16 * ORB_SOUND_CUE_COUNT;
    if (!p || bytes < directory_end || memcmp(p, "ORBSOUND", 8) || rd32(p + 8) != 1 ||
        rd32(p + 12) != ORB_SOUND_SAMPLE_RATE || rd32(p + 16) != ORB_SOUND_CUE_COUNT ||
        rd32(p + 20) != bytes) return false;
    size_t previous_end = directory_end;
    for (unsigned id = 0; id < ORB_SOUND_CUE_COUNT; id++) {
        const uint8_t *entry = p + 24 + id * 16;
        uint32_t offset = rd32(entry + 4), count = rd32(entry + 8), flags = rd32(entry + 12);
        if (rd32(entry) != id || offset < previous_end || offset % 2 || !count ||
            count > 2 * ORB_SOUND_SAMPLE_RATE || offset > bytes || count > (bytes - offset) / 2 ||
            flags != (unsigned)orb_sound_is_alert(id)) return false;
        previous_end = offset + count * 2;
    }
    if (previous_end != bytes) return false;
    s_pack = p; s_size = bytes;
    return true;
}

bool orb_sound_pcm_init(void)
{
    if (s_pack) return true;
    void *data = NULL;
    size_t bytes = 0;
#ifdef ESP_PLATFORM
    if (orb_assets_copy_sounds(&data, &bytes) != ESP_OK) return false;
#else
#ifndef ORB_SOUND_HOST_PACK
#define ORB_SOUND_HOST_PACK "tools/sounds/out/palette.orbs"
#endif
    FILE *file = fopen(ORB_SOUND_HOST_PACK, "rb");
    if (!file) file = fopen("../../../../../tools/sounds/out/palette.orbs", "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) || (long)(bytes = (size_t)ftell(file)) <= 0 || bytes > 1024 * 1024 ||
        fseek(file, 0, SEEK_SET) || !(data = malloc(bytes))) { fclose(file); return false; }
    bool read = fread(data, 1, bytes, file) == bytes;
    fclose(file);
    if (!read) { free(data); return false; }
#endif
    if (orb_sound_pcm_bind(data, bytes)) return true;
#ifdef ESP_PLATFORM
    heap_caps_free(data);
#else
    free(data);
#endif
    return false;
}

size_t orb_sound_pcm_bytes(void) { return s_size; }

size_t orb_sound_pcm_count(orb_sound_cue_t cue)
{
    return s_pack && s_size && cue >= 0 && cue < ORB_SOUND_CUE_COUNT ?
           rd32(s_pack + 24 + (unsigned)cue * 16 + 8) : 0;
}

size_t orb_sound_pcm(orb_sound_cue_t cue, size_t offset, int16_t *pcm, size_t capacity)
{
    size_t count = orb_sound_pcm_count(cue);
    if (!pcm || offset >= count) return 0;
    size_t n = count - offset < capacity ? count - offset : capacity;
    const uint8_t *data = s_pack + rd32(s_pack + 24 + (unsigned)cue * 16 + 4) + offset * 2;
    /* Explicit LE reads keep the file format portable in host tests. */
    for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)((unsigned)data[i * 2] | (unsigned)data[i * 2 + 1] << 8);
    return n;
}

size_t orb_sound_pcm_variant(orb_sound_cue_t cue, size_t offset, int16_t *pcm,
                             size_t capacity, unsigned sequence)
{
    size_t n = orb_sound_pcm(cue, offset, pcm, capacity);
    /* hig.md §8: tiny level variation on repeated ticks, no pitch/length drift. */
    unsigned gain = 100 - sequence % 3;
    for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)((int32_t)pcm[i] * (int32_t)gain / 100);
    return n;
}
