#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ORB_ASSETS_HEADER_SIZE 64u
#define ORB_ASSETS_SECTOR_SIZE 4096u
#define ORB_ASSETS_MAX_SIZE (8u * 1024u * 1024u)

typedef struct {
    uint32_t schema;
    uint32_t payload_size;
    uint32_t signed_size;
    uint32_t content_version;
    uint8_t payload_sha256[32];
} orb_assets_info_t;

static inline uint32_t orb_assets_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Bounds only. Callers must verify the SBv2 signature before trusting payloads. */
static inline bool orb_assets_parse_header(const uint8_t *header, size_t size,
                                           size_t partition_size, size_t image_size,
                                           orb_assets_info_t *info)
{
    if (!header || !info || size < ORB_ASSETS_HEADER_SIZE ||
        memcmp(header, "ORBASSET", 8) != 0 ||
        (orb_assets_rd32(header + 8) != 1 && orb_assets_rd32(header + 8) != 2) ||
        orb_assets_rd32(header + 12) != ORB_ASSETS_HEADER_SIZE ||
        orb_assets_rd32(header + 28) != orb_assets_rd32(header + 8)) {
        return false;
    }
    uint32_t payload = orb_assets_rd32(header + 16);
    uint32_t signed_size = orb_assets_rd32(header + 20);
    uint32_t version = orb_assets_rd32(header + 24);
    if (!version || payload < 48 || payload > ORB_ASSETS_MAX_SIZE - 2 * ORB_ASSETS_SECTOR_SIZE ||
        signed_size != ((ORB_ASSETS_HEADER_SIZE + payload + ORB_ASSETS_SECTOR_SIZE - 1) &
                        ~(ORB_ASSETS_SECTOR_SIZE - 1)) ||
        (uint64_t)signed_size + ORB_ASSETS_SECTOR_SIZE > partition_size ||
        (uint64_t)signed_size + ORB_ASSETS_SECTOR_SIZE > ORB_ASSETS_MAX_SIZE ||
        (image_size && image_size != (size_t)signed_size + ORB_ASSETS_SECTOR_SIZE)) {
        return false;
    }
    *info = (orb_assets_info_t){.schema = orb_assets_rd32(header + 8), .payload_size = payload, .signed_size = signed_size,
                               .content_version = version};
    memcpy(info->payload_sha256, header + 32, 32);
    return true;
}

/* Authenticated bundle bounds. Reject overlap, wraparound and unaligned PCM. */
static inline bool orb_assets_parse_sections(const uint8_t *header, size_t size,
                                              uint32_t payload_size,
                                              uint32_t *font_offset, uint32_t *font_size,
                                              uint32_t *sound_offset, uint32_t *sound_size)
{
    if (!header || !font_offset || !font_size || !sound_offset || !sound_size ||
        size < 32 || memcmp(header, "ORBBNDL2", 8) ||
        orb_assets_rd32(header + 8) != 2) return false;
    uint32_t fo = orb_assets_rd32(header + 12), fs = orb_assets_rd32(header + 16);
    uint32_t so = orb_assets_rd32(header + 20), ss = orb_assets_rd32(header + 24);
    if (fo != 32 || fs < 48 || fs > payload_size || fo > payload_size - fs ||
        so < fo + fs || so % 4 || ss < 24 || so > payload_size ||
        ss != payload_size - so || orb_assets_rd32(header + 28) != payload_size) return false;
    *font_offset = fo; *font_size = fs; *sound_offset = so; *sound_size = ss;
    return true;
}
