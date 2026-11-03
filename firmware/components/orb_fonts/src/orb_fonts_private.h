#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "orb_fonts.h"

/*
 * The generated table (src/orb_fonts_table.c, from tools/fonts/build.py) names every font
 * by an id: ids below builtin_count are C fonts in the app image, the rest are records in
 * the font pack (tools/fonts/pack.py has its layout), loaded by orb_fonts_pack.c.
 */
typedef uint16_t orb_font_id_t;

typedef struct {
    orb_font_id_t text, tabular, icons;
} orb_face_ids_t;

typedef struct {
    uint8_t pt, size_px, leading_px;
    int8_t letter_space;
    int16_t tracking;
    uint16_t weight[2];      /* regular, emphasized */
    orb_face_ids_t face[2];  /* regular, emphasized */
} orb_style_row_t;

typedef struct {
    orb_font_id_t font;
    uint16_t weight;
} orb_clock_row_t;

/* The fonts of one content size, with or without Bold Text. */
typedef struct {
    orb_style_row_t styles[ORB_TEXT_STYLE_COUNT];
    orb_clock_row_t clocks[ORB_CLOCK_COUNT];
} orb_font_set_t;

typedef struct {
    uint8_t size_px;
    int16_t tracking;
    int8_t letter_space;
} orb_clock_face_t;

typedef struct {
    uint32_t offset; /* of the zlib stream, from the start of the pack */
    uint32_t stored; /* its size */
    uint32_t size;   /* the record's size once inflated */
} orb_pack_entry_t;

typedef struct {
    const char *notices;
    const char *style_names[ORB_TEXT_STYLE_COUNT];
    orb_clock_face_t clock_faces[ORB_CLOCK_COUNT];
    orb_font_set_t sets[ORB_CONTENT_SIZE_COUNT][2]; /* [content size][Bold Text] */
    const lv_font_t *const *builtin;
    uint16_t builtin_count;
    const orb_pack_entry_t *pack;
    uint16_t pack_count;
    uint32_t pack_size;
    uint8_t pack_id[16];
} orb_fonts_table_t;

extern const orb_fonts_table_t orb_fonts_table;  /* src/orb_fonts_table.c */
extern const char orb_fonts_notices_text[];      /* src/orb_fonts_notices.c */

/* App-owned U+2026 only; safe for immutable numeric/icon pack faces to borrow. */
const lv_font_t *orb_fonts_ellipsis_fallback(void);
void orb_fonts_status_preload(void);
const lv_font_t *orb_fonts_builtin_ellipsis(const lv_font_t *font);
const lv_font_t *orb_fonts_builtin_source(const lv_font_t *font);

enum {
    ORB_FONT_TEXT_ROLES = ORB_TEXT_STYLE_COUNT * 6,
    ORB_FONT_ROLES = ORB_FONT_TEXT_ROLES + ORB_CLOCK_COUNT,
    ORB_FONT_HANDLE_COUNT = ORB_CONTENT_SIZE_COUNT * 2 * ORB_FONT_ROLES,
};
const lv_font_t *orb_fonts_handle(const orb_font_set_t *set, unsigned bank, unsigned role);
const lv_font_t *orb_fonts_handle_source(const lv_font_t *font);
bool orb_fonts_handles_prepare(void);
void orb_fonts_handles_refresh(const orb_font_set_t *set);
/* Main/UI holds LVGL; drain queued tasks as well as both active draw workers. */
void orb_fonts_draw_barrier(void);

/* orb_fonts_pack.c */

/* Checks the pack's header against the table once; later calls return the same answer. */
orb_fonts_err_t orb_fonts_pack_open(void);

/* The font with this id: a built-in one, a pack font already loaded, or a pack font loaded
 * now. NULL if it can't be loaded (logged). */
const lv_font_t *orb_fonts_get(orb_font_id_t id);
const lv_font_t *orb_fonts_peek(orb_font_id_t id);

/* Frees every loaded pack font that `set` doesn't use; NULL releases all. */
void orb_fonts_pack_release_unused(const orb_font_set_t *set);
/* Prepares a complete immutable set after retiring records it does not share. */
orb_fonts_err_t orb_fonts_pack_preload(const orb_font_set_t *set);
/* After boot, UI lookups may only return published records or the rescue face. */
void orb_fonts_pack_cache_only(void);

/* Builds an LVGL font from one inflated record (pack.py's layout) that stays owned by the
 * caller and must outlive the font. Returns NULL if the record is malformed. The host check
 * uses it to compare every record with the C font it was made from. */
lv_font_t *orb_fonts_record_font(const uint8_t *record, size_t size, const lv_font_t *fallback);
void orb_fonts_record_font_free(lv_font_t *font);

/*
 * Platform hooks for the pack, implemented by port/orb_fonts_port_esp.c on the device and
 * by the host check (tools/fonts/lvgl_check/main.c) on the Mac.
 */
bool orb_fonts_port_read(uint32_t offset, void *dst, size_t size);              /* from the pack's start */
bool orb_fonts_port_inflate(void *dst, size_t dst_size, const void *src, size_t src_size); /* zlib, Adler-32 checked */
void *orb_fonts_port_alloc(size_t size);                                       /* PSRAM for big blocks */
void orb_fonts_port_free(void *ptr);

/* Bitmap-only flash leases. Acquire/release span the LVGL decode, so a
 * single-slot assets update can migrate all leases before its first erase. */
typedef struct orb_fonts_bitmap orb_fonts_bitmap_t;
orb_fonts_bitmap_t *orb_fonts_port_map(uint32_t offset, size_t size);
const uint8_t *orb_fonts_port_bitmap_acquire(orb_fonts_bitmap_t *bitmap);
void orb_fonts_port_bitmap_release(void);
void orb_fonts_port_unmap(orb_fonts_bitmap_t *bitmap);
void orb_fonts_port_bitmap_stats(size_t *mapped, size_t *copied);

#ifdef ESP_PLATFORM
/* Serialize the whole release/load/recovery transaction with assets OTA. */
bool orb_fonts_port_begin_switch(void);
void orb_fonts_port_end_switch(void);
/* Streaming inflation retains only a 1 KiB input window, not the whole stream. */
bool orb_fonts_port_read_inflate(void *dst, size_t dst_size, uint32_t offset, size_t stored);
/* UI-only cache trim; worker allocations still preserve the physical floor. */
void orb_fonts_port_prepare(void);
void orb_fonts_port_preferences_load(orb_content_size_t *size, bool *bold);
bool orb_fonts_port_preferences_save(orb_content_size_t size, bool bold);
#endif
