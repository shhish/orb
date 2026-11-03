#include "orb_glass_sdf.h"

#include <math.h>

#define RIM_WIDTH 1.5f     /* px: the hairline and the specular highlight */
#define EDGE_WIDTH 1.5f    /* px: the darkened band just inside the rim */
#define HAIRLINE 0.12f     /* the rim where no light hits it */
#define SPEC_FAR 0.3f      /* the bounce on the far side, relative to the highlight */
#define SHADOW_FLOOR 3     /* shadow alpha below this (1 %) is dropped */

float orb_sdf_capsule(float px, float py, float w, float h, float *nx, float *ny)
{
    /* The capsule is the set of points within r of a segment through its middle. */
    float r = (w < h ? w : h) / 2;
    float ax = r, ay = r, bx = w - r, by = h - r;
    float qx = px < ax ? ax : px > bx ? bx : px;
    float qy = py < ay ? ay : py > by ? by : py;
    float dx = px - qx, dy = py - qy, len = sqrtf(dx * dx + dy * dy);
    if (nx) {
        *nx = len > 0 ? dx / len : 0;
        *ny = len > 0 ? dy / len : 0;
    }
    return len - r;
}

float orb_sdf_coverage(float d)
{
    float c = 0.5f - d;
    return c < 0 ? 0 : c > 1 ? 1 : c;
}

static uint8_t to_a8(float v)
{
    return (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + 0.5f);
}

static float capsule_at(orb_capsule_t c, float px, float py)
{
    return orb_sdf_capsule(px - c.x, py - c.y, c.w, c.h, 0, 0);
}

void orb_sdf_shadow(uint8_t *a8, int stride, int mw, int mh, orb_capsule_t caster, orb_capsule_t cover, float off_y,
                    float sigma, uint8_t opa)
{
    /* A blurred edge is the Gaussian's cumulative distribution across it. */
    float k = 1 / (sigma * sqrtf(2));
    for (int y = 0; y < mh; y++) {
        for (int x = 0; x < mw; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float a = 0.5f * erfcf(capsule_at(caster, px, py - off_y) * k);
            a *= 1 - orb_sdf_coverage(capsule_at(cover, px, py));
            /* The faint tail is invisible but costs a blend per pixel: leave it out. */
            uint8_t v = to_a8(a * opa / 255);
            a8[y * stride + x] = v < SHADOW_FLOOR ? 0 : v;
        }
    }
}

void orb_sdf_edge(uint8_t *a8, int stride, orb_capsule_t c, uint8_t opa)
{
    for (int y = 0; y < c.h; y++) {
        for (int x = 0; x < c.w; x++) {
            float d = orb_sdf_capsule(x + 0.5f, y + 0.5f, c.w, c.h, 0, 0);
            float band = orb_sdf_coverage(d + RIM_WIDTH) - orb_sdf_coverage(d + RIM_WIDTH + EDGE_WIDTH);
            uint8_t a = to_a8(band * opa / 255), *p = &a8[(c.y + y) * stride + c.x + x];
            *p = a > *p ? a : *p;
        }
    }
}

int orb_sdf_rim_pixels(orb_rim_px_t *out, int w, int h)
{
    int n = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float nx, ny;
            float d = orb_sdf_capsule(x + 0.5f, y + 0.5f, w, h, &nx, &ny);
            float ring = orb_sdf_coverage(d) - orb_sdf_coverage(d + RIM_WIDTH);
            if (ring * 255 < 1) {
                continue;
            }
            if (out) {
                out[n] = (orb_rim_px_t){ (uint16_t)x, (uint16_t)y, to_a8(ring), (int8_t)lrintf(nx * 127),
                                         (int8_t)lrintf(ny * 127) };
            }
            n++;
        }
    }
    return n;
}

/* x^6 for x in [0, 1], by multiplication: how tightly the highlight hugs the light. */
static float spec(float x)
{
    float x2 = x * x;
    return x2 * x2 * x2;
}

void orb_sdf_rim_light(uint8_t *a8, int stride, const orb_rim_px_t *px, int n, float lx, float ly, float amount)
{
    float peak = 0.4f + 0.2f * amount;
    for (int i = 0; i < n; i++) {
        /* Intensity = peak * max(0, n.l)^p, as on a lit convex edge, plus a bounce opposite. */
        float dot = (px[i].nx * lx + px[i].ny * ly) / 127;
        float lit = dot > 0 ? spec(dot) : SPEC_FAR * spec(-dot);
        a8[px[i].y * stride + px[i].x] = to_a8(px[i].ring / 255.0f * (HAIRLINE + peak * lit));
    }
}

/* Q8 distances keep the same half-pixel AA ramp without per-pixel sqrtf or
 * erfcf on the S3. Integer square root plus interpolation is within 1/256 px
 * at a standard control's edge. Interior and invisible shadow tails skip it. */
static unsigned root(unsigned n)
{
    if (!n) return 0;
    unsigned x = 1u << ((32 - __builtin_clz(n) + 1) / 2);
    for (;;) {
        unsigned next = (x + n / x) / 2;
        if (next >= x) return x;
        x = next;
    }
}

static unsigned capsule_square(int x, int y, orb_capsule_t c, int *dx, int *dy)
{
    int diameter = c.w < c.h ? c.w : c.h;
    int qx = 2 * (x - c.x) + 1 - c.w;
    int qy = 2 * (y - c.y) + 1 - c.h;
    *dx = qx < 0 ? -qx : qx;
    *dy = qy < 0 ? -qy : qy;
    *dx -= c.w - diameter;
    *dy -= c.h - diameter;
    if (*dx < 0) *dx = 0;
    if (*dy < 0) *dy = 0;
    return (unsigned)(*dx * *dx + *dy * *dy);
}

static int distance_q8(unsigned square, int diameter)
{
    unsigned r = root(square);
    return (int)(r * 128 + ((square - r * r) * 128 + r) / (2 * r + 1)) - diameter * 128;
}

static unsigned coverage_q8(int d)
{
    int a = 128 - d;
    return a < 0 ? 0 : a > 256 ? 256 : (unsigned)a;
}

/* Gaussian survival function at 1/32 sigma, Q16; its far tail is below the
 * existing 1% alpha cutoff. Generated from .5*erfc(x/sqrt(2)), not a new blur. */
static const uint16_t s_shadow_curve[] = {
    32768, 31951, 31135, 30320, 29508, 28699, 27894, 27094, 26299, 25510, 24729, 23955,
    23189, 22432, 21684, 20947, 20220, 19505, 18801, 18110, 17432, 16766, 16114, 15476,
    14852, 14243, 13648, 13068, 12503, 11954, 11420, 10901, 10398, 9910, 9437, 8981,
    8539, 8113, 7701, 7305, 6924, 6557, 6205, 5866, 5542, 5231, 4934, 4650,
    4378, 4119, 3872, 3637, 3413, 3200, 2999, 2807, 2625, 2453, 2291, 2137,
    1992, 1855, 1726, 1605, 1491, 1384, 1283, 1189, 1101, 1018, 941, 868,
    801, 738, 680, 626, 575, 528, 485, 444, 407, 372, 341, 311,
    284, 259, 236, 215, 195, 177, 161, 146, 132, 120, 108, 98,
    88, 80, 72, 65, 58, 52, 47, 42, 38, 34, 30, 27,
    24, 22, 19, 17, 15, 14, 12, 11, 9, 8, 7, 7,
    6, 5, 5, 4, 3, 3, 3, 2, 2,
};

static unsigned shadow_q16(int distance, unsigned sigma)
{
    unsigned a = distance < 0 ? -distance : distance;
    unsigned q = a / (sigma * 8); /* sigma-normalized Q5 index, Q8 fraction */
    unsigned fraction = a % (sigma * 8);
    unsigned value = 0;
    if (q < 128) {
        value = s_shadow_curve[q] -
                ((s_shadow_curve[q] - s_shadow_curve[q + 1]) * fraction + sigma * 4) / (sigma * 8);
    }
    return distance < 0 ? 65536 - value : value;
}

/* Generate all geometry together: one boundary scan, mirrored shadow spans,
 * sparse normals, and no supersampling or transcendental functions. The
 * caller zeros the masks once. Normal/tint/light are not part of this key. */
int orb_sdf_masks(uint8_t *dark, int stride, int mw, int mh,
                  orb_rim_px_t *pixels, int capacity, orb_capsule_t caster,
                  orb_capsule_t cover, int off_y, int sigma, uint8_t shadow_opa,
                  uint8_t edge_opa)
{
    int diameter = cover.w < cover.h ? cover.w : cover.h;
    int n = 0;
    for (int y = 0; y < cover.h; y++) {
        for (int x = 0; x < cover.w; x++) {
            int dx, dy;
            unsigned square = capsule_square(x + cover.x, y + cover.y, cover, &dx, &dy);
            /* The complete rim/inner edge ends 3.5 px inside the boundary. */
            if (diameter > 7 && square < (unsigned)((diameter - 7) * (diameter - 7))) continue;
            if (square > (unsigned)((diameter + 1) * (diameter + 1))) continue;
            int d = distance_q8(square, diameter);
            unsigned ring = coverage_q8(d) - coverage_q8(d + 384);
            unsigned edge = coverage_q8(d + 384) - coverage_q8(d + 768);
            dark[(y + cover.y) * stride + x + cover.x] = (edge * edge_opa + 128) / 256;
            if (ring < 1) continue;
            if (n >= capacity) return -1;
            unsigned len = (unsigned)(d + diameter * 128);
            int nx = len ? (int)((dx * 127u * 128 + len / 2) / len) : 0;
            int ny = len ? (int)((dy * 127u * 128 + len / 2) / len) : 0;
            /* Quantize normals against squared half-integer thresholds. This
             * avoids a near-tie in approximate length moving specular by 5 A8. */
            uint64_t xx = (uint64_t)(254 * dx) * (254 * dx);
            uint64_t yy = (uint64_t)(254 * dy) * (254 * dy);
            while ((uint64_t)(2 * nx + 1) * (2 * nx + 1) * square < xx) nx++;
            while (nx && (uint64_t)(2 * nx - 1) * (2 * nx - 1) * square > xx) nx--;
            while ((uint64_t)(2 * ny + 1) * (2 * ny + 1) * square < yy) ny++;
            while (ny && (uint64_t)(2 * ny - 1) * (2 * ny - 1) * square > yy) ny--;
            if (2 * x + 1 < cover.w) nx = -nx;
            if (2 * y + 1 < cover.h) ny = -ny;
            pixels[n++] = (orb_rim_px_t){x, y, (ring * 255 + 128) / 256, nx, ny};
        }
    }
    orb_capsule_t shifted = caster;
    shifted.y += off_y;
    int caster_d = caster.w < caster.h ? caster.w : caster.h;
    unsigned outer = (unsigned)(caster_d + 6 * sigma);
    outer *= outer;
    for (int y = 0; y < mh; y++) {
        for (int x = 0; x < (mw + 1) / 2; x++) {
            int dx, dy;
            unsigned square = capsule_square(x, y, cover, &dx, &dy);
            if (square < (unsigned)((diameter - 1) * (diameter - 1))) continue;
            unsigned outside = 256 - coverage_q8(distance_q8(square, diameter));
            if (!outside) continue;
            square = capsule_square(x, y, shifted, &dx, &dy);
            if (square > outer) continue;
            unsigned alpha = (shadow_q16(distance_q8(square, caster_d), sigma) * shadow_opa + 128) / 256;
            alpha = (alpha * outside + 32768) / 65536;
            if (alpha < SHADOW_FLOOR) continue;
            uint8_t *a = dark + y * stride + x;
            uint8_t *b = dark + y * stride + mw - 1 - x;
            if (alpha > *a) *a = alpha;
            if (alpha > *b) *b = alpha;
        }
    }
    return n;
}
