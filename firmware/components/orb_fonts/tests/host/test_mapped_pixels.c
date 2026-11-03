/* Compare every raw font in the shipped pack through the actual LVGL
 * decoder, once from PSRAM and once through the guarded flash bitmap path. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts_private.h"

static orb_pack_entry_t s_entry;
const orb_fonts_table_t orb_fonts_table = {
    .pack = &s_entry, .pack_count = 1, .pack_size = 8 * 1024 * 1024
};
struct orb_fonts_bitmap { const uint8_t *data; size_t bytes; };
static orb_fonts_bitmap_t s_bitmap;
static uint8_t *s_record;
static bool s_mapped;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t s_acquires;

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static void wr32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; i++) p[i] = n >> (i * 8);
}

bool orb_fonts_port_read(uint32_t offset, void *dst, size_t size)
{
    if (!offset) {
        assert(size == 48);
        memset(dst, 0, size);
        uint8_t *header = dst;
        memcpy(header, "ORBFONTS", 8);
        wr32(header + 8, 1);
        wr32(header + 12, 1);
        wr32(header + 32, orb_fonts_table.pack_size);
        return true;
    }
    assert(offset >= 48 && offset - 48 <= s_entry.size && size <= s_entry.size - (offset - 48));
    memcpy(dst, s_record + offset - 48, size);
    return true;
}

bool orb_fonts_port_inflate(void *dst, size_t dst_size, const void *src, size_t src_size)
{
    (void)dst; (void)dst_size; (void)src; (void)src_size;
    assert(!"raw records must never be inflated");
    return false;
}
void *orb_fonts_port_alloc(size_t size) { return malloc(size); }
void orb_fonts_port_free(void *pointer) { free(pointer); }
const lv_font_t *orb_fonts_ellipsis_fallback(void) { return NULL; }
const char *orb_fonts_err_name(orb_fonts_err_t err) { (void)err; return "test error"; }

orb_fonts_bitmap_t *orb_fonts_port_map(uint32_t offset, size_t size)
{
    assert(!s_mapped && offset >= 48 && offset - 48 == rd32(s_record + 36));
    assert(size == rd32(s_record + 40));
    s_bitmap = (orb_fonts_bitmap_t){s_record + offset - 48, size};
    s_mapped = true;
    return &s_bitmap;
}
const uint8_t *orb_fonts_port_bitmap_acquire(orb_fonts_bitmap_t *bitmap)
{
    assert(bitmap == &s_bitmap && s_mapped);
    assert(pthread_mutex_lock(&s_lock) == 0);
    s_acquires++;
    return bitmap->data;
}
void orb_fonts_port_bitmap_release(void) { assert(pthread_mutex_unlock(&s_lock) == 0); }
void orb_fonts_port_unmap(orb_fonts_bitmap_t *bitmap)
{
    assert(bitmap == &s_bitmap && s_mapped);
    s_mapped = false;
}
void orb_fonts_port_bitmap_stats(size_t *mapped, size_t *copied)
{
    *mapped = s_mapped ? s_bitmap.bytes : 0;
    *copied = 0;
}

typedef struct {
    const lv_font_t *reference;
    const lv_font_t *mapped;
    size_t glyphs;
} comparison_t;

static void compare_metrics(const lv_font_glyph_dsc_t *a, const lv_font_glyph_dsc_t *b)
{
    assert(a->adv_w == b->adv_w && a->box_w == b->box_w && a->box_h == b->box_h);
    assert(a->ofs_x == b->ofs_x && a->ofs_y == b->ofs_y && a->stride == b->stride);
    assert(a->format == b->format && a->gid.index == b->gid.index);
}

static void compare_glyph(comparison_t *comparison, uint32_t codepoint)
{
    lv_font_glyph_dsc_t a = {0}, b = {0};
    bool found = lv_font_get_glyph_dsc_fmt_txt(comparison->reference, &a, codepoint, codepoint);
    assert(found == lv_font_get_glyph_dsc_fmt_txt(comparison->mapped, &b, codepoint, codepoint));
    if (!found) return;
    compare_metrics(&a, &b);
    comparison->glyphs++;
    if (!a.box_w || !a.box_h) return;
    a.resolved_font = comparison->reference;
    b.resolved_font = comparison->mapped;
    lv_draw_buf_t *left = lv_draw_buf_create(a.box_w, a.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    lv_draw_buf_t *right = lv_draw_buf_create(a.box_w, a.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    assert(left && right);
    memset(left->data, 0, left->data_size);
    memset(right->data, 0, right->data_size);
    assert(comparison->reference->get_glyph_bitmap(&a, left) == left);
    assert(comparison->mapped->get_glyph_bitmap(&b, right) == right);
    assert(left->data_size == right->data_size && !memcmp(left->data, right->data, left->data_size));
    assert(a.resolved_font == comparison->reference && b.resolved_font == comparison->mapped);
    lv_draw_buf_destroy(left);
    lv_draw_buf_destroy(right);
}

static void *compare_fonts(void *context)
{
    comparison_t *comparison = context;
    const lv_font_fmt_txt_dsc_t *dsc = comparison->reference->dsc;
    for (unsigned i = 0; i < dsc->cmap_num; i++) {
        const lv_font_fmt_txt_cmap_t *map = &dsc->cmaps[i];
        for (uint32_t cp = map->range_start; cp < map->range_start + map->range_length; cp++) {
            compare_glyph(comparison, cp);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length >= 48 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *pack = malloc((size_t)length);
    assert(pack && fread(pack, 1, length, file) == (size_t)length);
    fclose(file);
    assert(!memcmp(pack, "ORBFONTS", 8));
    uint32_t count = rd32(pack + 12), directory = rd32(pack + 36);
    assert(directory <= length && count <= ((size_t)length - directory) / 16);
    lv_init();
    size_t fonts = 0, glyphs = 0, bitmap_bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = pack + directory + i * 16;
        uint32_t offset = rd32(entry), stored = rd32(entry + 4), size = rd32(entry + 8);
        if (stored != size) continue;
        assert(offset <= length && size <= (size_t)length - offset && size >= 48);
        s_record = pack + offset;
        assert(!memcmp(s_record, "OFR1", 4) && s_record[15] <= 2);
        /* This one-record fixture compares only each font's own glyphs. Its
         * normal text/icon fallback is exercised by the complete-set tests. */
        s_record[20] = s_record[21] = 0xff;
        s_entry = (orb_pack_entry_t){48, size, size};
        lv_font_t *reference = orb_fonts_record_font(s_record, size, NULL);
        const lv_font_t *mapped = orb_fonts_get(0);
        assert(reference && mapped && s_mapped && !mapped->static_bitmap);
        assert(reference->line_height == mapped->line_height && reference->base_line == mapped->base_line);
        comparison_t comparisons[2] = {{reference, mapped, 0}, {reference, mapped, 0}};
        pthread_t workers[2];
        for (unsigned n = 0; n < 2; n++) assert(pthread_create(&workers[n], NULL, compare_fonts, &comparisons[n]) == 0);
        for (unsigned n = 0; n < 2; n++) assert(pthread_join(workers[n], NULL) == 0);
        assert(comparisons[0].glyphs == comparisons[1].glyphs);
        glyphs += comparisons[0].glyphs;
        bitmap_bytes += s_bitmap.bytes;
        fonts++;
        orb_fonts_record_font_free(reference);
        orb_font_set_t unused;
        memset(&unused, 0xff, sizeof(unused));
        orb_fonts_pack_release_unused(&unused);
        assert(!s_mapped);
    }
    assert(fonts == 123 && bitmap_bytes > 3700000 && s_acquires > glyphs);
    printf("mapped LVGL parity: %zu fonts, %zu glyphs, %zu bitmap bytes, two decoder threads passed\n",
           fonts, glyphs, bitmap_bytes);
    free(pack);
    lv_deinit();
    return 0;
}
