/* Frozen round-1 scalar aperture/glyph oracle (its rim retired in polish #34).
 * Keep its floating-point operation order independent of the production span/fixed-point renderer. */
#include "orb_nav_compositor.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SIDE 466

#ifdef ORB_TEST_S3_COPY
/* Run the compositor's real S3 copy/conversion branch on the host. The SIMD
 * kernel dispatch/alignment itself has the separate nav_simd_copy contract. */
void orb_simd_copy_rgb565(void *destination, const void *source, size_t pixels)
{
    memcpy(destination, source, pixels * 2);
}
#endif

typedef struct {
    bool glyph_visible, reverse;
    uint8_t glyph[96 * 96];
    orb_nav_token_rect_t glyph_source, glyph_target;
    uint16_t glyph_ink;
} reference_t;
static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

static uint16_t blend(uint16_t a, uint16_t b, unsigned amount)
{
    unsigned inv = 256 - amount;
    unsigned r = (((a >> 11) * inv + (b >> 11) * amount + 128) >> 8);
    unsigned g = ((((a >> 5) & 63) * inv + ((b >> 5) & 63) * amount + 128) >> 8);
    unsigned bl = (((a & 31) * inv + (b & 31) * amount + 128) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static int luminance(uint16_t pixel)
{
    int r = ((pixel >> 11) * 255 + 15) / 31;
    int g = (((pixel >> 5) & 63) * 255 + 31) / 63;
    int b = ((pixel & 31) * 255 + 15) / 31;
    return (54 * r + 183 * g + 19 * b + 128) >> 8;
}

static void put_pixel(uint8_t *dst, uint16_t pixel, bool swapped)
{
    if (!swapped) memcpy(dst, &pixel, 2);
    else { dst[0] = pixel >> 8; dst[1] = pixel & 255; }
}

static void glyph_row(const reference_t *c, float progress, int x, int y,
                      int width, uint8_t *dst, bool swapped)
{
    if (!c->glyph_visible || progress <= 0 || progress >= 1) return;
    float t = c->reverse ? 1 - progress : progress;
    /* HIG §§1/6: establish the header before the aperture reveals readable
     * rows. A full-duration flight puts the token on the agent's last step
     * and headline; early arrival preserves identity without covering them. */
    t = fminf(1, t * 3);
    const orb_nav_token_rect_t *a = &c->glyph_source, *b = &c->glyph_target;
    float gx = a->x + (b->x - a->x) * t, gy = a->y + (b->y - a->y) * t;
    float gw = a->width + (b->width - a->width) * t;
    float gh = a->height + (b->height - a->height) * t;
    if (y + 1 <= gy || y >= gy + gh) return;
    int left = (int)floorf(gx), right = (int)ceilf(gx + gw) - 1;
    if (left < x) left = x;
    if (right >= x + width) right = x + width - 1;
    float sy = clampf((y - gy + .5f) * a->height / gh - .5f, 0, a->height - 1);
    int y0 = (int)sy, y1 = y0 + 1 < a->height ? y0 + 1 : y0;
    float fy = sy - y0;
    float cover_y = fminf(y + 1, gy + gh) - fmaxf(y, gy);
    /* The flat launch disc gives way to black as soon as the iris starts.
     * Use the header's contrast-safe ink throughout that flight, so a black
     * launcher glyph does not disappear against the destination canvas. */
    uint16_t ink = c->glyph_ink;
    for (int col = left; col <= right; col++) {
        float sx = clampf((col - gx + .5f) * a->width / gw - .5f, 0, a->width - 1);
        int x0 = (int)sx, x1 = x0 + 1 < a->width ? x0 + 1 : x0;
        float fx = sx - x0;
        float top = c->glyph[(size_t)y0 * a->width + x0] * (1 - fx) +
                    c->glyph[(size_t)y0 * a->width + x1] * fx;
        float bottom = c->glyph[(size_t)y1 * a->width + x0] * (1 - fx) +
                       c->glyph[(size_t)y1 * a->width + x1] * fx;
        float cover_x = fminf(col + 1, gx + gw) - fmaxf(col, gx);
        unsigned alpha = (unsigned)lroundf((top * (1 - fy) + bottom * fy) * cover_x * cover_y * 256 / 255);
        if (!alpha) continue;
        uint8_t *pixel = dst + (size_t)(col - x) * 2;
        uint16_t background;
        if (swapped) background = (uint16_t)((pixel[0] << 8) | pixel[1]);
        else memcpy(&background, pixel, 2);
        put_pixel(pixel, blend(background, ink, alpha), swapped);
    }
}

static void reference_glyph(reference_t *c, const uint16_t *source,
                            orb_nav_token_rect_t token, orb_nav_token_rect_t target)
{
    c->glyph_visible = false;
    int width = token.width * 7 / 10, height = token.height * 7 / 10;
    int x = token.x + (token.width - width) / 2;
    int y = token.y + (token.height - height) / 2;
    if (token.width < 8 || token.height < 8 || width > 96 || height > 96 || x < 0 || y < 0 ||
        x + width > SIDE || y + height > SIDE) return;
    int background = luminance(source[(token.y + token.height / 2) * SIDE + token.x + token.width * 9 / 10]);
    int ink = background > 127 ? 0 : 255;
    int left = width, top = height, right = -1, bottom = -1;
    for (int row = 0; row < height; row++) for (int col = 0; col < width; col++) {
        int value = luminance(source[(y + row) * SIDE + x + col]);
        int alpha = (value - background) * 255 / (ink - background);
        alpha = alpha < 0 ? 0 : alpha > 255 ? 255 : alpha;
        c->glyph[row * width + col] = alpha;
        if (alpha > 12) {
            if (col < left) left = col;
            if (col > right) right = col;
            if (row < top) top = row;
            if (row > bottom) bottom = row;
        }
    }
    if (right < left || bottom < top) return;
    int cropped_width = right - left + 1, cropped_height = bottom - top + 1;
    for (int row = 0; row < cropped_height; row++)
        memmove(c->glyph + row * cropped_width, c->glyph + (row + top) * width + left, cropped_width);
    c->glyph_source = (orb_nav_token_rect_t){x + left, y + top, cropped_width, cropped_height};
    c->glyph_target = target; c->glyph_ink = 0xFFFF; c->glyph_visible = true;
}

static void reference_row(const reference_t *glyph, orb_nav_token_rect_t token, float p,
                          const uint16_t *inner, const uint16_t *outer, int sy, uint16_t *output)
{
    float cx = token.x + token.width * .5f, cy = token.y + token.height * .5f;
    float initial_radius = fmaxf(token.width, token.height) * .5f;
    float far_x = fmaxf(cx, SIDE - 1 - cx), far_y = fmaxf(cy, SIDE - 1 - cy);
    float max_radius = hypotf(far_x, far_y) + 1;
    float radius = initial_radius + (max_radius - initial_radius) * (glyph->reverse ? 1 - p : p);
    int x = 0, width = SIDE;
    bool swapped = false;
    uint8_t *dst = (uint8_t *)output;
    memcpy(dst, outer + x, width * 2);
    float delta_y = sy - cy;
    float square = radius * radius - delta_y * delta_y;
    if (square > 0) {
        /* Analytic circle spans: two edges plus a contiguous RGB565
         * copy per scanline. No whole-screen mask or per-pixel sqrt. */
        float half = sqrtf(square);
        float left_f = cx - half, right_f = cx + half;
        int left = (int)ceilf(left_f), right = (int)floorf(right_f);
        if (left < x) left = x;
        if (right >= x + width) right = x + width - 1;
        if (left <= right) memcpy(dst + (size_t)(left - x) * 2, inner + left, (right - left + 1) * 2);
        int edge = (int)floorf(left_f);
        if (edge >= x && edge < x + width) {
            unsigned alpha = (unsigned)lroundf((1 - (left_f - floorf(left_f))) * 256);
            put_pixel(dst + (size_t)(edge - x) * 2, blend(outer[edge], inner[edge], alpha), swapped);
        }
        edge = (int)ceilf(right_f);
        if (edge >= x && edge < x + width) {
            unsigned alpha = (unsigned)lroundf((1 - (ceilf(right_f) - right_f)) * 256);
            put_pixel(dst + (size_t)(edge - x) * 2, blend(outer[edge], inner[edge], alpha), swapped);
        }
    }
    /* Polish #34: no hairline; the aperture's own AA edge is the boundary. */
    glyph_row(glyph, p, 0, sy, SIDE, dst, false);
}

static unsigned random_value = 17;
static uint16_t random_pixel(void)
{
    random_value ^= random_value << 13; random_value ^= random_value >> 17;
    random_value ^= random_value << 5;
    return (uint16_t)random_value;
}

int main(void)
{
    orb_nav_compositor_t *c = orb_nav_compositor_create(SIDE, SIDE);
    assert(c);
    for (unsigned slot = 0; slot < 2; slot++) {
        assert(orb_nav_compositor_capture_begin(c, slot, 1));
        uint16_t *source = orb_nav_compositor_pixels(c, slot);
        for (unsigned i = 0; i < SIDE * SIDE; i++) source[i] = random_pixel();
        assert(orb_nav_compositor_capture_complete(c, slot));
    }
    const orb_nav_token_rect_t tokens[] = {
        {197, 197, 72, 72}, {8, 8, 32, 32}, {-6, -6, 16, 16},
        {0, 0, 1, 1}, {391, 97, 73, 71}, {200, 200, 1, 1},
        {-900, -500, 13, 17}, {452, -31, 9, 35}, {0, 0, 466, 466},
        {383, 383, -300, -300}, {233, 233, 0, 0},
    };
    const orb_nav_token_rect_t target = {219, 30, 28, 28};
    uint64_t checks = 0;
    uint16_t expected[SIDE];
    uint8_t output[(SIDE * 2 + 7) * 42 + 1];
    uint16_t *dimmed = malloc(SIDE * SIDE * 2);
    assert(dimmed);
    for (unsigned token = 0; token < sizeof(tokens) / sizeof(tokens[0]); token++) {
        for (unsigned reverse = 0; reverse < 2; reverse++) {
            assert(orb_nav_compositor_begin(c, ORB_NAV_TRANSITION_IRIS, reverse, 1 - reverse,
                ORB_NAV_DIRECTION_LEFT, &tokens[token], reverse, false));
            const uint16_t *inner = orb_nav_compositor_pixels(c, 1);
            const uint16_t *outer = orb_nav_compositor_pixels(c, 0);
            for (unsigned i = 0; i < SIDE * SIDE; i++) dimmed[i] = blend(0, outer[i], 205);
            reference_t glyph = {.reverse = reverse};
            reference_glyph(&glyph, outer, tokens[token], target);
            assert(orb_nav_compositor_token_glyph(c, &target, 0xFFFF) == glyph.glyph_visible);
            for (unsigned frame = 0; frame <= 36; frame++) {
                float p = frame / 36.f;
                orb_nav_compositor_drag(c, p);
                for (unsigned swapped = 0; swapped < 2; swapped++) {
                    for (int y = 0; y < SIDE; y += 42) {
                        int height = SIDE - y < 42 ? SIDE - y : 42;
                        /* Odd output address/stride and changing x clips prove
                         * the fast copy never touches row padding or neighbours. */
                        int x = (y / 42) % 17, width = SIDE - 2 * x;
                        size_t stride = width * 2 + 7;
                        memset(output, 0xA5, sizeof(output));
                        assert(orb_nav_compositor_render_band(c, x, y, width, height,
                            output + 1, stride, swapped));
                        for (int row = 0; row < height; row++) {
                            int sy = y + row;
                            if (frame == 0 || frame == 36) {
                                const uint16_t *source = orb_nav_compositor_pixels(c, frame == 0 ? reverse : 1 - reverse);
                                memcpy(expected, source + sy * SIDE, SIDE * 2);
                            } else reference_row(&glyph, tokens[token], p, inner + sy * SIDE,
                                dimmed + sy * SIDE, sy, expected);
                            for (int col = 0; col < width; col++) {
                                const uint8_t *pixel = output + 1 + row * stride + col * 2;
                                uint16_t actual;
                                if (swapped) actual = (uint16_t)((pixel[0] << 8) | pixel[1]);
                                else memcpy(&actual, pixel, 2);
                                if (actual != expected[x + col]) {
                                    fprintf(stderr, "token=%u reverse=%u frame=%u swapped=%u x=%d y=%d: %04x != %04x\n",
                                        token, reverse, frame, swapped, x + col, sy, actual, expected[x + col]);
                                    return 1;
                                }
                                checks++;
                            }
                            for (size_t byte = width * 2; byte < stride; byte++)
                                assert(output[1 + row * stride + byte] == 0xA5);
                        }
                        assert(output[0] == 0xA5);
                    }
                }
            }
        }
    }
    printf("iris scalar oracle: %llu exact pixels, clipped/unaligned bands, forward/reverse and glyph\n",
        (unsigned long long)checks);
    free(dimmed); orb_nav_compositor_destroy(c);
    return 0;
}
