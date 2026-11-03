#include "lvgl.h"
#include "draw/sw/blend/lv_draw_sw_blend_private.h"
#include "orb_simd_copy.h"
#include <string.h>

/* Espressif's esp_lvgl_port SIMD routines (Apache-2.0), in this directory. */
typedef struct {
    uint32_t opa;
    void *dst_buf;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t dst_stride;
    const void *src_buf;
    uint32_t src_stride;
    const lv_opa_t *mask_buf;
    uint32_t mask_stride;
} asm_dsc_t;

extern int lv_color_blend_to_rgb565_esp(asm_dsc_t *dsc);
extern int lv_rgb565_blend_normal_to_rgb565_esp(asm_dsc_t *dsc);

void orb_simd_copy_rgb565(void *destination, const void *source, size_t pixels)
{
    if (!pixels) return;
    uint8_t *output = destination;
    const uint8_t *input = source;
    if (((uintptr_t)output | (uintptr_t)input) & 1) {
        memcpy(output, input, pixels * 2);
        return;
    }
    /* The shared kernel's general scalar fragments use 32-bit accesses.
     * Halfword-aligned iris edges would otherwise incur alignment exceptions.
     * Peel those edges here and send only complete vector blocks to its
     * aligned-destination / arbitrary-source SIMD path. */
    while (pixels && ((uintptr_t)output & 15)) {
        output[0] = input[0]; output[1] = input[1];
        output += 2; input += 2; pixels--;
    }
    size_t block = pixels & ~(size_t)7;
    asm_dsc_t dsc = {
        .dst_buf = output, .dst_w = block, .dst_h = 1,
        .dst_stride = block * 2, .src_buf = input, .src_stride = block * 2,
    };
    if (block) lv_rgb565_blend_normal_to_rgb565_esp(&dsc);
    output += block * 2; input += block * 2; pixels -= block;
    while (pixels--) {
        output[0] = input[0]; output[1] = input[1];
        output += 2; input += 2;
    }
}

lv_result_t orb_blend_fill_rgb565_swapped(lv_draw_sw_blend_fill_dsc_t *dsc)
{
    /* The fill packs the colour as 5-6-5 itself. Every 16-bit value splits into
     * 5-6-5 fields, so hand it the colour whose packing is the swapped value. */
    uint16_t c = lv_color_to_u16(dsc->color);
    uint16_t v = (uint16_t)(c << 8 | c >> 8);
    lv_color_t enc = { .red = (v >> 8) & 0xF8, .green = ((v >> 5) & 0x3F) << 2, .blue = (v & 0x1F) << 3 };
    asm_dsc_t a = {
        .dst_buf = dsc->dest_buf,
        .dst_w = dsc->dest_w,
        .dst_h = dsc->dest_h,
        .dst_stride = dsc->dest_stride,
        .src_buf = &enc,
    };
    return lv_color_blend_to_rgb565_esp(&a);
}

lv_result_t orb_blend_copy_rgb565(lv_draw_sw_blend_image_dsc_t *dsc)
{
    /* Opaque, unmasked: a copy, whatever the byte order. */
    asm_dsc_t a = {
        .dst_buf = dsc->dest_buf,
        .dst_w = dsc->dest_w,
        .dst_h = dsc->dest_h,
        .dst_stride = dsc->dest_stride,
        .src_buf = dsc->src_buf,
        .src_stride = dsc->src_stride,
    };
    return lv_rgb565_blend_normal_to_rgb565_esp(&a);
}
