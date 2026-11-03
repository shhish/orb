#pragma once

/*
 * The fake board's window: the panel at one pixel per point, the mouse as the
 * finger, and keys and drags that tilt the device.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* Opens the window and a touch input on disp; 's' saves screenshots into out_dir. */
bool sim_window_open(lv_display_t *disp, const char *out_dir);

/* Waits up to ms for input and handles it. False once the window is closed. */
bool sim_window_poll(uint32_t ms);

/* Shows the frame buffer. */
void sim_window_present(void);

/* The tilt the keys and the mouse ask for, in degrees: see sim_board_gravity(). */
void sim_window_tilt(float *right_deg, float *down_deg);

void sim_window_close(void);
