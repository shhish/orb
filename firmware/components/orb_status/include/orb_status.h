#pragma once

/* Live status symbols. Call on the LVGL thread, with the LVGL lock held.
 * Shapes use LVGL's anti-aliased draw primitives; only digits and the charging
 * bolt come from orb_fonts. See README.md for baseline layout and integration. */
#include "orb_fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int level; /* 0..100, clamped on input */
    bool charging;
    bool low_power;
    bool percentage;
} orb_battery_state_t;

typedef struct {
    uint8_t bars; /* 0..3, clamped on input */
    bool connected;
} orb_wifi_state_t;

/* Noninteractive, transparent objects. Their dimensions follow Dynamic Type and
 * Bold Text. They inherit text_color/text_opa from their parent. */
lv_obj_t *orb_battery_create(lv_obj_t *parent, orb_text_style_t style,
                             orb_text_variant_t variant);
lv_obj_t *orb_wifi_create(lv_obj_t *parent, orb_text_style_t style,
                          orb_text_variant_t variant);

void orb_battery_set_state(lv_obj_t *obj, int level, bool charging, bool low_power);
void orb_battery_set_percentage(lv_obj_t *obj, bool show);
orb_battery_state_t orb_battery_get_state(const lv_obj_t *obj);

void orb_wifi_set_bars(lv_obj_t *obj, int bars, bool connected);
void orb_wifi_set_rssi(lv_obj_t *obj, int rssi_dbm, bool connected);
orb_wifi_state_t orb_wifi_get_state(const lv_obj_t *obj);
/* >= -55 dBm: 3, >= -67: 2, >= -80: 1, below -80: 0.
 * Connection state is independent: a very weak connected AP still has 0 bars. */
uint8_t orb_wifi_bars_for_rssi(int rssi_dbm);

void orb_status_set_text_style(lv_obj_t *obj, orb_text_style_t style,
                               orb_text_variant_t variant);
/* Baseline measured from the widget's top. Set y = text_baseline - this value.
 * The widget's height equals the selected text font's line_height, so placing it
 * at the same y as a single-line label also aligns their baselines. */
int32_t orb_status_baseline(const lv_obj_t *obj);
/* Flat backdrop used by percentage/bolt knock-outs and the disconnected slash.
 * Defaults to black. Change this if the widget sits on an opaque surface. */
void orb_status_set_background(lv_obj_t *obj, lv_color_t color);

#ifdef __cplusplus
}
#endif
