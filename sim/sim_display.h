#pragma once

/*
 * The panel, faked: LVGL renders it exactly as orb_display does on the device
 * (bands of the same size in the same color format, cut to the circle by the
 * same geometry and kept clear of glass by the same guard), and the bands land
 * in a frame buffer standing in for the panel's memory.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define SIM_D 466   /* the panel, square, with a round glass */

/* After lv_init(). */
lv_display_t *sim_display_create(void);

/* True once if a band has landed since the last call. */
bool sim_display_take_changed(void);

/* The shell's certificate must end before live rendering or owner teardown. */
bool sim_display_snapshot_transition_active(void);

/* The screen as the round panel shows it, RGB888, SIM_D x SIM_D. */
void sim_display_rgb(uint8_t *rgb);

/* Saves the screen as a PNG. False on failure, which it reports. */
bool sim_display_save_png(const char *path);
