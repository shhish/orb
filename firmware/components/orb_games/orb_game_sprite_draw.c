#include "orb_game_view_private.h"
#include "src/draw/lv_draw_private.h"

#include <math.h>
#include <stdlib.h>

static void shape(lv_layer_t *layer, int x, int y, int rx, int ry, uint32_t colour,
                   uint8_t opacity, int border, int radius)
{
    const lv_area_t *clip = &layer->_clip_area;
    if (x + rx < clip->x1 || x - rx > clip->x2 ||
        y + ry < clip->y1 || y - ry > clip->y2) return;
    if (radius == LV_RADIUS_CIRCLE && rx == ry) {
        /* Static game rings are revisited for each small sprite rectangle.
         * Reject their empty centre/corners before allocating a draw task or
         * acquiring LVGL's circle-mask cache. Two pixels conservatively retain
         * all antialiased boundary ink; the normal renderer owns coverage. */
        int dx = clip->x1 > x ? clip->x1 - x : clip->x2 < x ? x - clip->x2 : 0;
        int dy = clip->y1 > y ? clip->y1 - y : clip->y2 < y ? y - clip->y2 : 0;
        int far_x = LV_MAX(abs(clip->x1 - x), abs(clip->x2 - x));
        int far_y = LV_MAX(abs(clip->y1 - y), abs(clip->y2 - y));
        int outer = rx + 2, inner = LV_MAX(0, rx - border - 2);
        if (dx * dx + dy * dy > outer * outer ||
            (border && far_x * far_x + far_y * far_y < inner * inner)) return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = radius;
    d.bg_color = lv_color_hex(colour);
    d.bg_opa = border ? LV_OPA_TRANSP : opacity;
    d.border_color = lv_color_hex(colour);
    d.border_opa = opacity;
    d.border_width = border;
    lv_area_t a = {x - rx, y - ry, x + rx, y + ry};
    lv_draw_rect(layer, &d, &a);
}

void game_sprite_disc(lv_layer_t *layer, int x, int y, int radius, uint32_t colour,
                      uint8_t opacity, int border)
{
    shape(layer, x, y, radius, radius, colour, opacity, border, LV_RADIUS_CIRCLE);
}

static void draw_gate(lv_layer_t *layer, const orb_game_sprite_t *s, int x, int y, int r)
{
    /* An outer gate's bounding box encloses inner-ring and marble damage
     * even though its stone has no pixels there. Reject that empty
     * annulus before allocating the two ARC tasks. Four pixels cover the
     * translated highlight, AA and LVGL's integer radius/angle rounding. */
    const lv_area_t *clip = &layer->_clip_area;
    float near_x = clip->x1 > x ? (float)(clip->x1 - x) :
        clip->x2 < x ? (float)(x - clip->x2) : 0;
    float near_y = clip->y1 > y ? (float)(clip->y1 - y) :
        clip->y2 < y ? (float)(y - clip->y2) : 0;
    float far_x = (float)LV_MAX(abs(clip->x1 - x), abs(clip->x2 - x));
    float far_y = (float)LV_MAX(abs(clip->y1 - y), abs(clip->y2 - y));
    float outer = s->arc_radius + r + 4;
    float inner = fmaxf(0, s->arc_radius - r - 4);
    if (near_x * near_x + near_y * near_y > outer * outer ||
        far_x * far_x + far_y * far_y < inner * inner) return;
    /* The same continuous intervals drive collision. A translated flat
     * edge gives the stone a 1 px top highlight without a gradient. */
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center = (lv_point_t){x, y - 1};
    d.radius = (uint16_t)lroundf(s->arc_radius) + r;
    d.width = r * 2; d.rounded = true;
    d.start_angle = s->arc_start * 360 / TAU;
    d.end_angle = s->arc_end * 360 / TAU;
    d.opa = s->opacity; d.color = lv_color_hex(0x636366);
    lv_draw_arc(layer, &d);
    d.center.y = y; d.color = lv_color_hex(s->colour);
    lv_draw_arc(layer, &d);
}

static void draw_star(lv_layer_t *layer, const orb_game_sprite_t *s, int x, int y, int r)
{
    /* A flat five-point star (Orbit's collectible), triangulated from the
     * centre so no glow or gradient is involved. */
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.opa = s->opacity;
    t.color = lv_color_hex(s->colour);
    lv_point_precise_t v[10];
    for (int i = 0; i < 10; ++i) {
        float rr = (i % 2 == 0) ? (float)r : (float)r * 0.5f;
        float a = -TAU / 4 + (float)i * TAU / 10;
        v[i] = (lv_point_precise_t){x + cosf(a) * rr, y + sinf(a) * rr};
    }
    /* Overlap the five tips with a flat centre. Independent triangles
     * meeting on radial edges otherwise leave antialias seams inside. */
    game_sprite_disc(layer, x, y, r / 2, s->colour, s->opacity, 0);
    for (int i = 0; i < 10; i += 2) {
        t.p[0] = v[i];
        t.p[1] = v[(i + 9) % 10];
        t.p[2] = v[(i + 1) % 10];
        lv_draw_triangle(layer, &t);
    }
}

void game_sprite_draw(lv_layer_t *layer, const orb_game_sprite_t *s)
{
    int x = (int)lroundf(s->x), y = (int)lroundf(s->y), r = s->radius;
    if (!r || !s->opacity) return;
    switch (s->shape) {
    case ORB_GAME_BEAD: {
        /* HIG §2.2: seeded flat bodies, with no printed-on highlight. The
         * smaller ink radius keeps individual beads visible in a pool. */
        game_sprite_disc(layer, x, y, r, s->colour, s->opacity, 0);
        break;
    }
    case ORB_GAME_MARBLE_SPRITE:
        /* Design 1.0 uses a white marble with one flat grey reflection. */
        game_sprite_disc(layer, x, y, r, 0xffffff, s->opacity, 0);
        game_sprite_disc(layer, x + s->light_x / 2 - 4, y + s->light_y / 2 - 4,
             3, 0xe0e0e0, s->opacity, 0);
        break;
    case ORB_GAME_BUBBLE:
        /* HIG §§2/5/9: bubbles are content, a flat translucent body and a
         * flat rim. Review #36: no printed-on specular dot (§2.2 forbids
         * static highlights); a thumbnail bubble keeps a hairline rim. */
        game_sprite_disc(layer, x, y, r, s->colour, (uint8_t)(s->opacity * 31 / 255), 0);
        game_sprite_disc(layer, x, y, r, s->colour, s->opacity, r >= 12 ? 3 : 1);
        break;
    case ORB_GAME_HEAD: {
        game_sprite_disc(layer, x, y, r, s->colour, s->opacity, 0);
        game_sprite_disc(layer, x + s->light_x, y + s->light_y, r >= 8 ? 2 : 1, 0x000000, s->opacity, 0);
        break;
    }
    case ORB_GAME_CAPSULE:
        shape(layer, x, y, r, r / 2, s->colour, s->opacity, 0, LV_RADIUS_CIRCLE);
        break;
    case ORB_GAME_FLOWER:
        /* Original five-petal flat blossom. Content art, never glass or a
         * borrowed character; every petal stays inside the sprite damage. */
        for (unsigned i = 0; i < 5; ++i) {
            float a = (float)i * TAU / 5 - TAU / 4 + s->light_x * TAU / 360;
            int petal = r * 3 / 7;
            game_sprite_disc(layer, x + (int)lroundf(cosf(a) * (r - petal)),
                 y + (int)lroundf(sinf(a) * (r - petal)), petal,
                 s->colour, s->opacity, 0);
        }
        game_sprite_disc(layer, x, y, r / 4, 0xffffff, s->opacity, 0);
        break;
    case ORB_GAME_STONE: {
        /* A filled, rotated pebble with one quiet upper edge. HIG §§1/5:
         * the highlight describes the stone; it is never a minus badge. */
        float a = s->light_x * TAU / 360;
        float dx = cosf(a) * r * .28f, dy = sinf(a) * r * .28f;
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.p1 = (lv_point_precise_t){x - dx, y - dy};
        d.p2 = (lv_point_precise_t){x + dx, y + dy};
        d.width = r * 1.4f; d.round_start = d.round_end = true;
        d.color = lv_color_hex(s->colour); d.opa = s->opacity;
        lv_draw_line(layer, &d);
        d.p1 = (lv_point_precise_t){x - dx - sinf(a) * r * -.55f,
            y - dy - cosf(a) * r * .55f};
        d.p2 = (lv_point_precise_t){x + dx - sinf(a) * r * -.55f,
            y + dy - cosf(a) * r * .55f};
        d.width = 1; d.color = lv_color_hex(0xc4beb3);
        d.opa = (uint8_t)(s->opacity * 3 / 5);
        lv_draw_line(layer, &d);
        break;
    }
    case ORB_GAME_GATE:
        draw_gate(layer, s, x, y, r);
        break;
    case ORB_GAME_DIAMOND: {
        lv_draw_triangle_dsc_t facet;
        lv_draw_triangle_dsc_init(&facet);
        facet.opa = s->opacity;
        facet.color = lv_color_hex(s->colour);
        facet.p[0] = (lv_point_precise_t){x, y - r};
        facet.p[1] = (lv_point_precise_t){x + r, y};
        facet.p[2] = (lv_point_precise_t){x, y + r};
        lv_draw_triangle(layer, &facet);
        facet.p[1] = (lv_point_precise_t){x - r, y};
        lv_draw_triangle(layer, &facet);
        game_sprite_disc(layer, x - 1, y - r / 3, 2, 0xffffff, s->opacity, 0);
        break;
    }
    case ORB_GAME_STAR:
        draw_star(layer, s, x, y, r);
        break;
    case ORB_GAME_SPARK: {
        static const int8_t s_spark_rays[5][4] = {
            {0, -20, 0, -34}, {14, -14, 24, -24}, {20, 0, 34, 0},
            {-14, -14, -24, -24}, {-20, 0, -34, 0}};
        float scale = (float)r / 36;
        for (unsigned i = 0; i < 5; ++i) {
            lv_draw_line_dsc_t d;
            lv_draw_line_dsc_init(&d);
            d.p1 = (lv_point_precise_t){x + s_spark_rays[i][0] * scale, y + s_spark_rays[i][1] * scale};
            d.p2 = (lv_point_precise_t){x + s_spark_rays[i][2] * scale, y + s_spark_rays[i][3] * scale};
            d.width = 3; d.round_start = d.round_end = true;
            d.color = lv_color_white(); d.opa = s->opacity;
            lv_draw_line(layer, &d);
        }
        break;
    }
    default:
        game_sprite_disc(layer, x, y, r, s->colour, s->opacity, s->shape == ORB_GAME_RING ? 2 : 0);
        break;
    }
}
