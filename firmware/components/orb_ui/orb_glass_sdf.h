#pragma once

/*
 * The Liquid Glass shapes as signed distance fields (docs/hig.md §2.2), drawn
 * into 8-bit alpha masks once per shape size: a soft shadow outside, the
 * darkened inner edge, and the rim with its specular highlight for a given
 * light direction. Plain C, no LVGL or ESP-IDF, so it builds and is tested on
 * the host.
 *
 * Shapes are capsules filling a w x h box (a circle when w == h); radius
 * min(w, h) / 2. Mask coordinates are pixel centres, (x + 0.5, y + 0.5).
 */

#include <stdint.h>

/* A capsule placed in a mask: its box's top left and size, in pixels. */
typedef struct {
    int x, y, w, h;
} orb_capsule_t;

/* Signed distance from (px, py) to the edge of a w x h capsule at the origin:
 * negative inside. With nx, ny, also the outward normal of the nearest edge. */
float orb_sdf_capsule(float px, float py, float w, float h, float *nx, float *ny);

/* Anti-aliased coverage, 0 to 1, for a signed distance. */
float orb_sdf_coverage(float d);

/*
 * Fills a mw x mh mask with the shadow `caster` throws: a Gaussian blur of it
 * (sigma px) shifted down by off_y, at `opa` where darkest. Nothing where
 * `cover` is, as the glass hides its own shadow.
 */
void orb_sdf_shadow(uint8_t *a8, int stride, int mw, int mh, orb_capsule_t caster, orb_capsule_t cover, float off_y,
                    float sigma, uint8_t opa);

/*
 * Adds the darkened inner edge of `c`, a band just inside its rim at `opa`,
 * to a mask, keeping the larger alpha. Shadow and edge are both black: one
 * mask, one pass to draw.
 */
void orb_sdf_edge(uint8_t *a8, int stride, orb_capsule_t c, uint8_t opa);

/* A pixel of the rim: where it is, how much of it is rim, and the edge's outward normal (x127). */
typedef struct {
    uint16_t x, y;
    uint8_t ring;
    int8_t nx, ny;
} orb_rim_px_t;

/*
 * The rim pixels of a w x h capsule, found once per size so that relighting
 * touches only them. Returns how many there are; with out == NULL, only counts.
 */
int orb_sdf_rim_pixels(orb_rim_px_t *out, int w, int h);

/*
 * Lights the rim: a hairline along the edge plus a specular highlight where
 * the edge faces the light (lx, ly, screen axes, y down; unit length), and a
 * fainter bounce on the far side. `amount` (0..1) is how hard the light hits.
 * Writes only the rim pixels of a8, which is otherwise left as it was (zero).
 */
void orb_sdf_rim_light(uint8_t *a8, int stride, const orb_rim_px_t *px, int n, float lx, float ly, float amount);

/* Fixed-point fused mask preparation. Both buffers must be zeroed; pixels
 * needs at least 8*(w+h) records. Returns record count, -1 on capacity failure.
 * Caster/cover are concentric capsules; off_y and sigma are integer pixels. */
int orb_sdf_masks(uint8_t *dark, int stride, int mw, int mh,
                  orb_rim_px_t *pixels, int capacity, orb_capsule_t caster,
                  orb_capsule_t cover, int off_y, int sigma, uint8_t shadow_opa,
                  uint8_t edge_opa);
