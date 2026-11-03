/* ORB PATCH: exact cold glyph resampling. Each caller supplies a Q8 footprint
 * no larger than 512 on either axis (the >=136/256 atlas scale contract). */
#ifndef LV_OBJ_ROWS_AVERAGE_H
#define LV_OBJ_ROWS_AVERAGE_H

#include <stdint.h>

static inline unsigned lv_obj_rows_average_a8(const uint8_t * source, unsigned stride,
                                             unsigned x0, unsigned x1, unsigned y0, unsigned y1)
{
    /* 255*512*512 < 2^26, including white coverage. Keeping the exact sum
     * native-width removes software 64-bit division on the ESP32-S3. */
    uint32_t sum = 0;
    for(unsigned sy = y0 / 256; sy < (y1 + 255) / 256; sy++) {
        unsigned wy = LV_MIN(y1, (sy + 1) * 256) - LV_MAX(y0, sy * 256);
        for(unsigned sx = x0 / 256; sx < (x1 + 255) / 256; sx++) {
            unsigned wx = LV_MIN(x1, (sx + 1) * 256) - LV_MAX(x0, sx * 256);
            sum += (uint32_t)source[sy * stride + sx] * wx * wy;
        }
    }
    unsigned denominator = (x1 - x0) * (y1 - y0);
    return (sum + denominator / 2) / denominator;
}

#endif
