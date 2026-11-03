/* Host tests: the glass shapes' distance fields and masks. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "orb_glass_sdf.h"

static void test_capsule(void)
{
    float nx, ny;
    /* A 72 px circle: -r at the centre, 0 on the edge, positive outside. */
    CHECK(NEAR(orb_sdf_capsule(36, 36, 72, 72, &nx, &ny), -36, 1e-4f));
    CHECK(NEAR(orb_sdf_capsule(36, 0, 72, 72, &nx, &ny), 0, 1e-4f));
    CHECK(NEAR(nx, 0, 1e-4f) && NEAR(ny, -1, 1e-4f));   /* the top edge faces up */
    CHECK(orb_sdf_capsule(80, 36, 72, 72, 0, 0) > 0);
    /* A 196 x 72 capsule: flat along the top, round at the ends. */
    CHECK(NEAR(orb_sdf_capsule(98, 0, 196, 72, &nx, &ny), 0, 1e-4f));
    CHECK(NEAR(orb_sdf_capsule(0, 36, 196, 72, &nx, &ny), 0, 1e-4f));
    CHECK(NEAR(nx, -1, 1e-4f));
    CHECK(orb_sdf_capsule(2, 2, 196, 72, 0, 0) > 0);    /* outside the rounded corner */
    CHECK(NEAR(orb_sdf_coverage(-3), 1, 1e-6f) && NEAR(orb_sdf_coverage(3), 0, 1e-6f));
    CHECK(NEAR(orb_sdf_coverage(0), 0.5f, 1e-6f));
}

static void test_shadow(void)
{
    enum { W = 72, P = 18, M = W + 2 * P };
    static uint8_t a8[M * M];
    orb_capsule_t glass = { P, P, W, W };
    orb_sdf_shadow(a8, M, M, M, glass, glass, 3, 6, 77);
    /* None under the glass, some just below it (it's cast downwards), little far out. */
    CHECK(a8[(P + W / 2) * M + P + W / 2] == 0);
    uint8_t below = a8[(P + W + 2) * M + P + W / 2], above = a8[(P - 3) * M + P + W / 2];
    CHECK(below > 0 && below <= 77);
    CHECK(below > above);
    CHECK(a8[0] == 0);
    /* Left and right are mirror images. */
    for (int y = 0; y < M; y++) {
        for (int x = 0; x < M; x++) {
            CHECK(abs(a8[y * M + x] - a8[y * M + (M - 1 - x)]) <= 1);
        }
    }
    /* The edge goes inside the glass, on top of the shadow. */
    orb_sdf_edge(a8, M, glass, 46);
    CHECK(a8[(P + 2) * M + P + W / 2] > 0);
    CHECK(a8[(P + W / 2) * M + P + W / 2] == 0);
}

static void test_rim(void)
{
    enum { W = 72 };
    int n = orb_sdf_rim_pixels(NULL, W, W);
    CHECK(n > 2 * 3 * W && n < 4 * 4 * W);   /* roughly the circumference times the rim's width */
    orb_rim_px_t *px = malloc(sizeof(*px) * n);
    CHECK(orb_sdf_rim_pixels(px, W, W) == n);
    for (int i = 0; i < n; i++) {
        CHECK(fabsf(orb_sdf_capsule(px[i].x + 0.5f, px[i].y + 0.5f, W, W, 0, 0)) < 2.5f);
    }
    /* Light from above (y grows down): the top of the rim is the brightest, the bottom has the bounce. */
    static uint8_t a8[W * W];
    memset(a8, 0, sizeof(a8));
    orb_sdf_rim_light(a8, W, px, n, 0, -1, 1);
    uint8_t top = 0, bottom = 0, side = 0;
    for (int y = 0; y < 3; y++) {
        top = a8[y * W + W / 2] > top ? a8[y * W + W / 2] : top;
        bottom = a8[(W - 1 - y) * W + W / 2] > bottom ? a8[(W - 1 - y) * W + W / 2] : bottom;
    }
    for (int x = 0; x < 3; x++) {
        side = a8[(W / 2) * W + x] > side ? a8[(W / 2) * W + x] : side;
    }
    CHECK(top > bottom && bottom > side && side > 0);
    CHECK(a8[(W / 2) * W + W / 2] == 0);
    free(px);
}

static void test_fixed_masks(void)
{
    const int shapes[][2] = {{36,36},{71,73},{72,72},{88,88},{96,96},{100,100},{140,72},{188,72},{196,72},{220,72},{230,72},{250,72}};
    for (unsigned shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); shape++) {
        for (int step = 0; step < 4; step++) {
            int w = shapes[shape][0], h = shapes[shape][1];
            int mw = w + (int)(w * .06f * step / 3) / 2 * 2;
            int mh = h + (int)(h * .06f * step / 3) / 2 * 2;
            int pad = 23, dw = mw + 2 * pad, dh = mh + 2 * pad;
            int cap = 6 * (mw + mh);
            uint8_t *reference = calloc(dw * dh, 1), *fast = calloc(dw * dh, 1);
            uint8_t *rim = calloc(mw * mh, 1), *reference_rim = calloc(mw * mh, 1);
            orb_rim_px_t *pixels = malloc(cap * sizeof(*pixels));
            orb_rim_px_t *ref_pixels = malloc(cap * sizeof(*pixels));
            CHECK(reference && fast && rim && reference_rim && pixels && ref_pixels);
            orb_capsule_t glass = {pad, pad, mw, mh};
            orb_capsule_t rest = {pad + (mw - w) / 2, pad + (mh - h) / 2, w, h};
            orb_sdf_shadow(reference, dw, dw, dh, rest, glass, 3, 7, 46);
            orb_sdf_edge(reference, dw, glass, 46);
            int n = orb_sdf_masks(fast, dw, dw, dh, pixels, cap, rest, glass, 3, 7, 46, 46);
            CHECK(n > 0 && n <= cap);
            for (int i = 0; i < dw * dh; i++) {
                /* The historical cutoff at rounded alpha 3 is discontinuous;
                 * Q8 may cross it by <1/256. All other dark alpha stays <=1. */
                int delta = abs(fast[i] - reference[i]);
                CHECK(delta <= 1 || (delta == 3 && (!fast[i] || !reference[i])));
            }
            int ref_n = orb_sdf_rim_pixels(ref_pixels, mw, mh);
            CHECK(abs(n - ref_n) <= 4);
            for (int light = 0; light < 3; light++) {
                float angle = light * 2.1f;
                orb_sdf_rim_light(rim, mw, pixels, n, cosf(angle), sinf(angle), .6f);
                orb_sdf_rim_light(reference_rim, mw, ref_pixels, ref_n, cosf(angle), sinf(angle), .6f);
                for (int i = 0; i < mw * mh; i++) CHECK(abs(rim[i] - reference_rim[i]) <= 2);
            }
            CHECK(orb_sdf_masks(fast, dw, dw, dh, pixels, 0, rest, glass, 3, 7, 46, 46) == -1);
            free(reference); free(fast); free(rim); free(reference_rim); free(pixels); free(ref_pixels);
        }
    }
}

int main(void)
{
    test_capsule();
    test_shadow();
    test_rim();
    test_fixed_masks();
    return finish("orb_ui glass SDF");
}
