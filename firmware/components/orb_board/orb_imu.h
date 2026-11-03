#pragma once

/*
 * The motion sensor's numbers, as plain C (no ESP-IDF), tested on the host:
 * QMI8658 accelerometer samples to g, and the sensor's axes to the screen's.
 */

#include <stdint.h>

#define ORB_IMU_LSB_PER_G_4G 8192.0f   /* QMI8658 at +-4 g (datasheet: 16-bit output) */
#define ORB_IMU_LSB_PER_DPS_512 64.0f

/* AX, AY, AZ as the sensor sends them (little-endian int16, CTRL1 BE = 0) to g. */
void orb_imu_decode(const uint8_t raw[6], float lsb_per_g, float g[3]);

/*
 * Sensor axes to screen axes (+x right, +y up, +z out of the screen). On the
 * 1.75C the sensor sits turned half a turn about z from the screen.
 * Output is specific force, not downhill gravity: face up reads +z = 1 g,
 * upright reads +y = 1 g, and right side down reads x < 0.
 */
void orb_imu_to_screen(const float sensor[3], float screen[3]);
