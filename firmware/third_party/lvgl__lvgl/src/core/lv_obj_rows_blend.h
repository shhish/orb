/* Exact RGB565 row arithmetic. Inputs to divide_255 are at most 65152;
 * the fold replaces expensive division/multiply-high code in the S3 loop. */
#ifndef LV_OBJ_ROWS_BLEND_H
#define LV_OBJ_ROWS_BLEND_H

#include <stdint.h>

static inline unsigned lv_obj_rows_divide_255(unsigned value)
{
    return (value + 1 + (value >> 8)) >> 8;
}

static inline unsigned lv_obj_rows_mul255(unsigned a, unsigned b)
{
    return lv_obj_rows_divide_255(a * b + 127);
}

static inline uint16_t lv_obj_rows_blend565(uint16_t dst, uint16_t src, unsigned a)
{
    if(a == 255) return src;
    if(a == 0) return dst;
    unsigned z = 255 - a;
    return (uint16_t)((lv_obj_rows_divide_255((src >> 11) * a + (dst >> 11) * z + 127) << 11) |
           (lv_obj_rows_divide_255(((src >> 5) & 63) * a + ((dst >> 5) & 63) * z + 127) << 5) |
           lv_obj_rows_divide_255((src & 31) * a + (dst & 31) * z + 127));
}

#endif
