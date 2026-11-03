/* Exercise the real pack loader with a capped allocator and a three-font
 * fallback chain. Only platform I/O and LVGL allocation/render hooks are stubbed. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts_private.h"

enum { STORED = 2048, RECORD = 4096, COUNT = 3, CAP = 3 * RECORD + STORED };
static const orb_pack_entry_t entries[] = {
    {48, STORED, RECORD}, {48 + STORED, STORED, RECORD}, {48 + 2 * STORED, STORED, RECORD}
};
const orb_fonts_table_t orb_fonts_table = {
    .pack = entries, .pack_count = COUNT, .pack_size = 48 + COUNT * STORED
};

static size_t live, peak, budget = SIZE_MAX, requests, fail_request, base_live;
static int fail_read = -1, fail_inflate = -1, malformed = -1;
typedef union { max_align_t alignment; size_t bytes; } allocation_t;
enum { ARENA_BYTES = 24 * 1024, ARENA_BLOCKS = 32, PROBE_BYTES = 11 * 1024 };
static union { max_align_t alignment; uint8_t bytes[ARENA_BYTES]; } s_arena;
static struct { size_t offset, size; bool used; } s_blocks[ARENA_BLOCKS];
static bool s_use_arena;

/* Deterministic first-fit heap: freed adjacent ranges naturally coalesce when
 * the next allocation scans the live ranges. Total-free-only caps cannot catch
 * short-lived input stranding a hole before a retained font record. */
static allocation_t *arena_alloc(size_t bytes)
{
    size_t alignment = _Alignof(max_align_t);
    size_t needed = (sizeof(allocation_t) + bytes + alignment - 1) / alignment * alignment;
    size_t start = 0;
    for (;;) {
        size_t next = ARENA_BYTES, next_size = 0;
        for (unsigned i = 0; i < ARENA_BLOCKS; i++) {
            if (s_blocks[i].used && s_blocks[i].offset >= start && s_blocks[i].offset < next) {
                next = s_blocks[i].offset;
                next_size = s_blocks[i].size;
            }
        }
        if (needed <= next - start) {
            unsigned slot = 0;
            while (slot < ARENA_BLOCKS && s_blocks[slot].used) slot++;
            assert(slot < ARENA_BLOCKS);
            s_blocks[slot].offset = start;
            s_blocks[slot].size = needed;
            s_blocks[slot].used = true;
            return (allocation_t *)(s_arena.bytes + start);
        }
        if (next == ARENA_BYTES) return NULL;
        start = next + next_size;
    }
}

static size_t arena_largest(void)
{
    size_t start = 0, largest = 0;
    for (;;) {
        size_t next = ARENA_BYTES, next_size = 0;
        for (unsigned i = 0; i < ARENA_BLOCKS; i++) {
            if (s_blocks[i].used && s_blocks[i].offset >= start && s_blocks[i].offset < next) {
                next = s_blocks[i].offset;
                next_size = s_blocks[i].size;
            }
        }
        if (next - start > largest) largest = next - start;
        if (next == ARENA_BYTES) return largest;
        start = next + next_size;
    }
}

void *orb_fonts_port_alloc(size_t bytes)
{
    requests++;
    if (requests == fail_request || bytes > budget - live) return NULL;
    allocation_t *a = s_use_arena ? arena_alloc(bytes) : malloc(sizeof(*a) + bytes);
    if (!a) return NULL;
    a->bytes = bytes;
    live += bytes;
    if (live > peak) peak = live;
    return a + 1;
}

void orb_fonts_port_free(void *p)
{
    if (!p) return;
    allocation_t *a = (allocation_t *)p - 1;
    assert(live >= a->bytes);
    live -= a->bytes;
    if (s_use_arena) {
        size_t offset = (uint8_t *)a - s_arena.bytes;
        unsigned slot = 0;
        while (slot < ARENA_BLOCKS && (!s_blocks[slot].used || s_blocks[slot].offset != offset)) slot++;
        assert(slot < ARENA_BLOCKS);
        s_blocks[slot].used = false;
    } else {
        free(a);
    }
}

static void wr16(uint8_t *p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
static void wr32(uint8_t *p, uint32_t n) { wr16(p, n); wr16(p + 2, n >> 16); }

bool orb_fonts_port_read(uint32_t offset, void *dst, size_t size)
{
    memset(dst, 0, size);
    uint8_t *p = dst;
    if (offset == 0) {
        assert(size == 48);
        memcpy(p, "ORBFONTS", 8);
        wr32(p + 8, 1);
        wr32(p + 12, COUNT);
        wr32(p + 32, orb_fonts_table.pack_size);
        return true;
    }
    assert(offset >= 48 && size == STORED);
    int id = (offset - 48) / STORED;
    assert(id >= 0 && id < COUNT);
    p[0] = id;
    return id != fail_read;
}

bool orb_fonts_port_inflate(void *dst, size_t dst_size, const void *src, size_t src_size)
{
    assert(dst_size == RECORD && src_size == STORED);
    int id = *(const uint8_t *)src;
    if (id == fail_inflate) return false;
    uint8_t *p = dst;
    memset(p, 0, dst_size);
    memcpy(p, id == malformed ? "BAD!" : "OFR1", 4);
    wr16(p + 4, 1);       /* one empty glyph */
    wr16(p + 8, 16);      /* line height */
    p[14] = 4;           /* bpp */
    wr16(p + 20, id + 1 < COUNT ? id + 1 : UINT16_MAX);
    wr32(p + 24, 48);     /* glyph at 48, zero cmaps/kern/bitmap */
    return true;
}

void *lv_malloc_zeroed(size_t size) { return calloc(1, size); }
void lv_free(void *p) { free(p); }
static const lv_font_t s_ellipsis;
const lv_font_t *orb_fonts_ellipsis_fallback(void) { return &s_ellipsis; }
const void *lv_font_get_bitmap_fmt_txt(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *buf)
{
    (void)dsc; (void)buf;
    return NULL;
}
bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                                  uint32_t letter, uint32_t next)
{
    (void)font; (void)out; (void)letter; (void)next;
    return false;
}

static void release(void)
{
    orb_font_set_t unused;
    memset(&unused, 0xff, sizeof(unused));
    orb_fonts_pack_release_unused(&unused);
    assert(live == base_live);
    orb_fonts_load_stats_t stats;
    orb_fonts_load_stats(&stats);
    assert(stats.resident_bytes == 0);
    requests = peak = fail_request = 0;
    budget = SIZE_MAX;
    fail_read = fail_inflate = malformed = -1;
}

static void assert_chain(const lv_font_t *font)
{
    for (int i = 0; i < COUNT; i++) {
        assert(font);
        assert(font->line_height == 16);
        font = font->fallback;
    }
    assert(font == &s_ellipsis && !font->fallback);
}

int main(void)
{
    assert(orb_fonts_pack_open() == ORB_FONTS_OK);
    base_live = live;
    assert_chain(orb_fonts_get(0));
    printf("unconstrained peak: %zu bytes; retained: %zu bytes\n", peak, live);
    orb_fonts_load_stats_t first;
    orb_fonts_load_stats(&first);
    assert(first.loads == COUNT && first.resident_bytes == live - base_live);
    assert(first.resident_bytes > COUNT * RECORD);
    release();

    budget = CAP + base_live;
    assert_chain(orb_fonts_get(0));
    assert(peak == CAP + base_live);
    orb_fonts_load_stats_t warmed;
    orb_fonts_load_stats(&warmed);
    assert(warmed.resident_bytes == first.resident_bytes);
    size_t cached_requests = requests;
    assert_chain(orb_fonts_get(0));
    assert(requests == cached_requests);
    orb_fonts_load_stats_t cached;
    orb_fonts_load_stats(&cached);
    assert(cached.loads == warmed.loads && cached.elapsed_us == warmed.elapsed_us);
    release();

    for (size_t i = 1; i <= 9; i++) {
        fail_request = i;
        (void)orb_fonts_get(0);
        release();
        assert_chain(orb_fonts_get(0));
        release();
    }
    for (int i = 0; i < COUNT; i++) {
        fail_read = i;
        assert(!orb_fonts_get(0)); /* no immutable parent may omit a required fallback */
        fail_read = -1;
        assert_chain(orb_fonts_get(0)); /* retry must build a complete chain */
        release();
        fail_inflate = i;
        (void)orb_fonts_get(0);
        release();
        malformed = i;
        (void)orb_fonts_get(0);
        release();
        assert_chain(orb_fonts_get(0));
        release();
    }
    assert(!orb_fonts_get(COUNT));
    orb_font_set_t selected;
    memset(&selected, 0, sizeof(selected));
    assert(orb_fonts_pack_preload(&selected) == ORB_FONTS_OK);
    assert_chain(orb_fonts_get(0));
    size_t preload_requests = requests;
    assert(orb_fonts_pack_preload(&selected) == ORB_FONTS_OK);
    assert(requests == preload_requests);
    release();
    s_use_arena = true;
    assert(arena_largest() == ARENA_BYTES);
    assert_chain(orb_fonts_get(0));
    size_t contiguous = arena_largest();
    size_t available = ARENA_BYTES;
    for (unsigned i = 0; i < ARENA_BLOCKS; i++) {
        if (s_blocks[i].used) available -= s_blocks[i].size;
    }
    assert(available >= PROBE_BYTES + sizeof(allocation_t));
    void *probe = orb_fonts_port_alloc(PROBE_BYTES);
    assert(probe); /* enough total free is insufficient if a font pins the input hole */
    memset(probe, 0xa5, PROBE_BYTES);
    assert_chain(orb_fonts_get(0));
    orb_fonts_port_free(probe);
    release();
    assert(arena_largest() == ARENA_BYTES);
    s_use_arena = false;
    printf("record-first arena: %zu contiguous bytes; %u-byte transient fits\n", contiguous, PROBE_BYTES);
    orb_fonts_pack_cache_only();
    assert(!orb_fonts_get(0));
    assert(requests == 0); /* cache-only UI lookups never touch flash or allocate */
    assert(orb_fonts_pack_preload(&selected) == ORB_FONTS_OK);
    assert_chain(orb_fonts_get(0));
    release();
    puts("pack memory: capped fallback chain, cache, allocation/read/inflate/parse failures and retries passed");
    return 0;
}
