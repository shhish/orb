#pragma once

#include "driver/i2c_master.h"
#include "orb_board.h"

esp_err_t qmi8658_init(i2c_master_bus_handle_t bus);

/* Raw sensor axes, in g. */
esp_err_t qmi8658_accel(orb_accel_t *out);

esp_err_t qmi8658_motion(orb_motion_sample_t *out);
esp_err_t qmi8658_configure(bool active, bool taps);
orb_board_imu_mode_t qmi8658_mode(void);
uint32_t qmi8658_generation(void);
