#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "orb_imu.h"

typedef int esp_err_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
typedef struct { float x, y, z; } orb_accel_t;
typedef struct { float ax, ay, az, gx, gy, gz; bool hardware_double_tap; } orb_motion_sample_t;
typedef enum { ORB_IMU_MODE_UNKNOWN, ORB_IMU_MODE_ACTIVE_224, ORB_IMU_MODE_ACCEL_250_TAP,
               ORB_IMU_MODE_ACCEL_21 } orb_board_imu_mode_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_NOT_FINISHED 5
#define I2C_ADDR_BIT_LEN_7 7
#define ESP_RETURN_ON_ERROR(x, ...) do { int e = (x); if (e) return e; } while (0)
#define ESP_RETURN_ON_FALSE(c, e, ...) do { if (!(c)) return (e); } while (0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)(tag))
#define pdMS_TO_TICKS(ms) (ms)
static uint8_t s_regs[256];
static unsigned s_operations, s_fail_at, s_last_read_bytes;
static bool s_fail_all;
static int64_t s_now;
static void vTaskDelay(unsigned ticks) { s_now += (int64_t)ticks * 1000; }
static int64_t esp_timer_get_time(void) { return s_now; }
#define esp_err_to_name(err) "error"
static int i2c_master_bus_add_device(void *bus, const i2c_device_config_t *cfg, void **out)
{
    assert(bus && cfg->device_address == 0x6b); *out = bus; return ESP_OK;
}
static int i2c_master_transmit(void *dev, const void *data, size_t length, int timeout)
{
    assert(dev && length == 2 && timeout == 50);
    const uint8_t *value = data;
    s_operations++;
    if (s_fail_all || s_operations == s_fail_at) return ESP_FAIL;
    if ((value[0] == 3 || value[0] == 4 || value[0] == 9 || value[0] == 10) && value[0] != 8)
        assert(s_regs[8] == 0); /* Mode/tap writes require both sensors disabled. */
    s_regs[value[0]] = value[1];
    return ESP_OK;
}
static int i2c_master_transmit_receive(void *dev, const void *tx, size_t ntx,
                                     void *data, size_t length, int timeout)
{
    assert(dev && ntx == 1 && timeout == 50);
    s_operations++;
    if (s_fail_all || s_operations == s_fail_at) return ESP_FAIL;
    uint8_t reg = *(const uint8_t *)tx;
    if (reg == 0x35) s_last_read_bytes = (unsigned)length;
    memcpy(data, s_regs + reg, length);
    if (reg == 0x2d) *(uint8_t *)data = s_regs[0x0a] ? 0x80 : 0;
    return ESP_OK;
}
#include "qmi_control.inc"

static void initialize(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    s_regs[0] = 5; s_regs[0x4d] = 0x80;
    s_mode = s_requested_mode = ORB_IMU_MODE_UNKNOWN;
    s_dev = NULL; s_ready_us = s_retry_us = 0; s_now = 0;
    s_operations = s_fail_at = 0; s_fail_all = false;
    assert(qmi8658_init((void *)1) == ESP_OK);
    assert(qmi8658_mode() == ORB_IMU_MODE_ACTIVE_224 && s_regs[8] == 3);
}
static void profiles(void)
{
    initialize();
    uint32_t gen = qmi8658_generation();
    assert(qmi8658_configure(false, false) == ESP_OK);
    assert(qmi8658_mode() == ORB_IMU_MODE_ACCEL_21 && s_regs[3] == 0x1d && s_regs[8] == 1 && s_regs[9] == 0x80);
    assert(qmi8658_generation() != gen);
    unsigned n = s_operations;
    assert(qmi8658_configure(false, false) == ESP_OK && s_operations == n);
    orb_motion_sample_t sample;
    assert(qmi8658_motion(&sample) == ESP_ERR_NOT_FINISHED);
    s_now += 150000; s_regs[0x3a] = 0x20; /* +1g z. */
    assert(qmi8658_motion(&sample) == ESP_OK && s_last_read_bytes == 6);
    assert(sample.az == 1 && sample.gx == 0 && sample.gy == 0 && sample.gz == 0);
    assert(qmi8658_configure(false, true) == ESP_OK);
    assert(qmi8658_mode() == ORB_IMU_MODE_ACCEL_250_TAP && s_regs[3] == 0x15 && s_regs[8] == 1 && s_regs[9] == 0x81);
    assert(s_regs[0x0f] == 0x52); /* Thresholds survive timing rescaling. */
    s_now += 150000;
    s_regs[0x2f] = 2; s_regs[0x59] = 2;
    assert(qmi8658_motion(&sample) == ESP_OK && !sample.hardware_double_tap);
    s_regs[0x2f] = 0; assert(qmi8658_motion(&sample) == ESP_OK);
    s_regs[0x2f] = 2; assert(qmi8658_motion(&sample) == ESP_OK && sample.hardware_double_tap);
    assert(qmi8658_configure(true, true) == ESP_OK);
    s_now += 199999; assert(qmi8658_motion(&sample) == ESP_ERR_NOT_FINISHED);
    s_now++; assert(qmi8658_motion(&sample) == ESP_OK && s_last_read_bytes == 12);
}
static void failures(void)
{
    for (unsigned fail = 1; fail <= 6; fail++) {
        initialize();
        s_fail_at = s_operations + fail;
        assert(qmi8658_configure(false, false) != ESP_OK);
        assert(qmi8658_mode() == ORB_IMU_MODE_ACTIVE_224 && s_regs[8] == 3 && s_regs[3] == 0x15);
        unsigned n = s_operations;
        assert(qmi8658_configure(false, false) != ESP_OK && s_operations == n);
        assert(qmi8658_configure(true, true) == ESP_OK);
        assert(qmi8658_configure(true, true) == ESP_OK);
    }
    initialize(); s_fail_all = true;
    assert(qmi8658_configure(false, false) != ESP_OK && qmi8658_mode() == ORB_IMU_MODE_UNKNOWN);
    unsigned n = s_operations;
    assert(qmi8658_configure(false, false) != ESP_OK && s_operations == n);
    s_fail_all = false; s_now += 1000000;
    assert(qmi8658_configure(false, false) == ESP_OK && s_regs[8] == 1);
}
int main(void)
{
    profiles(); failures();
    puts("QMI control: profiles, sensor-off tap writes, settling, stale tap latch, repeated requests, every mode-write failure and recovery passed");
}
