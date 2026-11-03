/**
 * @file lv_draw_sw_blend.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "../../../misc/lv_area_private.h"
#include "lv_draw_sw_blend_private.h"
#include "../../lv_draw_private.h"
#if LV_DRAW_SW_SUPPORT_A8
    #include "lv_draw_sw_blend_to_a8.h"
#endif
#if LV_DRAW_SW_SUPPORT_L8
    #include "lv_draw_sw_blend_to_l8.h"
#endif
#if LV_DRAW_SW_SUPPORT_AL88
    #include "lv_draw_sw_blend_to_al88.h"
#endif
#if LV_DRAW_SW_SUPPORT_RGB565
    #include "lv_draw_sw_blend_to_rgb565.h"
#endif
#if LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
    #include "lv_draw_sw_blend_to_rgb565_swapped.h"
#endif
#if LV_DRAW_SW_SUPPORT_ARGB8888
    #include "lv_draw_sw_blend_to_argb8888.h"
#endif
#if LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED
    #include "lv_draw_sw_blend_to_argb8888_premultiplied.h"
#endif
#if LV_DRAW_SW_SUPPORT_RGB888 || LV_DRAW_SW_SUPPORT_XRGB8888
    #include "lv_draw_sw_blend_to_rgb888.h"
#endif
#if LV_DRAW_SW_SUPPORT_I1
    #include "lv_draw_sw_blend_to_i1.h"
#endif
#if LV_USE_DRAW_SW

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static inline void /* LV_ATTRIBUTE_FAST_MEM */ lv_draw_sw_blend_color(lv_color_format_t layer_cf,
                                                                      lv_draw_sw_blend_fill_dsc_t * fill_dsc);

#if LV_DRAW_SW_SUPPORT_RGB565 || LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
static void blend_neutral_shadow(lv_draw_sw_blend_fill_dsc_t * dsc, bool swapped);
#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
static void blend_sparse_a8(lv_draw_sw_blend_fill_dsc_t * dsc, const uint16_t * spans,
                            int32_t offset_x, bool swapped, bool neutral);
#endif
#endif

static inline void /* LV_ATTRIBUTE_FAST_MEM */ lv_draw_sw_blend_image(lv_color_format_t layer_cf,
                                                                      lv_draw_sw_blend_image_dsc_t * image_dsc);


/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_sw_blend(lv_draw_task_t * t, const lv_draw_sw_blend_dsc_t * dsc)
{
    /*Do not draw transparent things*/
    if(dsc->opa <= LV_OPA_MIN) return;
    if(dsc->mask_buf && dsc->mask_res == LV_DRAW_SW_MASK_RES_TRANSP) return;

    lv_area_t blend_area;
    if(!lv_area_intersect(&blend_area, dsc->blend_area, &t->clip_area)) return;

    LV_PROFILER_DRAW_BEGIN;
    lv_layer_t * layer = t->target_layer;
    uint32_t layer_stride_byte = layer->draw_buf->header.stride;

    lv_draw_sw_blend_handler_t handler = lv_draw_sw_get_blend_handler(layer->color_format);
    if(handler) {
        handler(t, dsc);
        LV_PROFILER_DRAW_END;
        return;
    }

    if(dsc->src_buf == NULL) {
        lv_draw_sw_blend_fill_dsc_t fill_dsc;
        fill_dsc.dest_w = lv_area_get_width(&blend_area);
        fill_dsc.dest_h = lv_area_get_height(&blend_area);
        fill_dsc.dest_stride = layer_stride_byte;
        fill_dsc.opa = dsc->opa;
        fill_dsc.color = dsc->color;
        fill_dsc.mask_stride = 0;

        if(dsc->mask_buf == NULL) fill_dsc.mask_buf = NULL;
        else if(dsc->mask_res == LV_DRAW_SW_MASK_RES_FULL_COVER) fill_dsc.mask_buf = NULL;
        else fill_dsc.mask_buf = dsc->mask_buf;


        fill_dsc.relative_area  = blend_area;
        lv_area_move(&fill_dsc.relative_area, -layer->buf_area.x1, -layer->buf_area.y1);
        fill_dsc.dest_buf = lv_draw_layer_go_to_xy(layer, blend_area.x1 - layer->buf_area.x1,
                                                   blend_area.y1 - layer->buf_area.y1);
        if(fill_dsc.mask_buf) {
            fill_dsc.mask_stride = dsc->mask_stride == 0  ? lv_area_get_width(dsc->mask_area) : dsc->mask_stride;
            fill_dsc.mask_buf += fill_dsc.mask_stride * (blend_area.y1 - dsc->mask_area->y1) +
                                 (blend_area.x1 - dsc->mask_area->x1);
        }

#if LV_DRAW_SW_SUPPORT_RGB565 || LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
        if(dsc->a8_spans && fill_dsc.mask_buf &&
           (layer->color_format == LV_COLOR_FORMAT_RGB565 || layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED)) {
            const uint16_t * spans = dsc->a8_spans + (blend_area.y1 - dsc->mask_area->y1) * 4;
            blend_sparse_a8(&fill_dsc, spans, blend_area.x1 - dsc->mask_area->x1,
                            layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED,
                            dsc->rgb565_neutral_shadow && lv_color_to_u16(dsc->color) == 0);
        }
        else
#endif
        if(dsc->rgb565_neutral_shadow && lv_color_to_u16(dsc->color) == 0 &&
           (layer->color_format == LV_COLOR_FORMAT_RGB565 || layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED)) {
            /*The opt-in path is shared by host and ESP32: opaque-fill SIMD
             *does not implement masked shadows and cannot change the result.*/
            blend_neutral_shadow(&fill_dsc, layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED);
        }
        else
#endif
        lv_draw_sw_blend_color(layer->color_format, &fill_dsc);
    }
    else {
        if(!lv_area_intersect(&blend_area, &blend_area, dsc->src_area)) {
            LV_PROFILER_DRAW_END;
            return;
        }

        if(dsc->mask_area && !lv_area_intersect(&blend_area, &blend_area, dsc->mask_area)) {
            LV_PROFILER_DRAW_END;
            return;
        }

        lv_draw_sw_blend_image_dsc_t image_dsc;
        image_dsc.dest_w = lv_area_get_width(&blend_area);
        image_dsc.dest_h = lv_area_get_height(&blend_area);
        image_dsc.dest_stride = layer_stride_byte;

        image_dsc.opa = dsc->opa;
        image_dsc.blend_mode = dsc->blend_mode;
        image_dsc.src_stride = dsc->src_stride;
        image_dsc.src_color_format = dsc->src_color_format;

        const uint8_t * src_buf = dsc->src_buf;
        uint32_t src_px_size = lv_color_format_get_bpp(dsc->src_color_format);
        src_buf += image_dsc.src_stride * (blend_area.y1 - dsc->src_area->y1);
        src_buf += ((blend_area.x1 - dsc->src_area->x1) * src_px_size) >> 3;
        image_dsc.src_buf = src_buf;
        image_dsc.mask_stride = 0;

        if(dsc->mask_buf == NULL) image_dsc.mask_buf = NULL;
        else if(dsc->mask_res == LV_DRAW_SW_MASK_RES_FULL_COVER) image_dsc.mask_buf = NULL;
        else image_dsc.mask_buf = dsc->mask_buf;

        if(image_dsc.mask_buf) {
            LV_ASSERT_NULL(dsc->mask_area);
            image_dsc.mask_buf = dsc->mask_buf;
            image_dsc.mask_stride = dsc->mask_stride ? dsc->mask_stride : lv_area_get_width(dsc->mask_area);
            image_dsc.mask_buf += image_dsc.mask_stride * (blend_area.y1 - dsc->mask_area->y1) +
                                  (blend_area.x1 - dsc->mask_area->x1);
        }

        image_dsc.relative_area  = blend_area;
        lv_area_move(&image_dsc.relative_area, -layer->buf_area.x1, -layer->buf_area.y1);

        image_dsc.src_area  = *dsc->src_area;
        lv_area_move(&image_dsc.src_area, -layer->buf_area.x1, -layer->buf_area.y1);

        image_dsc.dest_buf = lv_draw_layer_go_to_xy(layer, blend_area.x1 - layer->buf_area.x1,
                                                    blend_area.y1 - layer->buf_area.y1);

        lv_draw_sw_blend_image(layer->color_format, &image_dsc);
    }
    LV_PROFILER_DRAW_END;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#if LV_DRAW_SW_SUPPORT_RGB565 || LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
/* ORB PATCH: no helper call per pixel on size-optimized firmware builds. Repeated flat
 *runs (the long inside edge of a capsule) need only a key comparison.*/
#define NEUTRAL_SHADOW_PIXEL(dest_px, mask_alpha)                                      \
    do {                                                                              \
        uint32_t alpha_ = (mask_alpha);                                                \
        if(opa < LV_OPA_MAX) alpha_ = LV_OPA_MIX2(alpha_, opa);                        \
        uint32_t mix5_ = (alpha_ + 4) >> 3;                                            \
        uint16_t raw_ = (dest_px);                                                    \
        if(!mix5_ || !raw_) break;                                                    \
        uint32_t key_ = ((uint32_t)raw_ << 6) | mix5_;                                \
        if(key_ != last_key) {                                                        \
            uint16_t original_ = swapped ? lv_color_swap_16(raw_) : raw_;             \
            uint32_t r_ = original_ >> 11, g_ = (original_ >> 5) & 63;                \
            uint16_t result_;                                                        \
            if(r_ == (original_ & 31) && (g_ == 2 * r_ || g_ == 2 * r_ + 1)) {       \
                uint32_t level_ = (r_ * (32 - mix5_)) >> 5;                          \
                result_ = (uint16_t)((level_ << 11) |                                \
                          (((level_ << 1) | (level_ >> 4)) << 5) | level_);           \
            }                                                                         \
            else {                                                                    \
                uint32_t bg_ = LV_COLOR_MIX_16_PREPARE(original_);                    \
                uint32_t res_ = (((0u - bg_) * mix5_ >> 5) + bg_) & 0x07E0F81Fu;     \
                result_ = (uint16_t)((res_ >> 16) | res_);                            \
            }                                                                         \
            last_result = swapped ? lv_color_swap_16(result_) : result_;             \
            last_key = key_;                                                          \
        }                                                                             \
        (dest_px) = last_result;                                                       \
    } while(0)

static void blend_neutral_shadow(lv_draw_sw_blend_fill_dsc_t * dsc, bool swapped)
{
    uint8_t * dest = dsc->dest_buf;
    const lv_opa_t * mask = dsc->mask_buf;
    lv_opa_t opa = dsc->opa;
    uint32_t last_key = UINT32_MAX;
    uint16_t last_result = 0;
    int32_t width = dsc->dest_w;
    for(int32_t y = 0; y < dsc->dest_h; y++) {
        uint16_t * row = (uint16_t *)dest;
        int32_t x = 0;
        if(mask) {
            for(; x < width && ((lv_uintptr_t)(mask + x) & 3); x++) {
                NEUTRAL_SHADOW_PIXEL(row[x], mask[x]);
            }
            for(; x <= width - 4; x += 4) {
                /*The outer shadow is mostly transparent. Avoid even reading
                 *the destination on these four pixels, as the usual A8 path.*/
                if(((const lv_draw_sw_word_t *)(mask + x))->u32 == 0) continue;
                NEUTRAL_SHADOW_PIXEL(row[x], mask[x]);
                NEUTRAL_SHADOW_PIXEL(row[x + 1], mask[x + 1]);
                NEUTRAL_SHADOW_PIXEL(row[x + 2], mask[x + 2]);
                NEUTRAL_SHADOW_PIXEL(row[x + 3], mask[x + 3]);
            }
            for(; x < width; x++) NEUTRAL_SHADOW_PIXEL(row[x], mask[x]);
            mask += dsc->mask_stride;
        }
        else {
            for(; x < width; x++) NEUTRAL_SHADOW_PIXEL(row[x], LV_OPA_COVER);
        }
        dest += dsc->dest_stride;
    }
}
#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
/* The immutable SDF span table proves every omitted A8 sample is zero.
 * Keep the same two RGB565 blend formulas as the dense implementations;
 * only the set of visited pixels changes. No destination or mask scratch. */
static void blend_sparse_a8(lv_draw_sw_blend_fill_dsc_t * dsc, const uint16_t * spans,
                            int32_t offset_x, bool swapped, bool neutral)
{
    uint8_t * dest = dsc->dest_buf;
    const lv_opa_t * mask = dsc->mask_buf;
    lv_opa_t opa = dsc->opa;
    uint16_t color16 = lv_color_to_u16(dsc->color);
    uint16_t stored_color = swapped ? lv_color_swap_16(color16) : color16;
    uint32_t fg_prep = LV_COLOR_MIX_16_PREPARE(color16);
    uint32_t last_key = UINT32_MAX;
    uint16_t last_result = 0;
    for(int32_t y = 0; y < dsc->dest_h; y++, spans += 4) {
        uint16_t * row = (uint16_t *)dest;
        for(unsigned i = 0; i < 4; i += 2) {
            int32_t start = LV_MAX(0, (int32_t)spans[i] - offset_x);
            int32_t end = LV_MIN(dsc->dest_w, (int32_t)spans[i + 1] - offset_x);
            if(neutral) {
                for(int32_t x = start; x < end; x++) NEUTRAL_SHADOW_PIXEL(row[x], mask[x]);
            }
            else {
                for(int32_t x = start; x < end; x++) {
                    uint32_t alpha = mask[x];
                    if(opa < LV_OPA_MAX) alpha = LV_OPA_MIX2(alpha, opa);
                    if(!alpha) continue;
                    if(opa >= LV_OPA_MAX && alpha >= LV_OPA_MAX) row[x] = stored_color;
                    else {
                        uint16_t value = swapped ? lv_color_swap_16(row[x]) : row[x];
                        LV_COLOR_MIX_16_TO_16_PREPARED(value, fg_prep, value, alpha);
                        row[x] = swapped ? lv_color_swap_16(value) : value;
                    }
                }
            }
        }
        mask += dsc->mask_stride;
        dest += dsc->dest_stride;
    }
}
#endif
#undef NEUTRAL_SHADOW_PIXEL

#endif

static inline void LV_ATTRIBUTE_FAST_MEM lv_draw_sw_blend_color(lv_color_format_t layer_cf,
                                                                lv_draw_sw_blend_fill_dsc_t * fill_dsc)
{
    switch(layer_cf) {
#if LV_DRAW_SW_SUPPORT_RGB565
        case LV_COLOR_FORMAT_RGB565:
            lv_draw_sw_blend_color_to_rgb565(fill_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
        case LV_COLOR_FORMAT_RGB565_SWAPPED:
            lv_draw_sw_blend_color_to_rgb565_swapped(fill_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_ARGB8888
        case LV_COLOR_FORMAT_ARGB8888:
            lv_draw_sw_blend_color_to_argb8888(fill_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_RGB888
        case LV_COLOR_FORMAT_RGB888:
            lv_draw_sw_blend_color_to_rgb888(fill_dsc, 3);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_XRGB8888
        case LV_COLOR_FORMAT_XRGB8888:
            lv_draw_sw_blend_color_to_rgb888(fill_dsc, 4);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED
        case LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED:
            lv_draw_sw_blend_color_to_argb8888_premultiplied(fill_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_A8
        case LV_COLOR_FORMAT_A8:
            lv_draw_sw_blend_color_to_a8(fill_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_L8
        case LV_COLOR_FORMAT_L8:
            lv_draw_sw_blend_color_to_l8(fill_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_AL88
        case LV_COLOR_FORMAT_AL88:
            lv_draw_sw_blend_color_to_al88(fill_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_I1
        case LV_COLOR_FORMAT_I1:
            lv_draw_sw_blend_color_to_i1(fill_dsc);
            break;
#endif
        default:
            break;
    }
}

static inline void LV_ATTRIBUTE_FAST_MEM lv_draw_sw_blend_image(lv_color_format_t layer_cf,
                                                                lv_draw_sw_blend_image_dsc_t * image_dsc)
{
    switch(layer_cf) {
#if LV_DRAW_SW_SUPPORT_RGB565
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB565A8:
            lv_draw_sw_blend_image_to_rgb565(image_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
        case LV_COLOR_FORMAT_RGB565_SWAPPED:
            lv_draw_sw_blend_image_to_rgb565_swapped(image_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_ARGB8888
        case LV_COLOR_FORMAT_ARGB8888:
            lv_draw_sw_blend_image_to_argb8888(image_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_RGB888
        case LV_COLOR_FORMAT_RGB888:
            lv_draw_sw_blend_image_to_rgb888(image_dsc, 3);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_XRGB8888
        case LV_COLOR_FORMAT_XRGB8888:
            lv_draw_sw_blend_image_to_rgb888(image_dsc, 4);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED
        case LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED:
            lv_draw_sw_blend_image_to_argb8888_premultiplied(image_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_A8
        case LV_COLOR_FORMAT_A8:
            lv_draw_sw_blend_image_to_a8(image_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_L8
        case LV_COLOR_FORMAT_L8:
            lv_draw_sw_blend_image_to_l8(image_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_AL88
        case LV_COLOR_FORMAT_AL88:
            lv_draw_sw_blend_image_to_al88(image_dsc);
            break;
#endif
#if LV_DRAW_SW_SUPPORT_I1
        case LV_COLOR_FORMAT_I1:
            lv_draw_sw_blend_image_to_i1(image_dsc);
            break;
#endif
        default:
            break;
    }
}

#endif
