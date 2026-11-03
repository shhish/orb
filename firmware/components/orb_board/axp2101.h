#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "orb_board.h"

esp_err_t axp2101_init(i2c_master_bus_handle_t bus);

/* Turns off every rail but these, by enable bit: dcdc has DCDC1-5 in bits
 * 0-4; ldo has ALDO1-4, BLDO1-2, CPUSLDO, DLDO1 in bits 0-7 and DLDO2 in
 * bit 8. Turns nothing on. */
esp_err_t axp2101_keep_rails(uint8_t dcdc, uint16_t ldo);

esp_err_t axp2101_read(orb_power_t *out);
esp_err_t axp2101_battery_detail(orb_battery_detail_t *out);
esp_err_t axp2101_set_desk_charge_limit(bool enabled);
bool axp2101_desk_charge_limit(void);
esp_err_t axp2101_desk_poll(bool resting, uint64_t now_ms);

esp_err_t axp2101_power_off(void);
