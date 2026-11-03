/* QMI8658 6-axis motion sensor; tap events are polled because IRQ is unwired. */
#include "qmi8658.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orb_imu.h"

static const char *TAG = "qmi8658";

#define QMI8658_ADDR 0x6B
#define QMI8658_ID 0x05

#define REG_WHO_AM_I 0x00
#define REG_REVISION 0x01
#define REG_CTRL1 0x02          /* bit6 register auto-increment, bit5 big-endian */
#define REG_CTRL2 0x03          /* accel: bits[6:4] full scale, bits[3:0] rate */
#define REG_CTRL3 0x04          /* gyro range and ODR */
#define REG_CTRL7 0x08          /* bit0 accel, bit1 gyro, bit7 sync sample */
#define REG_CTRL8 0x09          /* bit7 polled CTRL9 handshake, bit0 tap enable */
#define REG_CTRL9 0x0A
#define REG_CAL1_L 0x0B
#define REG_STATUSINT 0x2D
#define REG_STATUS1 0x2F
#define REG_AX_L 0x35           /* AX, AY, AZ: little-endian int16 */
#define REG_TAP_STATUS 0x59
#define REG_RESET_DONE 0x4D     /* 0x80 once a reset has completed */
#define REG_RESET 0x60

#define RESET_CMD 0xB0
#define ACCEL_4G_224HZ 0x15     /* 224.2 Hz in 6DOF, 250 Hz accel-only */
#define ACCEL_4G_LOW_POWER_21HZ 0x1D
#define GYRO_512DPS_224HZ 0x55

static i2c_master_dev_handle_t s_dev;
static bool s_tap_enabled;
static bool s_tap_latched;
static orb_board_imu_mode_t s_mode, s_requested_mode;
static bool s_requested_taps, s_rejected_request;
static uint32_t s_generation;
static int64_t s_ready_us, s_retry_us;

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static esp_err_t rd(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, 50);
}

static esp_err_t tap_command(const uint8_t params[8])
{
    /* QMI8658A Rev A sections 5.10.4, 10.3: both sensors disabled,
     * CAL1..CAL4 parameters, command 0x0c, CmdDone, ACK, CmdDone clear.
     * This works without either physical interrupt pin. */
    for (unsigned i = 0; i < 8; i++)
        ESP_RETURN_ON_ERROR(wr(REG_CAL1_L + i, params[i]), TAG, "tap parameter");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL9, 0x0C), TAG, "tap command");
    uint8_t status = 0;
    for (unsigned i = 0; i < 40; i++) {
        ESP_RETURN_ON_ERROR(rd(REG_STATUSINT, &status, 1), TAG, "tap handshake");
        if (status & 0x80) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_RETURN_ON_FALSE(status & 0x80, ESP_ERR_TIMEOUT, TAG, "tap command timeout");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL9, 0), TAG, "tap ack");
    for (unsigned i = 0; i < 40; i++) {
        ESP_RETURN_ON_ERROR(rd(REG_STATUSINT, &status, 1), TAG, "tap ack status");
        if (!(status & 0x80)) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t tap_configure(bool accel_only)
{
    /* 224.2 Hz: peak <=~36ms, quiet >=~90ms, second tap <=~299ms.
     * Z has priority; 0.8g^2 peak and 0.08g^2 quiet thresholds.
     * Case/desk transfer response still needs tuning on the actual enclosure. */
    const uint8_t first[8] = {accel_only ? 9 : 8, 4, accel_only ? 22 : 20, 0,
                            accel_only ? 75 : 67, 0, 0, 1};
    const uint8_t second[8] = {8, 32, 0x33, 0x03, 0x52, 0, 0, 2};
    ESP_RETURN_ON_ERROR(wr(REG_CTRL7, 0), TAG, "disable for tap config");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL8, 0x80), TAG, "polled handshake");
    ESP_RETURN_ON_ERROR(tap_command(first), TAG, "tap timing");
    ESP_RETURN_ON_ERROR(tap_command(second), TAG, "tap thresholds");
    return wr(REG_CTRL8, 0x81);
}

esp_err_t qmi8658_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");

    uint8_t id[2] = { 0 };
    ESP_RETURN_ON_ERROR(rd(REG_WHO_AM_I, id, 1), TAG, "not responding");
    ESP_RETURN_ON_FALSE(id[0] == QMI8658_ID, ESP_ERR_NOT_FOUND, TAG, "unexpected id 0x%02x", id[0]);

    /* The power chip keeps the sensor powered across our resets and reflashes,
     * so whatever the last firmware set up is still in it. Without this, the
     * accelerometer read 0.46 g lying flat although CTRL2 said +-4 g (8192
     * LSB/g, as the datasheet has it): state outside the CTRL registers, such
     * as an offset set through CTRL9 (0x09, accelerometer host delta offset),
     * survives until a reset; with it, the same board read 0.93 g. Reset to
     * the defaults first; it takes at most 15 ms (datasheet §5.9, §7.4). */
    ESP_RETURN_ON_ERROR(wr(REG_RESET, RESET_CMD), TAG, "reset");
    uint8_t done = 0;
    for (int i = 0; i < 10 && done != 0x80; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        rd(REG_RESET_DONE, &done, 1);
    }
    ESP_RETURN_ON_FALSE(done == 0x80, ESP_ERR_TIMEOUT, TAG, "reset didn't complete (0x%02x)", done);

    ESP_RETURN_ON_ERROR(wr(REG_CTRL1, 0x40), TAG, "ctrl1");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL2, ACCEL_4G_224HZ), TAG, "ctrl2");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL3, GYRO_512DPS_224HZ), TAG, "ctrl3");
    esp_err_t tap = tap_configure(false);
    s_tap_enabled = tap == ESP_OK;
    s_tap_latched = false;
    if (!s_tap_enabled) {
        ESP_LOGW(TAG, "tap engine unavailable (%s); guarded sampled fallback", esp_err_to_name(tap));
        ESP_RETURN_ON_ERROR(wr(REG_CTRL9, 0), TAG, "clear tap command");
        ESP_RETURN_ON_ERROR(wr(REG_CTRL8, 0x80), TAG, "disable tap engine");
    }
    ESP_RETURN_ON_ERROR(wr(REG_CTRL7, 0x03), TAG, "ctrl7");
    s_mode = s_requested_mode = ORB_IMU_MODE_ACTIVE_224;
    s_requested_taps = s_tap_enabled;
    s_rejected_request = false;
    s_generation++;
    s_ready_us = esp_timer_get_time() + 200000;
    uint8_t ctrl[9];
    if (rd(REG_REVISION, id + 1, 1) == ESP_OK && rd(REG_CTRL1, ctrl, sizeof(ctrl)) == ESP_OK) {
        ESP_LOGI(TAG, "rev 0x%02x, ctrl1-9: %02x %02x %02x %02x %02x %02x %02x %02x %02x", id[1], ctrl[0], ctrl[1],
                 ctrl[2], ctrl[3], ctrl[4], ctrl[5], ctrl[6], ctrl[7], ctrl[8]);
    }
    return ESP_OK;
}

static esp_err_t configure_mode(orb_board_imu_mode_t mode, bool taps)
{
    ESP_RETURN_ON_ERROR(wr(REG_CTRL7, 0), TAG, "disable for mode change");
    /* Rev A section 7.3: stopping a sensor takes up to 2/ODR. In particular
     * low-power 21 Hz must finish before CTRL9 tap programming can begin. */
    vTaskDelay(pdMS_TO_TICKS(s_mode == ORB_IMU_MODE_ACCEL_21 ? 96 : 10));
    ESP_RETURN_ON_ERROR(wr(REG_CTRL9, 0), TAG, "clear command");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL2, mode == ORB_IMU_MODE_ACCEL_21 ?
                           ACCEL_4G_LOW_POWER_21HZ : ACCEL_4G_224HZ), TAG, "accel mode");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL3, GYRO_512DPS_224HZ), TAG, "gyro rate");
    if (taps) ESP_RETURN_ON_ERROR(tap_configure(mode != ORB_IMU_MODE_ACTIVE_224), TAG, "tap mode");
    else ESP_RETURN_ON_ERROR(wr(REG_CTRL8, 0x80), TAG, "tap off");
    ESP_RETURN_ON_ERROR(wr(REG_CTRL7, mode == ORB_IMU_MODE_ACTIVE_224 ? 0x03 : 0x01), TAG, "enable mode");
    s_mode = mode;
    s_tap_enabled = taps;
    /* A status held across the transition cannot become a new double tap.
     * Wait for it to clear before accepting the next hardware event. */
    s_tap_latched = true;
    s_generation++;
    /* Rev A section 7.3: gyro wake ~150 ms plus 3/ODR settling; low-power
     * accel needs ~3 ms plus 3/21 s. Never publish stale register contents. */
    s_ready_us = esp_timer_get_time() + (mode == ORB_IMU_MODE_ACTIVE_224 ? 200000 : 150000);
    return ESP_OK;
}

esp_err_t qmi8658_configure(bool active, bool taps)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    orb_board_imu_mode_t wanted = active ? ORB_IMU_MODE_ACTIVE_224 :
        taps ? ORB_IMU_MODE_ACCEL_250_TAP : ORB_IMU_MODE_ACCEL_21;
    /* A refused profile is retried slowly: a one-off I2C glitch must not pin
     * the gyro on for a whole idle period, or stall the service every poll. */
    if (s_requested_mode == wanted && s_requested_taps == taps && s_rejected_request &&
        esp_timer_get_time() < s_retry_us)
        return ESP_FAIL;
    s_requested_mode = wanted;
    s_requested_taps = taps;
    if (s_mode == wanted && s_tap_enabled == taps) {
        s_rejected_request = false;
        return ESP_OK;
    }
    esp_err_t err = configure_mode(wanted, taps);
    s_rejected_request = err != ESP_OK;
    s_retry_us = esp_timer_get_time() + 1000000;
    if (err == ESP_OK) return ESP_OK;
    /* A failed savings request must not strand CTRL7 with both sensors off.
     * Attempt the known working profile even after intermediate errors. */
    ESP_LOGW(TAG, "mode change failed (%s); restoring 224 Hz", esp_err_to_name(err));
    esp_err_t restore = configure_mode(ORB_IMU_MODE_ACTIVE_224, true);
    if (restore != ESP_OK) {
        /* Even a tap handshake failure must still attempt accelerometer on. */
        esp_err_t accel = ESP_FAIL, gyro = ESP_FAIL;
        if (wr(REG_CTRL7, 0) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(s_mode == ORB_IMU_MODE_ACCEL_21 ? 96 : 10));
            wr(REG_CTRL9, 0);
            wr(REG_CTRL8, 0x80);
            accel = wr(REG_CTRL2, ACCEL_4G_224HZ);
            gyro = wr(REG_CTRL3, GYRO_512DPS_224HZ);
        }
        esp_err_t enable = wr(REG_CTRL7, 0x03);
        s_mode = accel == ESP_OK && gyro == ESP_OK && enable == ESP_OK ?
            ORB_IMU_MODE_ACTIVE_224 : ORB_IMU_MODE_UNKNOWN;
        s_tap_enabled = false;
        s_generation++;
        s_ready_us = esp_timer_get_time() + 200000;
    }
    s_retry_us = esp_timer_get_time() + (s_mode == ORB_IMU_MODE_UNKNOWN ? 1000000 : 30000000);
    return err;
}

orb_board_imu_mode_t qmi8658_mode(void) { return s_mode; }
uint32_t qmi8658_generation(void) { return s_generation; }

esp_err_t qmi8658_motion(orb_motion_sample_t *out)
{
    uint8_t raw[12];
    ESP_RETURN_ON_FALSE(s_dev && out, ESP_ERR_INVALID_STATE, TAG, "not initialised/argument");
    if (s_mode == ORB_IMU_MODE_UNKNOWN || esp_timer_get_time() < s_ready_us) return ESP_ERR_NOT_FINISHED;
    bool gyro_on = s_mode == ORB_IMU_MODE_ACTIVE_224;
    ESP_RETURN_ON_ERROR(rd(REG_AX_L, raw, gyro_on ? sizeof(raw) : 6), TAG, "read motion");
    float accel[3], gyro[3] = {0}, a[3], g[3];
    orb_imu_decode(raw, ORB_IMU_LSB_PER_G_4G, accel);
    if (gyro_on) orb_imu_decode(raw + 6, ORB_IMU_LSB_PER_DPS_512, gyro);
    orb_imu_to_screen(accel, a);
    orb_imu_to_screen(gyro, g);
    *out = (orb_motion_sample_t){.ax=a[0], .ay=a[1], .az=a[2], .gx=g[0], .gy=g[1], .gz=g[2]};
    if (s_tap_enabled) {
        uint8_t status = 0, tap = 0;
        ESP_RETURN_ON_ERROR(rd(REG_STATUS1, &status, 1), TAG, "tap status");
        bool pending = (status & 0x02) != 0;
        if (pending && rd(REG_TAP_STATUS, &tap, 1) == ESP_OK)
            out->hardware_double_tap = !s_tap_latched && (tap & 3) == 2;
        /* Some revisions hold activity status until the next event; a held
         * bit must never repeatedly invoke a screen's primary action. */
        s_tap_latched = pending;
    }
    return ESP_OK;
}

esp_err_t qmi8658_accel(orb_accel_t *out)
{
    uint8_t raw[6];
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_ERROR(rd(REG_AX_L, raw, sizeof(raw)), TAG, "read");
    float g[3];
    orb_imu_decode(raw, ORB_IMU_LSB_PER_G_4G, g);
    *out = (orb_accel_t){ g[0], g[1], g[2] };
    return ESP_OK;
}
