#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool present, usb, charging, discharging, thermal_regulation;
    bool charger_enabled, gauge_enabled, ts_enabled, ts_is_temperature;
    bool current_valid, temperature_valid, health_valid;
    bool desk_limit_enabled, desk_limit_active;
    int battery_pct, battery_mv, vbus_mv, system_mv;
    int charge_current_setting_ma, charge_voltage_setting_mv;
    int charge_current_ma, discharge_current_ma, health_pct; /* -1 unavailable */
    int ts_uv, ts_source_ua, ntc_ohms, die_temperature_raw;
    float temperature_c; /* NAN until a verified thermistor curve is supplied */
} orb_battery_detail_t;

/* AXP2101 public ADC values, high register first. Status/config values are
 * kept separate so host tests exercise the production parser byte for byte. */
typedef struct {
    uint8_t status1, status2, percent, charger_control, adc_enable, ts_control;
    uint8_t charge_current, charge_voltage;
    uint8_t adc[10]; /* 0x34..0x3d: VBAT, TS, VBUS, VSYS, die temperature */
} orb_battery_registers_t;

void orb_battery_parse(const orb_battery_registers_t *registers, orb_battery_detail_t *out);
/* The schematic specifies RP4=10k NTC, but not its beta. Never manufacture
 * a battery temperature from a guessed thermistor curve. */
bool orb_battery_temperature(orb_battery_detail_t *detail, float r25_ohms, float beta_kelvin);

typedef struct {
    bool enabled, timing, active;
    uint64_t desk_since_ms;
} orb_battery_desk_t;

/* Pure policy: 2 h continuously resting on VBUS, 80% cut, 75% resume.
 * The PMU driver applies a changed result with a read/modify/write of charge
 * enable, preserving fuel gauge, watchdog and backup-cell configuration. */
bool orb_battery_desk_update(orb_battery_desk_t *state, bool usb, bool resting,
                            int battery_pct, uint64_t now_ms);
