#pragma once

/*
 * The fake board's physics, as plain C (no LVGL, no SDL) so it is tested on
 * the host: the motion sensor's reading for a tilt, and what the round panel
 * shows of a frame.
 */

#include <stdint.h>

/*
 * The accelerometer of a device lying face up, its right edge dipped
 * `right_deg` degrees (negative: the left edge) and its bottom edge
 * `down_deg` (negative: the top edge), in g on screen axes as
 * orb_ui_set_gravity() takes it: +x right, +y up, +z out of the screen,
 * pointing up as the sensor reads it at rest.
 */
void sim_board_gravity(float right_deg, float down_deg, float g[3]);

/*
 * How much of pixel (x, y) a round panel of diameter d shows, 0..255: about
 * the share of the pixel inside the circle, as the bezel cuts it.
 */
uint8_t sim_board_cover(int x, int y, int d);

/*
 * n pixels of the panel's format (RGB565, high byte first, as the CO5300
 * takes it) to RGB888, each scaled by its cover (cover may be NULL: all shown).
 */
void sim_board_to_rgb(const uint8_t *px, const uint8_t *cover, uint8_t *rgb, int n);
