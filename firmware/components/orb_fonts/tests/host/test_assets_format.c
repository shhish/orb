#include <assert.h>
#include <stdio.h>
#include "orb_assets_format.h"

static void write32(uint8_t *p, uint32_t n)
{
    for (int i = 0; i < 4; i++) p[i] = n >> (8 * i);
}

int main(void)
{
    uint8_t header[64] = {0};
    memcpy(header, "ORBASSET", 8);
    write32(header + 8, 1);
    write32(header + 12, 64);
    write32(header + 16, 48);
    write32(header + 20, 4096);
    write32(header + 24, 1);
    write32(header + 28, 1);
    orb_assets_info_t info;
    unsigned checks = 0;
    assert(orb_assets_parse_header(header, 64, ORB_ASSETS_MAX_SIZE, 8192, &info)); checks++;
    assert(info.content_version == 1 && info.payload_size == 48 && info.signed_size == 4096); checks++;
    assert(orb_assets_parse_header(header, 64, 8192, 0, &info)); checks++;
    for (size_t size = 0; size < 64; size++) {
        assert(!orb_assets_parse_header(header, size, ORB_ASSETS_MAX_SIZE, 8192, &info)); checks++;
    }
    for (size_t offset = 0; offset < 32; offset++) {
        uint8_t saved = header[offset];
        header[offset] ^= 0x80;
        /* Small payload changes still fit the same sector; signature/digest
         * verification rejects those after this structural-only parser. */
        bool allowed = offset == 16 || (offset >= 24 && offset < 28);
        assert(orb_assets_parse_header(header, 64, ORB_ASSETS_MAX_SIZE, 8192, &info) == allowed); checks++;
        header[offset] = saved;
    }
    const uint32_t invalid[] = {0, 1, 47, UINT32_MAX, ORB_ASSETS_MAX_SIZE};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        write32(header + 16, invalid[i]);
        assert(!orb_assets_parse_header(header, 64, ORB_ASSETS_MAX_SIZE, 8192, &info)); checks++;
    }
    write32(header + 16, 48);
    write32(header + 24, 0);
    assert(!orb_assets_parse_header(header, 64, 8192, 8192, &info)); checks++;
    write32(header + 24, UINT32_MAX);
    assert(orb_assets_parse_header(header, 64, 8192, 8192, &info)); checks++;
    assert(!orb_assets_parse_header(header, 64, 8191, 8192, &info)); checks++;
    assert(!orb_assets_parse_header(header, 64, 8192, 8191, &info)); checks++;
    assert(!orb_assets_parse_header(header, 64, 8192, 8193, &info)); checks++;
    assert(!orb_assets_parse_header(NULL, 64, 8192, 8192, &info)); checks++;
    assert(!orb_assets_parse_header(header, 64, 8192, 8192, NULL)); checks++;
    write32(header + 8, 2); write32(header + 28, 2);
    assert(orb_assets_parse_header(header, 64, 8192, 8192, &info) && info.schema == 2); checks++;
    uint8_t sections[32] = {0};
    memcpy(sections, "ORBBNDL2", 8);
    write32(sections + 8, 2); write32(sections + 12, 32); write32(sections + 16, 48);
    write32(sections + 20, 80); write32(sections + 24, 24); write32(sections + 28, 104);
    uint32_t fo, fs, so, ss;
    assert(orb_assets_parse_sections(sections, 32, 104, &fo, &fs, &so, &ss)); checks++;
    assert(fo == 32 && fs == 48 && so == 80 && ss == 24); checks++;
    for (unsigned offset = 0; offset < 32; offset++) {
        uint8_t saved = sections[offset]; sections[offset] ^= 0x80;
        assert(!orb_assets_parse_sections(sections, 32, 104, &fo, &fs, &so, &ss)); checks++;
        sections[offset] = saved;
    }
    write32(sections + 20, 76); /* no overlapping sections */
    assert(!orb_assets_parse_sections(sections, 32, 104, &fo, &fs, &so, &ss)); checks++;
    write32(sections + 20, 80); write32(sections + 16, UINT32_MAX);
    assert(!orb_assets_parse_sections(sections, 32, 104, &fo, &fs, &so, &ss)); checks++;
    printf("assets header/sections: %u checks passed\n", checks);
    return 0;
}
