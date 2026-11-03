/*
 * The font pack: fonts for the Larger and AX3 content sizes and for Bold Text, in the
 * assets partition. Records are zlib streams or raw records with flash-backed
 * bitmaps (tools/fonts/pack.py has the layout). Compressed records are read with
 * the flash driver and inflated into PSRAM with their Adler-32 checked. Raw
 * records retain only metadata in PSRAM; a lease protects flash bitmap decodes
 * against single-slot OTA writes. LVGL's
 * font structures are pointed into it. Firmware prepares complete sets before
 * publishing them; old-only records retire before the next set loads. Fonts of
 * a set that is no longer selected are freed behind the draw barrier. Public UI
 * selectors expose lifetime-stable handles, never these payload descriptors.
 */

#include <string.h>
#include <stdatomic.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#else
#include <time.h>
#endif

#include "orb_fonts_private.h"

#define PACK_HEADER_SIZE 48
#define RECORD_HEADER_SIZE 48
#define GLYPH_SIZE 12
#define CMAP_SIZE 20

typedef struct {
    lv_font_t font;
    lv_font_fmt_txt_dsc_t dsc;
    union {
        lv_font_fmt_txt_kern_classes_t classes;
        lv_font_fmt_txt_kern_pair_t pairs;
    } kern;
    size_t metadata_bytes;
    size_t record_bytes;
    uint8_t *record; /* the inflated record when loaded from the pack, else NULL */
    orb_fonts_bitmap_t *bitmap;
    lv_font_fmt_txt_cmap_t *cmaps;
    lv_font_fmt_txt_glyph_dsc_t *glyphs;
} record_font_t;

#ifdef ESP_PLATFORM
/* tools/fonts/budget.py uses the board ABI, not the host's wider pointers. */
_Static_assert(sizeof(record_font_t) == 108 && sizeof(lv_font_fmt_txt_cmap_t) == 24 &&
               sizeof(lv_font_fmt_txt_glyph_dsc_t) == 8, "update the font PSRAM budget for this ABI");
#endif

static bool s_pack_checked;
static orb_fonts_err_t s_pack;
/* A failed boot preload can leave a current-set slot empty. The background
 * loader may later fill that shared ID while UI cache-only lookups read it.
 * Publish a fully parsed immutable record with release/acquire even on that
 * recovery path. Retirement happens only after worker completion on the UI. */
static _Atomic(record_font_t *) *s_loaded; /* [pack_count] */

static record_font_t *loaded(uint16_t index)
{
    return atomic_load_explicit(&s_loaded[index], memory_order_acquire);
}

static void publish(uint16_t index, record_font_t *font)
{
    atomic_store_explicit(&s_loaded[index], font, memory_order_release);
}
static bool s_cache_only;
static orb_fonts_load_stats_t s_stats;
#ifdef ESP_PLATFORM
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
#define STATS_LOCK() portENTER_CRITICAL(&s_stats_lock)
#define STATS_UNLOCK() portEXIT_CRITICAL(&s_stats_lock)
#else
#define STATS_LOCK() ((void)0)
#define STATS_UNLOCK() ((void)0)
#endif

static uint64_t now_us(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
#endif
}

void orb_fonts_load_stats(orb_fonts_load_stats_t *out)
{
    if (!out) return;
    STATS_LOCK();
    *out = s_stats;
    STATS_UNLOCK();
#if defined(ESP_PLATFORM) || defined(ORB_FONTS_TEST_MAPPING)
    orb_fonts_port_bitmap_stats(&out->mapped_bytes, &out->bitmap_copy_bytes);
    out->resident_bytes += out->bitmap_copy_bytes;
#endif
}

void orb_fonts_pack_cache_only(void)
{
    s_cache_only = true;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool in_record(size_t size, uint32_t offset, size_t len, size_t align)
{
    return offset % align == 0 && offset <= size && len <= size - offset;
}

/* Decode before wiring maps so rejected glyphs use the caller's one cleanup. */
static bool record_glyphs(record_font_t *f, const uint8_t *rec, uint32_t glyph_off,
                          uint16_t glyph_count, uint32_t bitmap_size)
{
    for (uint32_t i = 0; i < glyph_count; i++) {
        const uint8_t *g = rec + glyph_off + i * GLYPH_SIZE;
        uint32_t bitmap_index = rd32(g);
        uint16_t adv_w = rd16(g + 4);
        if (bitmap_index >= (1u << 20) || adv_w >= (1u << 12) || bitmap_index > bitmap_size) {
            return false;
        }
        /* Read the complete wire entry before an in-place conversion writes
         * its shorter native entry over these bytes. No published face mutates. */
        lv_font_fmt_txt_glyph_dsc_t glyph = {
            .bitmap_index = bitmap_index, .adv_w = adv_w,
            .box_w = g[6], .box_h = g[7], .ofs_x = (int8_t)g[8], .ofs_y = (int8_t)g[9],
        };
        f->glyphs[i] = glyph;
    }
    return true;
}

static lv_font_t *record_font(const uint8_t *rec, size_t size, size_t metadata_size,
                              const lv_font_t *fallback, bool owned)
{
    if (size < RECORD_HEADER_SIZE || memcmp(rec, "OFR1", 4) != 0) {
        return NULL;
    }
    uint16_t glyph_count = rd16(rec + 4), cmap_num = rd16(rec + 6);
    uint8_t bpp = rec[14], bitmap_format = rec[15], kern_type = rec[16], stride = rec[17];
    uint32_t glyph_off = rd32(rec + 24), cmap_off = rd32(rec + 28), kern_off = rd32(rec + 32);
    uint32_t bitmap_off = rd32(rec + 36), bitmap_size = rd32(rec + 40);
    if (!glyph_count || !in_record(metadata_size, glyph_off, (size_t)glyph_count * GLYPH_SIZE, 4) ||
        !in_record(metadata_size, cmap_off, (size_t)cmap_num * CMAP_SIZE, 4) || !in_record(size, bitmap_off, bitmap_size, 4) ||
        bpp != 4 || bitmap_format > 2 || kern_type > 2) {
        return NULL;
    }

    size_t bytes = sizeof(record_font_t) + cmap_num * sizeof(lv_font_fmt_txt_cmap_t) +
                   (owned ? 0 : glyph_count * sizeof(lv_font_fmt_txt_glyph_dsc_t));
    record_font_t *f = orb_fonts_port_alloc(bytes);
    if (!f) {
        return NULL;
    }
    memset(f, 0, bytes);
    f->metadata_bytes = bytes;
    f->cmaps = (lv_font_fmt_txt_cmap_t *)(f + 1);
    /* The owned wire array has 12 bytes per glyph; LVGL needs only 8. Reuse
     * that storage instead of retaining both representations for two full sets.
     * The public const-record parser keeps its independent converted array. */
    _Static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t) <= GLYPH_SIZE, "native glyph fits wire storage");
    f->glyphs = owned ? (lv_font_fmt_txt_glyph_dsc_t *)(rec + glyph_off) :
        (lv_font_fmt_txt_glyph_dsc_t *)(f->cmaps + cmap_num);

    if (!record_glyphs(f, rec, glyph_off, glyph_count, bitmap_size)) goto bad;
    for (uint32_t i = 0; i < cmap_num; i++) {
        const uint8_t *c = rec + cmap_off + i * CMAP_SIZE;
        lv_font_fmt_txt_cmap_t *m = &f->cmaps[i];
        uint32_t unicode_list = rd32(c + 8), ofs_list = rd32(c + 12);
        uint16_t list_length = rd16(c + 16);
        uint8_t type = c[18], ofs_size = c[19];
        m->range_start = rd32(c);
        m->range_length = rd16(c + 4);
        m->glyph_id_start = rd16(c + 6);
        m->list_length = list_length;
        m->type = (lv_font_fmt_txt_cmap_type_t)type;
        if (type > LV_FONT_FMT_TXT_CMAP_SPARSE_TINY || (unicode_list && !in_record(metadata_size, unicode_list, list_length * 2u, 2)) ||
            (ofs_list && !in_record(metadata_size, ofs_list, (size_t)list_length * ofs_size, ofs_size ? ofs_size : 1))) {
            goto bad;
        }
        m->unicode_list = unicode_list ? (const uint16_t *)(rec + unicode_list) : NULL;
        m->glyph_id_ofs_list = ofs_list ? rec + ofs_list : NULL;
    }
    if (kern_type == 1) {
        if (!in_record(metadata_size, kern_off, 16, 4)) {
            goto bad;
        }
        const uint8_t *k = rec + kern_off;
        uint8_t left_cnt = k[0], right_cnt = k[1];
        uint32_t left = rd32(k + 4), right = rd32(k + 8), values = rd32(k + 12);
        if (!in_record(metadata_size, left, glyph_count, 1) || !in_record(metadata_size, right, glyph_count, 1) ||
            !in_record(metadata_size, values, (size_t)left_cnt * right_cnt, 1)) {
            goto bad;
        }
        f->kern.classes.left_class_cnt = left_cnt;
        f->kern.classes.right_class_cnt = right_cnt;
        f->kern.classes.left_class_mapping = rec + left;
        f->kern.classes.right_class_mapping = rec + right;
        f->kern.classes.class_pair_values = (const int8_t *)(rec + values);
        f->dsc.kern_dsc = &f->kern.classes;
        f->dsc.kern_classes = 1;
    } else if (kern_type == 2) {
        if (!in_record(metadata_size, kern_off, 16, 4)) {
            goto bad;
        }
        const uint8_t *k = rec + kern_off;
        uint32_t pair_cnt = rd32(k), ids = rd32(k + 8), values = rd32(k + 12);
        uint8_t ids_size = k[4];
        if (ids_size > 1 || !in_record(metadata_size, ids, (size_t)pair_cnt * 2u * (ids_size + 1u), ids_size + 1u) ||
            !in_record(metadata_size, values, pair_cnt, 1)) {
            goto bad;
        }
        f->kern.pairs.pair_cnt = pair_cnt;
        f->kern.pairs.glyph_ids_size = ids_size;
        f->kern.pairs.glyph_ids = rec + ids;
        f->kern.pairs.values = (const int8_t *)(rec + values);
        f->dsc.kern_dsc = &f->kern.pairs;
        f->dsc.kern_classes = 0;
    }

    f->dsc.glyph_bitmap = metadata_size == size ? rec + bitmap_off : NULL;
    f->dsc.glyph_dsc = f->glyphs;
    f->dsc.cmaps = f->cmaps;
    f->dsc.kern_scale = rd16(rec + 18);
    f->dsc.cmap_num = cmap_num;
    f->dsc.bpp = bpp;
    f->dsc.bitmap_format = bitmap_format;
    f->dsc.stride = stride;

    f->font.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    f->font.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    f->font.line_height = rd16(rec + 8);
    f->font.base_line = (int16_t)rd16(rec + 10);
    f->font.underline_position = (int8_t)rec[12];
    f->font.underline_thickness = (int8_t)rec[13];
    f->font.subpx = LV_FONT_SUBPX_NONE;
    f->font.dsc = &f->dsc;
    f->font.fallback = fallback;
    return &f->font;

bad:
    orb_fonts_port_free(f);
    return NULL;
}

lv_font_t *orb_fonts_record_font(const uint8_t *rec, size_t size, const lv_font_t *fallback)
{
    return record_font(rec, size, size, fallback, false);
}

void orb_fonts_record_font_free(lv_font_t *font)
{
    if (font) {
        orb_fonts_port_free((record_font_t *)font); /* `font` is the first member */
    }
}

orb_fonts_err_t orb_fonts_pack_open(void)
{
    if (s_pack_checked) {
        return s_pack;
    }
    s_pack_checked = true;
    uint8_t h[PACK_HEADER_SIZE];
    if (!orb_fonts_port_read(0, h, sizeof(h)) || memcmp(h, "ORBFONTS", 8) != 0) {
        s_pack = ORB_FONTS_ERR_NO_PACK;
    } else if (rd32(h + 8) != 1 || rd32(h + 12) != orb_fonts_table.pack_count ||
               memcmp(h + 16, orb_fonts_table.pack_id, 16) != 0 || rd32(h + 32) != orb_fonts_table.pack_size) {
        s_pack = ORB_FONTS_ERR_PACK_MISMATCH;
    } else {
        size_t bytes = orb_fonts_table.pack_count * sizeof(*s_loaded);
        s_loaded = orb_fonts_port_alloc(bytes);
        if (s_loaded) {
            for (uint16_t i = 0; i < orb_fonts_table.pack_count; i++) atomic_init(&s_loaded[i], NULL);
        }
        s_pack = s_loaded || !orb_fonts_table.pack_count ? ORB_FONTS_OK : ORB_FONTS_ERR_NO_MEM;
        if (s_pack == ORB_FONTS_ERR_NO_MEM) s_pack_checked = false;
    }
    if (s_pack != ORB_FONTS_OK) {
        LV_LOG_WARN("orb_fonts: font pack: %s", orb_fonts_err_name(s_pack));
    }
    return s_pack;
}

/* The software renderer requests decoded A8 pixels (static_bitmap remains 0).
 * Decode into its existing draw buffer while the assets lease is locked; no
 * flash pointer escapes into a deferred draw task or across an OTA erase. */
#if defined(ESP_PLATFORM) || defined(ORB_FONTS_TEST_MAPPING)
static const void *mapped_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *buffer)
{
    if (glyph->req_raw_bitmap) return NULL;
    record_font_t *f = (record_font_t *)glyph->resolved_font;
    lv_font_fmt_txt_dsc_t dsc = f->dsc;
    dsc.glyph_bitmap = orb_fonts_port_bitmap_acquire(f->bitmap);
    lv_font_t font = f->font;
    font.dsc = &dsc;
    lv_font_glyph_dsc_t local = *glyph;
    local.resolved_font = &font;
    const void *pixels = lv_font_get_bitmap_fmt_txt(&local, buffer);
    orb_fonts_port_bitmap_release();
    return pixels;
}
#endif

static uint8_t *read_record(const orb_pack_entry_t *e, size_t *bytes, orb_fonts_bitmap_t **bitmap)
{
    *bytes = e->size;
    *bitmap = NULL;
    uint8_t *rec = NULL;
    if (e->stored == e->size) {
#if defined(ESP_PLATFORM) || defined(ORB_FONTS_TEST_MAPPING)
        uint8_t header[RECORD_HEADER_SIZE];
        if (e->size < sizeof(header) || !orb_fonts_port_read(e->offset, header, sizeof(header))) return NULL;
        uint32_t offset = rd32(header + 36), size = rd32(header + 40);
        if (offset < sizeof(header) || !in_record(e->size, offset, size, 4)) return NULL;
        *bitmap = orb_fonts_port_map(e->offset + offset, size);
        if (*bitmap) *bytes = offset;
#endif
        rec = orb_fonts_port_alloc(*bytes);
        if (rec && !orb_fonts_port_read(e->offset, rec, *bytes)) {
            orb_fonts_port_free(rec);
            rec = NULL;
        }
    } else {
        /* Place the long-lived record before short-lived compressed input, so
         * releasing input does not strand a hole ahead of every loaded face. */
        rec = orb_fonts_port_alloc(e->size);
#if defined(ESP_PLATFORM) || defined(ORB_FONTS_TEST_STREAMING)
        if (rec && !orb_fonts_port_read_inflate(rec, e->size, e->offset, e->stored)) {
            orb_fonts_port_free(rec);
            rec = NULL;
        }
#else
        uint8_t *stored = rec ? orb_fonts_port_alloc(e->stored) : NULL;
        if (rec && (!stored || !orb_fonts_port_read(e->offset, stored, e->stored) ||
                    !orb_fonts_port_inflate(rec, e->size, stored, e->stored))) {
            orb_fonts_port_free(rec);
            rec = NULL;
        }
        /* Release short-lived input before parsing/loading the fallback chain. */
        orb_fonts_port_free(stored);
#endif
    }
    return rec;
}

static record_font_t *load(uint16_t index, int depth)
{
    const orb_pack_entry_t *e = &orb_fonts_table.pack[index];
    size_t bytes;
    orb_fonts_bitmap_t *bitmap;
    uint8_t *rec = read_record(e, &bytes, &bitmap);
    lv_font_t *font = NULL;
    if (rec) {
        if (e->size < RECORD_HEADER_SIZE) goto failed;
        int16_t fallback_id = (int16_t)rd16(rec + 20);
        const lv_font_t *fallback = fallback_id < 0 ? orb_fonts_ellipsis_fallback() : NULL;
        if (fallback_id >= 0 && depth < 4) {
            if (fallback_id < orb_fonts_table.builtin_count) {
                fallback = orb_fonts_table.builtin[fallback_id];
            } else if (fallback_id - orb_fonts_table.builtin_count < orb_fonts_table.pack_count) {
                uint16_t fi = (uint16_t)(fallback_id - orb_fonts_table.builtin_count);
                record_font_t *next = loaded(fi);
                if (!next) {
                    next = load(fi, depth + 1);
                    publish(fi, next);
                }
                fallback = next ? &next->font : NULL;
            }
        }
        /* Published records are immutable. Caching a parent with a missing
         * required fallback would permanently drop icons/normal digits even
         * after a later preload successfully retried the fallback slot. */
        if (fallback_id >= 0 && !fallback) {
            LV_LOG_WARN("orb_fonts: pack font %u required fallback unavailable", index);
        } else {
            font = record_font(rec, e->size, bytes, fallback, true);
            if (!font) LV_LOG_WARN("orb_fonts: pack font %u is malformed", index);
        }
    }
failed:
    if (!font) {
        orb_fonts_port_free(rec);
#if defined(ESP_PLATFORM) || defined(ORB_FONTS_TEST_MAPPING)
        if (bitmap) orb_fonts_port_unmap(bitmap);
#endif
        STATS_LOCK();
        s_stats.failures++;
        STATS_UNLOCK();
        return NULL;
    }
    record_font_t *f = (record_font_t *)font;
    f->record = rec;
    f->record_bytes = bytes;
    f->bitmap = bitmap;
#if defined(ESP_PLATFORM) || defined(ORB_FONTS_TEST_MAPPING)
    if (bitmap) f->font.get_glyph_bitmap = mapped_bitmap;
#endif
    STATS_LOCK();
    s_stats.loads++;
    s_stats.resident_bytes += bytes + f->metadata_bytes;
    STATS_UNLOCK();
    return f;
}

static const lv_font_t *get(orb_font_id_t id, bool may_load)
{
    if (id < orb_fonts_table.builtin_count) {
        return orb_fonts_table.builtin[id];
    }
    uint32_t index = (uint32_t)id - orb_fonts_table.builtin_count;
    if (index >= orb_fonts_table.pack_count || orb_fonts_pack_open() != ORB_FONTS_OK) {
        return NULL;
    }
    record_font_t *font = loaded((uint16_t)index);
    if (!font && may_load) {
        uint64_t started = now_us();
        font = load((uint16_t)index, 0);
        publish((uint16_t)index, font);
        STATS_LOCK();
        s_stats.elapsed_us += now_us() - started;
        STATS_UNLOCK();
    }
    return font ? &font->font : NULL;
}

const lv_font_t *orb_fonts_get(orb_font_id_t id)
{
    return get(id, !s_cache_only);
}

const lv_font_t *orb_fonts_peek(orb_font_id_t id)
{
    if (id < orb_fonts_table.builtin_count) return orb_fonts_table.builtin[id];
    uint32_t index = (uint32_t)id - orb_fonts_table.builtin_count;
    if (!s_loaded || index >= orb_fonts_table.pack_count) return NULL;
    record_font_t *font = loaded((uint16_t)index);
    return font ? &font->font : NULL;
}

orb_fonts_err_t orb_fonts_pack_preload(const orb_font_set_t *set)
{
    orb_fonts_err_t err = orb_fonts_pack_open();
    if (err != ORB_FONTS_OK) return err;
    bool ok = true;
    for (int s = 0; s < ORB_TEXT_STYLE_COUNT; s++) {
        for (int v = 0; v < 2; v++) {
            const orb_face_ids_t *f = &set->styles[s].face[v];
            if (!get(f->text, true)) ok = false;
            if (!get(f->tabular, true)) ok = false;
            if (!get(f->icons, true)) ok = false;
        }
    }
    for (int c = 0; c < ORB_CLOCK_COUNT; c++) {
        if (!get(set->clocks[c].font, true)) ok = false;
    }
    return ok ? ORB_FONTS_OK : ORB_FONTS_ERR_LOAD;
}

static bool set_uses(const orb_font_set_t *set, orb_font_id_t id)
{
    if (!set) return false;
    for (int s = 0; s < ORB_TEXT_STYLE_COUNT; s++) {
        for (int v = 0; v < 2; v++) {
            const orb_face_ids_t *f = &set->styles[s].face[v];
            if (f->text == id || f->tabular == id || f->icons == id) {
                return true;
            }
        }
    }
    for (int c = 0; c < ORB_CLOCK_COUNT; c++) {
        if (set->clocks[c].font == id) {
            return true;
        }
    }
    return false;
}

static bool fallback_uses(const orb_font_set_t *set, const lv_font_t *font)
{
    /* Keep the actual published fallback closure, even if a future generated
     * set stops listing a fallback as a top-level face. Retirement must not
     * depend on that incidental property of today's pack. */
    for (uint16_t i = 0; i < orb_fonts_table.pack_count; i++) {
        record_font_t *root = loaded(i);
        if (!root || !set_uses(set, orb_fonts_table.builtin_count + i)) continue;
        for (const lv_font_t *next = root->font.fallback; next; next = next->fallback) {
            if (next == font) return true;
        }
    }
    return false;
}

void orb_fonts_pack_release_unused(const orb_font_set_t *set)
{
    if (!s_loaded) {
        return;
    }
    for (uint16_t i = 0; i < orb_fonts_table.pack_count; i++) {
        record_font_t *candidate = loaded(i);
        if (candidate && !set_uses(set, (orb_font_id_t)(orb_fonts_table.builtin_count + i)) &&
            !fallback_uses(set, &candidate->font)) {
            record_font_t *font = atomic_exchange_explicit(&s_loaded[i], NULL, memory_order_acq_rel);
            STATS_LOCK();
            s_stats.resident_bytes -= font->record_bytes + font->metadata_bytes;
            STATS_UNLOCK();
#if defined(ESP_PLATFORM) || defined(ORB_FONTS_TEST_MAPPING)
            if (font->bitmap) orb_fonts_port_unmap(font->bitmap);
#endif
            uint8_t *rec = font->record;
            orb_fonts_record_font_free(&font->font);
            orb_fonts_port_free(rec);
        }
    }
}
