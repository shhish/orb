#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "src/display/lv_display_private.h"

/* Only blur samples neighbouring output pixels in the current renderer.
 * Check every live system layer too: a game cannot certify its own scene
 * while a volume/notification control has appeared above it. */
static inline bool orb_display_object_reads_backdrop(lv_obj_t *object)
{
    /* Hidden ancestry is a safe subtree rejection. Geometry visibility can
     * miss overflow/custom descendants; conservatively reject unhidden blur
     * even when its current coordinates are outside the viewport. */
    if (!object || lv_obj_is_hidden(object)) return false;
    if (lv_obj_get_style_blur_radius(object, LV_PART_MAIN) > 0) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++)
        if (orb_display_object_reads_backdrop(lv_obj_get_child(object, i))) return true;
    return false;
}

static inline bool orb_display_live_bands_independent(lv_display_t *display)
{
    return display && !orb_display_object_reads_backdrop(lv_display_get_screen_active(display)) &&
        !orb_display_object_reads_backdrop(lv_display_get_screen_prev(display)) &&
        !orb_display_object_reads_backdrop(lv_display_get_layer_bottom(display)) &&
        !orb_display_object_reads_backdrop(lv_display_get_layer_top(display)) &&
        !orb_display_object_reads_backdrop(lv_display_get_layer_sys(display));
}

/* Inspect only after LVGL joins its invalidations (RENDER_START). Live regions
 * must fit whole: blindly reducing them to 42-row bands would split backdrop
 * blur and change its pixels. An immutable full-screen snapshot compositor has
 * no neighbourhood reads, so it may split regions after fitting one scanline.
 * The caller fences DMA before changing buffer views. */
static inline bool orb_display_half_buffers_fit_scene(const lv_display_t *disp, size_t half_bytes,
                                                       bool snapshot_transition)
{
    if (!disp || disp->render_mode != LV_DISPLAY_RENDER_MODE_PARTIAL ||
        disp->rotation != LV_DISPLAY_ROTATION_0 || disp->inv_p == 0) return false;

    bool found = false;
    for (uint32_t i = 0; i < disp->inv_p; i++) {
        if (disp->inv_area_joined[i]) continue;
        const lv_area_t *area = &disp->inv_areas[i];
        int32_t width = lv_area_get_width(area), height = lv_area_get_height(area);
        if (width <= 0 || height <= 0) return false;
        uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)width, disp->color_format);
        if (!stride || (uint64_t)stride * (snapshot_transition ? 1u : (uint32_t)height) > half_bytes)
            return false;
        found = true;
    }
    return found;
}

static inline bool orb_display_half_buffers_fit(const lv_display_t *disp, size_t half_bytes)
{
    return orb_display_half_buffers_fit_scene(disp, half_bytes, false);
}
