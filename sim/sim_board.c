#include "sim_board.h"

#include <math.h>

void sim_board_gravity(float right_deg, float down_deg, float g[3])
{
    /* The sensor reads "up": (0, 0, 1) lying flat. Dipping the right edge turns
     * the device about its y axis, so up leans to -x; then dipping the bottom
     * edge turns it about its x axis, so up leans to +y. */
    float a = right_deg * (float)M_PI / 180, b = down_deg * (float)M_PI / 180;
    g[0] = -sinf(a) * cosf(b);
    g[1] = sinf(b);
    g[2] = cosf(a) * cosf(b);
}

uint8_t sim_board_cover(int x, int y, int d)
{
    /* Distance from the pixel's centre to the rim, clamped to one pixel's width. */
    float r = d / 2.0f, dx = x + 0.5f - r, dy = y + 0.5f - r;
    float distance = sqrtf(dx * dx + dy * dy);
    /* Circular transport never refreshes exterior pixel centres. Exposing an
     * AA fringe there retained stale ink after full black redraws (AX Sleep). */
    if (distance > r) return 0;
    float in = r - distance + 0.5f;
    return in >= 1 ? 255 : in <= 0 ? 0 : (uint8_t)lrintf(in * 255);
}

void sim_board_to_rgb(const uint8_t *px, const uint8_t *cover, uint8_t *rgb, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned c = (unsigned)px[2 * i] << 8 | px[2 * i + 1];
        unsigned r = c >> 11, g = c >> 5 & 0x3f, b = c & 0x1f;
        unsigned k = cover ? cover[i] : 255;
        /* 5 and 6 bits widened to 8 by repeating the top bits, so full scale stays full (white is white). */
        rgb[3 * i] = (uint8_t)(((r << 3 | r >> 2) * k + 127) / 255);
        rgb[3 * i + 1] = (uint8_t)(((g << 2 | g >> 4) * k + 127) / 255);
        rgb[3 * i + 2] = (uint8_t)(((b << 3 | b >> 2) * k + 127) / 255);
    }
}
