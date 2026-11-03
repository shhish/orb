#include "orb_faces_measure.h"
#include "orb_kit.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "src/misc/lv_text_private.h"

lv_area_t orb_faces_label_ink(lv_obj_t *object)
{
    lv_area_t area; lv_obj_get_coords(object, &area);
    const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
    const char *value = lv_label_get_text(object);
    lv_point_t measured;
    lv_text_get_size(&measured, value, font, lv_obj_get_style_text_letter_space(object, 0), 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int width = LV_MIN(measured.x, lv_obj_get_width(object));
    int top = font->line_height, bottom = 0;
    for (uint32_t i = 0; value[i];) {
        uint32_t character = lv_text_encoded_next(value, &i);
        lv_font_glyph_dsc_t glyph;
        if (lv_font_get_glyph_dsc(font, &glyph, character, 0) && glyph.box_h) {
            int glyph_top = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
            top = LV_MIN(top, glyph_top); bottom = LV_MAX(bottom, glyph_top + glyph.box_h);
        }
    }
    if (!bottom) top = bottom = 0;
    area.x1 += (lv_obj_get_width(object) - width) / 2;
    area.x2 = area.x1 + LV_MAX(0, width - 1);
    area.y1 += top; area.y2 = area.y1 + LV_MAX(0, bottom - top - 1);
    return area;
}

int orb_faces_text_width(lv_obj_t *object, const char *value)
{
    lv_point_t measured;
    lv_text_get_size(&measured, value, lv_obj_get_style_text_font(object, 0),
                     lv_obj_get_style_text_letter_space(object, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return measured.x;
}

void orb_faces_caption_fit(lv_obj_t *object, const char *source, int width, char *out, size_t capacity)
{
    snprintf(out, capacity, "%s", source);
    size_t end = strlen(out);
    /* Kit live-value labels intentionally use CLIP. Fit the optional weather
     * caption first, preserving its beginning and complete UTF-8 characters,
     * rather than centre-cropping a long condition into a meaningless fragment. */
    while (end && orb_faces_text_width(object, out) > width) {
        do { end--; } while (end && ((unsigned char)source[end] & 0xc0) == 0x80);
        while (end && source[end - 1] == ' ') end--;
        snprintf(out, capacity, "%.*s…", (int)end, source);
    }
}

/* Clock-dial geometry: degrees clockwise from 12 o'clock about (233,233). */
void orb_faces_polar(lv_point_precise_t *point, float degrees, float radius)
{
    float angle = (degrees - 90.f) * 3.14159265358979323846f / 180.f;
    point->x = 233 + lroundf(cosf(angle) * radius);
    point->y = 233 + lroundf(sinf(angle) * radius);
}

lv_obj_t *orb_faces_clock_line(lv_obj_t *parent, lv_point_precise_t points[2], int width, lv_color_t colour)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, points, 2);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_color(line, colour, 0);
    lv_obj_set_clickable(line, false);
    return line;
}

void orb_faces_at(lv_obj_t *object, int cx, int cy)
{
    lv_obj_update_layout(object);
    if (lv_obj_get_style_align(object, 0) != LV_ALIGN_TOP_LEFT) lv_obj_set_align(object, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(object, cx - lv_obj_get_width(object) / 2, cy - lv_obj_get_height(object) / 2);
}

/* Style setters always invalidate; Always On's 1Hz refresh must not (HIG §7:
 * update rarely), so state inks change only when they differ. */
void orb_faces_ink_set(lv_obj_t *object, lv_color_t colour, lv_opa_t opacity)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(object, 0), colour)) lv_obj_set_style_text_color(object, colour, 0);
    if (lv_obj_get_style_text_opa(object, 0) != opacity) lv_obj_set_style_text_opa(object, opacity, 0);
}

lv_obj_t *orb_faces_text_create(lv_obj_t *parent, const char *value, int cx, int cy,
                      int width, orb_text_style_t style, lv_color_t colour)
{
    lv_obj_t *object = lv_label_create(parent);
    orb_text_apply(object, style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 0);
    lv_obj_set_style_text_color(object, colour, 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(object, value);
    lv_label_set_long_mode(object, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(object, width, lv_obj_get_style_text_font(object, 0)->line_height);
    lv_obj_set_style_text_line_space(object, 0, 0);
    lv_obj_set_clickable(object, false);
    orb_faces_at(object, cx, cy);
    return object;
}

void orb_faces_clock_fit(lv_obj_t *object, const char *clock, int cy, bool medium, int width)
{
    if (!clock[0]) return;
    orb_clock_apply(object, medium ? ORB_CLOCK_MEDIUM : ORB_CLOCK_LARGE, 0);
    orb_kit_hero_set_value(object, clock, "88:88");
    lv_obj_set_size(object, width, LV_SIZE_CONTENT);
    lv_obj_update_layout(object);
    const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
    lv_font_glyph_dsc_t digit;
    int top = 0, height = font->line_height;
    if (lv_font_get_glyph_dsc(font, &digit, '0', 0)) {
        top = font->line_height - font->base_line - digit.box_h - digit.ofs_y;
        height = digit.box_h;
    }
    lv_obj_set_pos(object, 233 - width / 2, cy - height / 2 - top);
}

void orb_faces_static_text(lv_obj_t *object, char *storage, size_t capacity, const char *value)
{
    if (!object || !strcmp(storage, value)) return;
    snprintf(storage, capacity, "%s", value);
    /* Each form owns fixed string storage. Updating a retained provider never
     * allocates a label buffer on the crown/scroll path. */
    orb_kit_value_set(object, storage, NULL);
}

void orb_faces_lane_at(int scale, lv_obj_t *object, int cx, int cy)
{
    if (!scale) { orb_faces_at(object, cx, cy); return; }
    lv_obj_update_layout(object);
    int w = lv_obj_get_width(object), h = lv_obj_get_height(object);
    if (lv_obj_get_style_transform_pivot_x(object, 0) != w / 2) lv_obj_set_style_transform_pivot_x(object, w / 2, 0);
    if (lv_obj_get_style_transform_pivot_y(object, 0) != h / 2) lv_obj_set_style_transform_pivot_y(object, h / 2, 0);
    lv_obj_set_pos(object, cx * scale / 256 - w / 2, cy * scale / 256 - h / 2);
}


/* HIG §§5/7: cached progress must be dimmed as well as its companion text. */
void orb_faces_ring_ink(lv_obj_t *ring, bool locked, bool stale)
{
    lv_opa_t track = locked ? 25 : stale ? 28 : 46;
    lv_opa_t fill = locked ? (stale ? 100 : 140) : stale ? ORB_KIT_SECONDARY_OPA : LV_OPA_COVER;
    if (lv_obj_get_style_arc_opa(ring, LV_PART_MAIN) != track)
        lv_obj_set_style_arc_opa(ring, track, LV_PART_MAIN);
    if (lv_obj_get_style_arc_opa(ring, LV_PART_INDICATOR) != fill)
        lv_obj_set_style_arc_opa(ring, fill, LV_PART_INDICATOR);
}
