/*
 * The device's display path (components/orb_display) minus the wire: same
 * band buffers, color format, band hook, guard and window alignment, so what
 * the band split does to the UI (glass blur that only sees its band, columns
 * outside the circle never drawn) shows here too. A flushed band is copied
 * into the frame buffer at once, where the device would DMA it to the panel.
 */
#include "sim_display.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "orb_display_geom.h"
#include "orb_display_snapshot.h"
#include "orb_ui.h"
#include "sim_board.h"
#include "src/libs/lodepng/lodepng.h"
#include "src/display/lv_display_private.h"

#define BAND_ROWS 84   /* as orb_display.c */
#define BAND_BYTES (SIM_D * BAND_ROWS * 2)

/* Aligned as orb_display allocates them (for the DMA there). */
static uint8_t s_buf1[BAND_BYTES] __attribute__((aligned(64)));
static uint8_t s_buf2[BAND_BYTES] __attribute__((aligned(64)));
static uint8_t s_fb[SIM_D * SIM_D * 2];   /* the panel's memory: RGB565, high byte first */
static uint8_t s_cover[SIM_D * SIM_D];
static int16_t s_half_chord[SIM_D];
static bool s_changed;
static bool s_snapshot_transition;

bool orb_display_te_pacing_active(void) { return false; }
uint32_t orb_display_panel_period_us(void) { return 0; }
/* Simulator owners already dispatch LVGL continuously; there is no parked
 * device task to notify for an explicitly queued developer callback. */
void orb_display_wake_service(void) {}

bool orb_display_refresh_pending(void)
{
    lv_display_t *display = lv_display_get_default();
    return display && display->inv_p > 0;
}

bool orb_display_capture_snapshot(lv_obj_t *object, lv_draw_buf_t *destination)
{
    /* Simulator flushes synchronously, so its existing first band is already
     * retired. The same region/guard/copy loop runs on the device. */
    lv_draw_wait_for_finish();
    return orb_display_snapshot_bands(object, destination, s_buf1, BAND_BYTES, orb_glass_band_guard);
}

bool sim_display_snapshot_transition_active(void)
{
    return s_snapshot_transition;
}

void orb_display_set_snapshot_transition(bool active)
{
    s_snapshot_transition = active;
    lv_display_t *display = lv_display_get_default();
    if (display) lv_display_set_buffers(display, s_buf1, s_buf2,
        active ? BAND_BYTES / 2 : BAND_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = lv_area_get_width(area);
    for (int32_t y = area->y1; y <= area->y2; y++) {
        memcpy(&s_fb[(y * SIM_D + area->x1) * 2], px_map, (size_t)w * 2);
        px_map += w * 2;
    }
    s_changed = true;
    lv_display_flush_ready(disp);
}

static void band_cb(lv_display_t *disp, lv_area_t *band, int32_t y_last, uint32_t buf_px)
{
    orb_geom_band(s_half_chord, SIM_D, band->y1, y_last, band->x1, band->x2, buf_px,
                  s_snapshot_transition ? NULL : orb_glass_band_guard,
                  &band->y2, &band->x1, &band->x2);
}

/* The CO5300 takes only even-aligned windows; the band choice relies on it. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

lv_display_t *sim_display_create(void)
{
    s_snapshot_transition = false;
    orb_geom_half_chords(s_half_chord, SIM_D);
    for (int y = 0; y < SIM_D; y++) {
        for (int x = 0; x < SIM_D; x++) {
            s_cover[y * SIM_D + x] = sim_board_cover(x, y, SIM_D);
        }
    }

    lv_display_t *disp = lv_display_create(SIM_D, SIM_D);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(disp, s_buf1, s_buf2, BAND_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_band_cb(disp, band_cb);
    lv_display_set_tile_cnt(disp, 1);
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);
    return disp;
}

bool sim_display_take_changed(void)
{
    bool c = s_changed;
    s_changed = false;
    return c;
}

void sim_display_rgb(uint8_t *rgb)
{
    sim_board_to_rgb(s_fb, s_cover, rgb, SIM_D * SIM_D);
}

bool sim_display_save_png(const char *path)
{
    static uint8_t rgb[SIM_D * SIM_D * 3];
    sim_display_rgb(rgb);
    unsigned char *png = NULL;
    size_t size = 0;
    /* Keep screenshots RGB8. Some image preview clients misrender indexed PNGs
     * by treating black palette entries as missing regions; the panel pixels
     * and independent decoders were correct. Explicit RGB preserves the same
     * pixels without the palette-dependent presentation ambiguity. */
    LodePNGState state;
    lodepng_state_init(&state);
    state.encoder.auto_convert = 0;
    state.info_raw.colortype = LCT_RGB;
    state.info_raw.bitdepth = 8;
    state.info_png.color.colortype = LCT_RGB;
    state.info_png.color.bitdepth = 8;
    unsigned err = lodepng_encode(&png, &size, rgb, SIM_D, SIM_D, &state);
    lodepng_state_cleanup(&state);
    if (err) {
        fprintf(stderr, "orb_sim: PNG: %s\n", lodepng_error_text(err));
        lv_free(png);
        return false;
    }
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(png, 1, size, f) == size;
    if (f && fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "orb_sim: %s: %s\n", path, strerror(errno));
    }
    lv_free(png);
    return ok;
}
