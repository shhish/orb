/*
 * AXP2101 power chip: battery and USB status, unused rails off, soft power-off.
 *
 * Adapted from Meta's Muse Gadgets SDK (esp32/components/muse/muse_pmu.c),
 * Copyright (c) Meta Platforms, Inc. and affiliates, Apache License 2.0.
 */
#include "axp2101.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "axp2101";

#define AXP2101_ADDR 0x34

#define REG_STATUS1 0x00        /* bit5 VBUS good, bit3 battery present */
#define REG_STATUS2 0x01        /* bits[6:5] 01 = charging */
#define REG_COMMON_CFG 0x10     /* bit0 = soft power-off */
#define REG_CHARGER_CONTROL 0x18 /* bit1 cell charge enable, bit3 gauge */
#define REG_IRQ_LEVEL 0x27      /* bits[3:2] power-key hold-to-off time */
#define REG_ADC_ENABLE 0x30     /* bits0..4 VBAT,TS,VBUS,VSYS,die temperature */
#define REG_VBAT_H 0x34         /* bits[5:0]; 1 mV per count with REG_VBAT_L */
#define REG_VBAT_L 0x35
#define REG_INTEN1 0x40         /* three enable registers, then three status */
#define REG_INTSTS1 0x48
#define REG_DCDC_ONOFF 0x80     /* bits[4:0] = DCDC5..1 */
#define REG_LDO_ONOFF0 0x90     /* ALDO1-4, BLDO1-2, CPUSLDO, DLDO1 from bit0 */
#define REG_LDO_ONOFF1 0x91     /* bit0 = DLDO2 */
#define REG_BAT_PERCENT 0xA4
#define REG_TS_CONTROL 0x50
#define REG_CHARGE_CURRENT 0x62
#define REG_CHARGE_VOLTAGE 0x64

/* Holding PWR this long cuts power in hardware, even with the firmware wedged. */
#define PKEY_OFF_10S (3u << 2)

static i2c_master_dev_handle_t s_dev;
static orb_battery_desk_t s_desk;
static bool s_charge_suspended;

static esp_err_t save_desk_settings(bool enabled, bool suspended)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("orb_board", NVS_READWRITE, &nvs), TAG, "desk settings");
    esp_err_t err = nvs_set_u8(nvs, "desk80", enabled);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "desk_owned", suspended);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 50);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

esp_err_t axp2101_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");

    uint8_t v;
    ESP_RETURN_ON_ERROR(rd(REG_IRQ_LEVEL, &v), TAG, "not responding");
    ESP_RETURN_ON_ERROR(wr(REG_IRQ_LEVEL, (v & ~0x0C) | PKEY_OFF_10S), TAG, "set off time");

    /* PWR is read on GPIO 3 and the IRQ line isn't wired, so no interrupts:
     * a pending one would only hold the line low. */
    for (int i = 0; i < 3; i++) {
        ESP_RETURN_ON_ERROR(wr(REG_INTEN1 + i, 0), TAG, "disable irqs");
        ESP_RETURN_ON_ERROR(wr(REG_INTSTS1 + i, 0xFF), TAG, "clear irqs");
    }

    ESP_RETURN_ON_ERROR(rd(REG_ADC_ENABLE, &v), TAG, "read adc enable");
    /* Enable all documented telemetry channels without changing the TS source
     * or temperature protection thresholds configured by the board vendor. */
    ESP_RETURN_ON_ERROR(wr(REG_ADC_ENABLE, v | 0x1f), TAG, "enable telemetry ADC");
    nvs_handle_t nvs;
    if (nvs_open("orb_board", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t enabled = 0, owned = 0;
        nvs_get_u8(nvs, "desk80", &enabled);
        nvs_get_u8(nvs, "desk_owned", &owned);
        nvs_close(nvs);
        s_desk.enabled = enabled != 0;
        if (owned) {
            /* The PMU survives MCU resets. Restore only a suspension whose
             * ownership was committed before the previous PMU write. */
            ESP_RETURN_ON_ERROR(rd(REG_CHARGER_CONTROL, &v), TAG, "restore desk charger");
            ESP_RETURN_ON_ERROR(wr(REG_CHARGER_CONTROL, v | 0x02), TAG, "resume after reset");
            ESP_RETURN_ON_ERROR(save_desk_settings(s_desk.enabled, false), TAG, "clear desk ownership");
        }
    }
    return ESP_OK;
}

esp_err_t axp2101_keep_rails(uint8_t dcdc, uint16_t ldo)
{
    uint8_t dc, ldo0, ldo1;
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_ERROR(rd(REG_DCDC_ONOFF, &dc), TAG, "read dcdc");
    ESP_RETURN_ON_ERROR(rd(REG_LDO_ONOFF0, &ldo0), TAG, "read ldo0");
    ESP_RETURN_ON_ERROR(rd(REG_LDO_ONOFF1, &ldo1), TAG, "read ldo1");
    /* The DCDC register's top bits aren't enables; leave them. */
    ESP_RETURN_ON_ERROR(wr(REG_DCDC_ONOFF, dc & (0xE0 | (dcdc & 0x1F))), TAG, "dcdc off");
    ESP_RETURN_ON_ERROR(wr(REG_LDO_ONOFF0, ldo0 & (ldo & 0xFF)), TAG, "ldo0 off");
    ESP_RETURN_ON_ERROR(wr(REG_LDO_ONOFF1, ldo & 0x100 ? ldo1 : ldo1 & ~0x01), TAG, "ldo1 off");
    return ESP_OK;
}

esp_err_t axp2101_read(orb_power_t *out)
{
    uint8_t s1, s2, pct, hi, lo;
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_ERROR(rd(REG_STATUS1, &s1), TAG, "status1");
    ESP_RETURN_ON_ERROR(rd(REG_STATUS2, &s2), TAG, "status2");
    bool battery = s1 & (1u << 3);
    out->usb = s1 & (1u << 5);
    out->charging = battery && ((s2 >> 5) & 0x3) == 0x1;
    out->battery_pct = -1;
    out->battery_mv = 0;
    if (battery && rd(REG_BAT_PERCENT, &pct) == ESP_OK && pct <= 100) {
        out->battery_pct = pct;
    }
    if (battery && rd(REG_VBAT_H, &hi) == ESP_OK && rd(REG_VBAT_L, &lo) == ESP_OK) {
        out->battery_mv = (hi & 0x3F) << 8 | lo;
    }
    return ESP_OK;
}

esp_err_t axp2101_battery_detail(orb_battery_detail_t *out)
{
    ESP_RETURN_ON_FALSE(s_dev && out, ESP_ERR_INVALID_ARG, TAG, "battery arguments");
    orb_battery_registers_t r = {0};
    ESP_RETURN_ON_ERROR(rd(REG_STATUS1, &r.status1), TAG, "detail status1");
    ESP_RETURN_ON_ERROR(rd(REG_STATUS2, &r.status2), TAG, "detail status2");
    ESP_RETURN_ON_ERROR(rd(REG_BAT_PERCENT, &r.percent), TAG, "detail percent");
    ESP_RETURN_ON_ERROR(rd(REG_CHARGER_CONTROL, &r.charger_control), TAG, "charger enable");
    ESP_RETURN_ON_ERROR(rd(REG_ADC_ENABLE, &r.adc_enable), TAG, "ADC enable");
    ESP_RETURN_ON_ERROR(rd(REG_TS_CONTROL, &r.ts_control), TAG, "TS config");
    ESP_RETURN_ON_ERROR(rd(REG_CHARGE_CURRENT, &r.charge_current), TAG, "charge current setting");
    ESP_RETURN_ON_ERROR(rd(REG_CHARGE_VOLTAGE, &r.charge_voltage), TAG, "charge voltage setting");
    /* Datasheet V1.4 section 6.10: read the high six bits before the low
     * byte to latch each 14-bit measurement. Never use AXP192 current regs. */
    for (unsigned i = 0; i < sizeof(r.adc); i++)
        ESP_RETURN_ON_ERROR(rd(REG_VBAT_H + i, r.adc + i), TAG, "battery ADC");
    orb_battery_parse(&r, out);
    out->desk_limit_enabled = s_desk.enabled;
    out->desk_limit_active = s_charge_suspended;
    return ESP_OK;
}

static esp_err_t resume_desk_charge(void)
{
    if (!s_charge_suspended) return ESP_OK;
    uint8_t control;
    ESP_RETURN_ON_ERROR(rd(REG_CHARGER_CONTROL, &control), TAG, "read charger for resume");
    ESP_RETURN_ON_ERROR(wr(REG_CHARGER_CONTROL, control | 0x02), TAG, "resume desk charge");
    ESP_RETURN_ON_ERROR(save_desk_settings(s_desk.enabled, false), TAG, "clear desk suspension");
    s_charge_suspended = false;
    return ESP_OK;
}

esp_err_t axp2101_set_desk_charge_limit(bool enabled)
{
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    if (enabled == s_desk.enabled) return ESP_OK;
    if (!enabled) ESP_RETURN_ON_ERROR(resume_desk_charge(), TAG, "release desk limit");
    ESP_RETURN_ON_ERROR(save_desk_settings(enabled, false), TAG, "save desk limit");
    s_desk = (orb_battery_desk_t){.enabled = enabled};
    return ESP_OK;
}

bool axp2101_desk_charge_limit(void)
{
    return s_desk.enabled;
}

esp_err_t axp2101_desk_poll(bool resting, uint64_t now)
{
    if (!s_desk.enabled && !s_charge_suspended) return ESP_OK;
    orb_power_t p;
    ESP_RETURN_ON_ERROR(axp2101_read(&p), TAG, "desk battery status");
    bool suspend = orb_battery_desk_update(&s_desk, p.usb, resting, p.battery_pct, now);
    if (!suspend) return resume_desk_charge();
    uint8_t control;
    ESP_RETURN_ON_ERROR(rd(REG_CHARGER_CONTROL, &control), TAG, "read charger for desk limit");
    if (control & 0x02) {
        if (!s_charge_suspended)
            ESP_RETURN_ON_ERROR(save_desk_settings(s_desk.enabled, true), TAG, "record desk suspension");
        /* An I2C timeout may occur after the write reached the PMU. Retain
         * ownership on failure and read/retry the register on the next poll. */
        s_charge_suspended = true;
        ESP_RETURN_ON_ERROR(wr(REG_CHARGER_CONTROL, control & ~0x02), TAG, "pause desk charge");
    }
    return ESP_OK;
}

esp_err_t axp2101_power_off(void)
{
    uint8_t v;
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_ERROR(rd(REG_COMMON_CFG, &v), TAG, "read common cfg");
    ESP_LOGI(TAG, "powering off");
    return wr(REG_COMMON_CFG, v | 0x01);
}
