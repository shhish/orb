#include "orb_fonts_private.h"

LV_FONT_DECLARE(orb_inter_28_regular)

const lv_font_t *orb_status_builtin_font_for_cap(orb_text_style_t style,
                                                orb_text_variant_t variant, int32_t cap_height);

typedef struct {
    const lv_font_t *source;
    lv_font_t font;
} status_face_t;

/* Selectors use these wrappers; generated app and asset glyphs stay untouched.
 * Status is numeric-only and never uses DOT mode. The rescue ellipsis makes its
 * availability explicit; compact digits/bolt retain their exact metrics.
 * Keep descriptors in the font owner's PSRAM budget, not internal RAM. */
static status_face_t *s_faces;
static bool s_prepared;
static const lv_font_t *s_last_source;
static const lv_font_t *s_last_font;

static bool ellipsis_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                         uint32_t letter, uint32_t next)
{
    return letter == 0x2026 && orb_inter_28_regular.get_glyph_dsc(&orb_inter_28_regular, glyph, letter, next);
}

static const void *ellipsis_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *buffer)
{
    lv_font_glyph_dsc_t rescue = *glyph;
    rescue.resolved_font = &orb_inter_28_regular;
    return orb_inter_28_regular.get_glyph_bitmap(&rescue, buffer);
}

const lv_font_t *orb_fonts_ellipsis_fallback(void)
{
    /* The rescue face is immutable app data, so workers may share it across pack
     * retirement. Only the ellipsis is exposed, preserving numeric/icon coverage. */
    static const lv_font_t font = {
        .get_glyph_dsc = ellipsis_dsc,
        .get_glyph_bitmap = ellipsis_bitmap,
        .line_height = 35,
        .base_line = 7,
        .static_bitmap = 1,
    };
    return &font;
}

void orb_fonts_status_preload(void)
{
    if (s_prepared) return;
    s_prepared = true;
    s_faces = orb_fonts_port_alloc(sizeof(*s_faces) * orb_fonts_table.builtin_count);
    if (!s_faces) return;
    for (unsigned i = 0; i < orb_fonts_table.builtin_count; i++) {
        s_faces[i].source = orb_fonts_table.builtin[i];
        s_faces[i].font = *s_faces[i].source;
        s_faces[i].font.fallback = orb_fonts_ellipsis_fallback();
    }
}

const lv_font_t *orb_fonts_builtin_ellipsis(const lv_font_t *font)
{
    if (!s_faces || font->fallback) return font;
    for (unsigned i = 0; i < orb_fonts_table.builtin_count; i++) {
        if (s_faces[i].source == font) return &s_faces[i].font;
    }
    return font;
}

const lv_font_t *orb_fonts_builtin_source(const lv_font_t *font)
{
    if (s_faces) {
        for (unsigned i = 0; i < orb_fonts_table.builtin_count; i++) {
            if (&s_faces[i].font == font) return s_faces[i].source;
        }
    }
    return font;
}

const lv_font_t *orb_status_font_for_cap(orb_text_style_t style,
                                        orb_text_variant_t variant, int32_t cap_height)
{
    const lv_font_t *source = orb_status_builtin_font_for_cap(style, variant, cap_height);
    if (!s_faces) return source;
    if (source == s_last_source) return s_last_font;
    for (unsigned i = 0; i < orb_fonts_table.builtin_count; i++) {
        if (s_faces[i].source == source) {
            s_last_source = source;
            s_last_font = &s_faces[i].font;
            return s_last_font;
        }
    }
    return source;
}
