/* Real pack loader with a raw record and deterministic flash/lease failures.
 * The decoder stub checks that only caller-owned pixels escape the lease. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts_private.h"

enum { RECORD = 2048, PREFIX = 92, BITMAP = RECORD - PREFIX };
static const orb_pack_entry_t s_entries[] = {{48, RECORD, RECORD}};
const orb_fonts_table_t orb_fonts_table = {
    .pack = s_entries, .pack_count = 1, .pack_size = 48 + RECORD
};
static uint8_t s_flash[48 + RECORD];
static size_t s_live, s_requests, s_fail_request, s_reads, s_fail_read;
static bool s_map_fail, s_mapped, s_locked;
static unsigned s_decodes, s_maps;
struct orb_fonts_bitmap { const uint8_t *data; size_t size; };
static orb_fonts_bitmap_t s_bitmap;
typedef union { max_align_t alignment; size_t bytes; } allocation_t;

void *orb_fonts_port_alloc(size_t bytes)
{
    s_requests++;
    if (s_requests == s_fail_request) return NULL;
    allocation_t *a = malloc(sizeof(*a) + bytes);
    assert(a);
    a->bytes = bytes;
    s_live += bytes;
    return a + 1;
}

void orb_fonts_port_free(void *p)
{
    if (!p) return;
    allocation_t *a = (allocation_t *)p - 1;
    assert(s_live >= a->bytes);
    s_live -= a->bytes;
    free(a);
}

bool orb_fonts_port_read(uint32_t offset, void *dst, size_t size)
{
    assert(offset <= sizeof(s_flash) && size <= sizeof(s_flash) - offset);
    s_reads++;
    if (s_reads == s_fail_read) return false;
    memcpy(dst, s_flash + offset, size);
    return true;
}

bool orb_fonts_port_inflate(void *dst, size_t dst_size, const void *src, size_t src_size)
{
    (void)dst; (void)dst_size; (void)src; (void)src_size;
    assert(!"raw records must never be inflated");
    return false;
}

orb_fonts_bitmap_t *orb_fonts_port_map(uint32_t offset, size_t size)
{
    assert(offset == 48 + PREFIX && size == BITMAP && !s_mapped);
    s_maps++;
    if (s_map_fail) return NULL;
    s_bitmap = (orb_fonts_bitmap_t){s_flash + offset, size};
    s_mapped = true;
    return &s_bitmap;
}

const uint8_t *orb_fonts_port_bitmap_acquire(orb_fonts_bitmap_t *bitmap)
{
    assert(bitmap == &s_bitmap && s_mapped && !s_locked);
    s_locked = true;
    return bitmap->data;
}

void orb_fonts_port_bitmap_release(void)
{
    assert(s_locked);
    s_locked = false;
}

void orb_fonts_port_unmap(orb_fonts_bitmap_t *bitmap)
{
    assert(bitmap == &s_bitmap && s_mapped && !s_locked);
    s_mapped = false;
}

void orb_fonts_port_bitmap_stats(size_t *mapped, size_t *copied)
{
    *mapped = s_mapped ? BITMAP : 0;
    *copied = 0;
}

static const lv_font_t s_ellipsis;
const lv_font_t *orb_fonts_ellipsis_fallback(void) { return &s_ellipsis; }
void *lv_malloc_zeroed(size_t size) { return calloc(1, size); }
void lv_free(void *p) { free(p); }

const void *lv_font_get_bitmap_fmt_txt(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *buffer)
{
    assert(s_locked == s_mapped && !glyph->req_raw_bitmap && buffer);
    const lv_font_fmt_txt_dsc_t *dsc = glyph->resolved_font->dsc;
    assert(glyph->gid.index == 1 && dsc->glyph_dsc[1].box_w == 2);
    const uint8_t *bitmap = dsc->glyph_bitmap;
    assert(bitmap && bitmap[0] == 0x12 && bitmap[1] == 0x34);
    memcpy(buffer->data, bitmap, 2);
    s_decodes++;
    return buffer;
}

bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                                  uint32_t letter, uint32_t next)
{
    (void)font; (void)out; (void)letter; (void)next;
    return false;
}

static void wr16(uint8_t *p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
static void wr32(uint8_t *p, uint32_t n) { wr16(p, n); wr16(p + 2, n >> 16); }

static void fixture(void)
{
    memset(s_flash, 0, sizeof(s_flash));
    memcpy(s_flash, "ORBFONTS", 8);
    wr32(s_flash + 8, 1);
    wr32(s_flash + 12, 1);
    wr32(s_flash + 32, sizeof(s_flash));
    uint8_t *r = s_flash + 48;
    memcpy(r, "OFR1", 4);
    wr16(r + 4, 2);
    wr16(r + 6, 1);
    wr16(r + 8, 16);
    r[14] = 4;
    wr16(r + 20, UINT16_MAX);
    wr32(r + 24, 48);
    wr32(r + 28, 72);
    wr32(r + 36, PREFIX);
    wr32(r + 40, BITMAP);
    wr16(r + 64, 32); /* second glyph advance: 2 px */
    r[66] = r[67] = 2;
    wr32(r + 72, 'A');
    wr16(r + 76, 1);
    wr16(r + 78, 1);
    r[90] = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY;
    r[PREFIX] = 0x12;
    r[PREFIX + 1] = 0x34;
}

static void release(size_t base)
{
    orb_font_set_t unused;
    memset(&unused, 0xff, sizeof(unused));
    orb_fonts_pack_release_unused(&unused);
    orb_fonts_load_stats_t stats;
    orb_fonts_load_stats(&stats);
    assert(s_live == base && !s_mapped && !s_locked);
    assert(stats.resident_bytes == 0 && stats.mapped_bytes == 0);
    s_fail_request = s_fail_read = 0;
    s_map_fail = false;
}

static void decode(const lv_font_t *font)
{
    assert(font && font->line_height == 16 && font->fallback == &s_ellipsis);
    assert(!font->static_bitmap);
    uint8_t pixels[16] = {0};
    lv_draw_buf_t buffer = {.data = pixels};
    lv_font_glyph_dsc_t glyph = {.resolved_font = font};
    glyph.gid.index = 1;
    assert(font->get_glyph_bitmap(&glyph, &buffer) == &buffer);
    assert(pixels[0] == 0x12 && pixels[1] == 0x34 && !s_locked);
    assert(glyph.resolved_font == font); /* no local descriptor escapes */
    if (s_mapped) {
        unsigned decodes = s_decodes;
        glyph.req_raw_bitmap = 1;
        assert(!font->get_glyph_bitmap(&glyph, NULL));
        assert(s_decodes == decodes && !s_locked);
    }
}

int main(void)
{
    fixture();
    assert(orb_fonts_pack_open() == ORB_FONTS_OK);
    size_t base = s_live;
    const lv_font_t *font = orb_fonts_get(0);
    assert(s_mapped);
    decode(font);
    orb_fonts_load_stats_t mapped;
    orb_fonts_load_stats(&mapped);
    assert(mapped.mapped_bytes == BITMAP && mapped.bitmap_copy_bytes == 0);
    assert(mapped.resident_bytes == s_live - base && mapped.resident_bytes < RECORD);
    size_t requests = s_requests, reads = s_reads;
    unsigned maps = s_maps;
    assert(orb_fonts_get(0) == font);
    decode(font);
    assert(s_requests == requests && s_reads == reads && s_maps == maps);
    release(base);

    s_map_fail = true;
    font = orb_fonts_get(0);
    assert(!s_mapped);
    decode(font); /* mapping unavailable: retain all data, preserve exact glyphs */
    orb_fonts_load_stats_t copied;
    orb_fonts_load_stats(&copied);
    assert(!copied.mapped_bytes && copied.resident_bytes == mapped.resident_bytes + BITMAP);
    release(base);

    for (size_t i = 1; i <= 2; i++) {
        s_fail_request = s_requests + i;
        assert(!orb_fonts_get(0));
        release(base); /* both prefix and parsed-metadata OOM retire the lease */
        decode(orb_fonts_get(0));
        release(base);
        s_fail_read = s_reads + i;
        assert(!orb_fonts_get(0));
        release(base); /* header and prefix read failures do not leak mappings */
    }
    s_flash[48] = 'X';
    assert(!orb_fonts_get(0));
    release(base);
    fixture();
    wr32(s_flash + 48 + 24, PREFIX); /* metadata may not point into mapped bitmap */
    assert(!orb_fonts_get(0));
    release(base);
    fixture();
    orb_fonts_pack_cache_only();
    requests = s_requests; reads = s_reads; maps = s_maps;
    assert(!orb_fonts_get(0));
    assert(s_requests == requests && s_reads == reads && s_maps == maps);
    orb_font_set_t set = {0};
    assert(orb_fonts_pack_preload(&set) == ORB_FONTS_OK);
    decode(orb_fonts_get(0));
    release(base);
    puts("mapped pack: metadata-only residency, guarded decode, failures, retirement and cache-only hits passed");
    return 0;
}
