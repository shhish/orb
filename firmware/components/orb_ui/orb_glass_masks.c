/* Geometry/buffer helpers shared by the bounded glass-cache transaction. */
#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <time.h>
#endif

#include "orb_glass_masks.h"
#include "orb_ui_private.h"

uint64_t orb_glass_masks_clock_us(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
#endif
}

void *orb_glass_masks_buffer_alloc(lv_draw_buf_t *b, int32_t w, int32_t h)
{
    uint8_t *data = orb_ui_alloc((size_t)w * h);
    memset(b, 0, sizeof(*b));
    if (!data) {
        return NULL;
    }
    memset(data, 0, (size_t)w * h);
    /* Marked allocated, like any draw buffer, so LVGL draws it as it is, without its decoders. */
    b->header.magic = LV_IMAGE_HEADER_MAGIC;
    b->header.cf = LV_COLOR_FORMAT_A8;
    b->header.flags = LV_IMAGE_FLAGS_ALLOCATED;
    b->header.w = w;
    b->header.h = h;
    b->header.stride = w;
    b->data = data;
    b->unaligned_data = data;
    b->data_size = w * h;
    b->handlers = lv_draw_buf_get_handlers();
    return data;
}

void orb_glass_masks_buffer_free(lv_draw_buf_t *b)
{
    orb_ui_free(b->data);
    memset(b, 0, sizeof(*b));
}

void orb_glass_masks_light_vector(float degrees, float *lx, float *ly)
{
    float a = degrees * (float)M_PI / 180;
    *lx = cosf(a);
    *ly = sinf(a);
}

void orb_glass_masks_size(int32_t w, int32_t h, int step, int32_t *mw, int32_t *mh)
{
    *mw = w + (int32_t)(w * SWELL * step / (SWELL_STEPS - 1)) / 2 * 2;
    *mh = h + (int32_t)(h * SWELL * step / (SWELL_STEPS - 1)) / 2 * 2;
}
