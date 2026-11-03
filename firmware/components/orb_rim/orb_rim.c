#include "orb_rim.h"

#include <math.h>

#define RIM_PI 3.14159265358979323846f
#define RIM_GAP 24.0f

static void draw_arc(lv_layer_t *layer, lv_point_t origin, float start, float end,
                     int radius, int width, lv_color_t colour)
{
    if (end <= start) return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center = (lv_point_t){origin.x + 233, origin.y + 233};
    d.radius = radius;
    d.width = width;
    d.start_angle = start;
    d.end_angle = end;
    d.color = colour;
    d.rounded = true;
    lv_draw_arc(layer, &d);
}

void orb_rim_draw_segment(const orb_rim_config_t *config, lv_layer_t *layer,
                          lv_point_t origin, float start, float end,
                          int radius, int width, lv_color_t colour)
{
    unsigned gaps = config && config->gaps <= 2 ? config->gaps : 0;
    for (unsigned i = 0; i < gaps; ++i) {
        float gap = 225 + i * 90;
        draw_arc(layer, origin, start, fminf(end, gap - RIM_GAP), radius, width, colour);
        start = fmaxf(start, gap + RIM_GAP);
    }
    draw_arc(layer, origin, start, end, radius, width, colour);
}

void orb_rim_draw_ticks(const orb_rim_config_t *config, lv_layer_t *layer,
                        lv_point_t origin, unsigned steps, int selected,
                        lv_color_t selected_colour, lv_color_t secondary)
{
    if (!steps) return;
    for (unsigned i = 0; i <= steps; ++i) {
        float angle = orb_rim_angle(config, (float)i / steps) * RIM_PI / 180;
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.p1 = (lv_point_precise_t){origin.x + 233 + 185 * cosf(angle), origin.y + 233 + 185 * sinf(angle)};
        int end = i == 0 || i == steps ? 195 : 191;
        d.p2 = (lv_point_precise_t){origin.x + 233 + end * cosf(angle), origin.y + 233 + end * sinf(angle)};
        d.width = (int)i == selected ? 3 : 2;
        d.color = (int)i == selected ? selected_colour : secondary;
        lv_draw_line(layer, &d);
    }
}

void orb_rim_invalidate(const orb_rim_config_t *config, lv_obj_t *canvas,
                        float before, float after)
{
    if (!canvas || before == after) return;
    float from = orb_rim_angle(config, fminf(before, after));
    float to = orb_rim_angle(config, fmaxf(before, after));
    lv_area_t bounds;
    lv_obj_get_coords(canvas, &bounds);
    /* HIG §§2/6: damage only the moving rim and its inner ticks. The centre
     * and opposite rim stay retained; no full arc bounding square is dirty. */
    for (float deg = from; deg <= to + 3; deg += 6) {
        float angle = fminf(deg, to) * RIM_PI / 180;
        int x = bounds.x1 + (int)lroundf(233 + 199 * cosf(angle));
        int y = bounds.y1 + (int)lroundf(233 + 199 * sinf(angle));
        lv_area_t damage = {x - 25, y - 25, x + 25, y + 25};
        lv_obj_invalidate_area(canvas, &damage);
    }
}


