#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lvgl.h"
#include "src/core/lv_obj_draw_private.h"

/* Caller owns the UI lock and has drained every user of scratch. This shared
 * device/simulator seam performs cold drawing only in the existing internal
 * band; PSRAM receives sequential copies after all draw tasks finish. */
static inline bool orb_display_snapshot_bands(lv_obj_t *object, lv_draw_buf_t *destination,
    void *scratch, size_t scratch_bytes,
    int32_t (*guard)(int32_t y, int32_t y_last, int32_t max_rows))
{
    if (!object || !destination || !scratch || !destination->data ||
        destination->header.cf != LV_COLOR_FORMAT_RGB565 || scratch_bytes > UINT32_MAX) return false;
    lv_obj_update_layout(object);
    lv_area_t bounds;
    lv_obj_get_coords(object, &bounds);
    int32_t ext = lv_obj_get_ext_draw_size(object);
    lv_area_increase(&bounds, ext, ext);
    int32_t width = lv_area_get_width(&bounds), height = lv_area_get_height(&bounds);
    if (width <= 0 || height <= 0 || destination->header.w != (uint32_t)width ||
        destination->header.h != (uint32_t)height || destination->header.stride < (uint32_t)width * 2u ||
        (uint64_t)destination->header.stride * height > destination->data_size) return false;
    uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)width, LV_COLOR_FORMAT_RGB565);
    if (!stride || scratch_bytes < stride) return false;
    size_t capacity_rows = scratch_bytes / stride;
    for (int32_t y = bounds.y1; y <= bounds.y2;) {
        int32_t remaining = bounds.y2 - y + 1;
        int32_t rows = capacity_rows < (size_t)remaining ? (int32_t)capacity_rows : remaining;
        if (guard) {
            int32_t guarded = guard(y, bounds.y2, rows);
            if (guarded <= 0 || guarded > rows) return false;
            rows = guarded;
        }
        lv_draw_buf_t band;
        if (lv_draw_buf_init(&band, (uint32_t)width, (uint32_t)rows, LV_COLOR_FORMAT_RGB565,
            stride, scratch, (uint32_t)scratch_bytes) != LV_RESULT_OK) return false;
        lv_area_t area = {bounds.x1, y, bounds.x2, y + rows - 1};
        if (lv_snapshot_take_area_to_draw_buf(object, LV_COLOR_FORMAT_RGB565, &area, &band) != LV_RESULT_OK)
            return false;
        for (int32_t row = 0; row < rows; row++) {
            memcpy(destination->data + (size_t)(y - bounds.y1 + row) * destination->header.stride,
                band.data + (size_t)row * band.header.stride, (size_t)width * 2u);
        }
        y += rows;
    }
    return true;
}
