/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C: round 466 px CO5300 AMOLED over QSPI,
 * CST9217 touch, ES8311 + ES7210 audio, AXP2101 power chip, QMI8658 motion
 * sensor. PWR reaches the power chip and, through an inverter, GPIO 3; BOOT is
 * GPIO 0. Button wiring and panel quirks as found by Muse Gadgets'
 * board_waveshare_s3_175c.c. Pins, the touch and audio set-up as in Waveshare's
 * BSP for this board (esp32_s3_touch_amoled_1_75c, Apache License 2.0), which
 * Orb no longer pulls in with its LVGL adapter, FreeType and libpng.
 */
#include "orb_board.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "axp2101.h"
#include "board_audio.h"
#include "orb_imu.h"
#include "qmi8658.h"

static const char *TAG = "board";

#define PWR_GPIO GPIO_NUM_3     /* high while PWR is held */
#define BOOT_GPIO GPIO_NUM_0    /* low while BOOT is held */

#define I2C_PORT I2C_NUM_1
#define I2C_SDA GPIO_NUM_15
#define I2C_SCL GPIO_NUM_14
#define I2C_HZ 400000

#define LCD_SPI SPI2_HOST
#define LCD_CS GPIO_NUM_12
#define LCD_CLK GPIO_NUM_38
#define LCD_D0 GPIO_NUM_4
#define LCD_D1 GPIO_NUM_5
#define LCD_D2 GPIO_NUM_6
#define LCD_D3 GPIO_NUM_7
#define LCD_RST GPIO_NUM_1

#define TOUCH_RST GPIO_NUM_2
#define TOUCH_INT GPIO_NUM_11

/* Waveshare's BSP runs the panel at 40 MHz, which takes 26 ms for a full screen: slower
 * than the panel's 16.8 ms scan. 80 MHz is the S3's SPI limit. */
#define LCD_PCLK_HZ (80 * 1000 * 1000)
/* PSRAM also serves CPU instructions, rodata and rendering. Direct PSRAM DMA
 * underflowed at 80 MHz on this board; leave more shared-bus bandwidth. */
#define LCD_PSRAM_PCLK_HZ (40 * 1000 * 1000)

/* QSPI framing of the CO5300: an opcode, then the command as a 24-bit address,
 * then its parameters: on one line, or for pixels (opcode 0x32) on four. */
#define QSPI_OP_CMD 0x02
#define QSPI_OP_PIXELS 0x32
#define LCD_QUEUE 8      /* a window takes 2 commands and up to 3 pixel chunks */
#define LCD_GAP_X 6      /* the panel's memory is wider than the glass */

static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},                     /* RGB565 */
    {0x35, (uint8_t[]){0x00}, 1, 0},                     /* tearing effect on, V-blank only */
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},                     /* dark until the first frame */
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

static i2c_master_bus_handle_t s_i2c;
static bool s_pmu, s_imu;
static StaticSemaphore_t s_pmu_mutex_storage, s_imu_mutex_storage;
static SemaphoreHandle_t s_pmu_mutex, s_imu_mutex;

/*
 * The panel's wire. esp_lcd sends each command as blocking transactions of
 * its own (two per command with parameters, ~45 us each), seven per band;
 * here a window is one queued chain: CASET, RASET and the pixels, which the
 * SPI driver runs back to back while the CPU draws the next band.
 */
static spi_device_handle_t s_lcd;
static spi_transaction_ext_t s_tx[LCD_QUEUE];
static int s_inflight;
static bool s_cs_held, s_bus_acquired;
static size_t s_chunk_max;
static orb_panel_done_cb_t s_done;
static void *s_done_arg;
#define LCD_PIXEL_LAST 1u
#define LCD_PIXEL_FIRST 2u
static volatile int64_t s_pixel_started_us, s_pixel_completed_us;
static bool s_pixel_chain_queued, s_pixel_timing_valid;

esp_err_t orb_board_init(void)
{
    s_pmu_mutex = xSemaphoreCreateMutexStatic(&s_pmu_mutex_storage);
    s_imu_mutex = xSemaphoreCreateMutexStatic(&s_imu_mutex_storage);
    ESP_RETURN_ON_FALSE(s_pmu_mutex && s_imu_mutex, ESP_ERR_NO_MEM, TAG, "board mutexes");
    const i2c_master_bus_config_t i2c = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c, &s_i2c), TAG, "i2c");
    i2c_master_bus_handle_t bus = s_i2c;
    board_audio_init(bus);

    const gpio_config_t buttons = {
        .pin_bit_mask = (1ULL << PWR_GPIO) | (1ULL << BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&buttons), TAG, "buttons");

    esp_err_t err = axp2101_init(bus);
    if (err == ESP_OK) {
        /* Only DCDC1 (3.3 V) and ALDO1 (codecs) feed anything on this board. */
        err = axp2101_keep_rails(BIT(0), BIT(0));
    }
    s_pmu = err == ESP_OK;
    if (!s_pmu) {
        ESP_LOGW(TAG, "power chip: %s", esp_err_to_name(err));
    }

    err = qmi8658_init(bus);
    s_imu = err == ESP_OK;
    if (!s_imu) {
        ESP_LOGW(TAG, "motion sensor: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void IRAM_ATTR lcd_pre_cb(spi_transaction_t *t)
{
    if ((uintptr_t)t->user & LCD_PIXEL_FIRST) s_pixel_started_us = esp_timer_get_time();
}

static void IRAM_ATTR lcd_post_cb(spi_transaction_t *t)
{
    if ((uintptr_t)t->user & LCD_PIXEL_LAST) {
        s_pixel_completed_us = esp_timer_get_time();
        if (s_done) s_done(s_done_arg);
    }
}

/* Every returned descriptor has finished, even if its DMA result failed. */
static esp_err_t lcd_drain(void)
{
    esp_err_t first = ESP_OK;
    while (s_inflight > 0) {
        spi_transaction_t *t = NULL;
        esp_err_t err = spi_device_get_trans_result(s_lcd, &t, portMAX_DELAY);
        if (err == ESP_ERR_TIMEOUT) continue; /* Still owned by SPI; never recycle it. */
        /* DMA underflow returns INVALID_STATE AND a completed transaction.
         * An invalid driver handle/result is a fatal invariant, not permission
         * to release a buffer that hardware may still be reading. */
        if (!t) {
            ESP_LOGE(TAG, "SPI completion lost ownership: %s", esp_err_to_name(err));
            abort();
        }
        s_inflight--;
        if (first == ESP_OK && err != ESP_OK) first = err;
    }
    return first;
}

static esp_err_t lcd_recycle(void)
{
    esp_err_t first = lcd_drain();
    if (s_cs_held) {
        /* If queueing stopped halfway through a pixel chain, its final chunk
         * never released CS. End the chain with no command, address or pixels
         * before the next CASET/RASET. A failed release stays pending. */
        s_tx[0] = (spi_transaction_ext_t){
            .base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR |
                          SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL,
        };
        esp_err_t err = spi_device_queue_trans(s_lcd, &s_tx[0].base, portMAX_DELAY);
        if (err == ESP_OK) {
            s_inflight = 1;
            err = lcd_drain();
            if (err == ESP_OK) s_cs_held = false;
        }
        if (first == ESP_OK) first = err;
    }
    return first;
}

static esp_err_t lcd_acquire(void)
{
    if (s_bus_acquired) return ESP_OK;
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_lcd, portMAX_DELAY), TAG, "acquire");
    s_bus_acquired = true;
    return ESP_OK;
}

/* lcd_send alone owns this fence and every command. Release only after DMA
 * and CS are idle; keeping the device acquired also keeps IDF's APB lock. */
esp_err_t orb_board_panel_bus_release(void)
{
    ESP_RETURN_ON_ERROR(lcd_recycle(), TAG, "panel sleep fence");
    if (s_bus_acquired) {
        spi_device_release_bus(s_lcd);
        s_bus_acquired = false;
    }
    return ESP_OK;
}

esp_err_t orb_board_panel_finish(void)
{
    esp_err_t err = lcd_recycle();
    if (err != ESP_OK) s_pixel_chain_queued = false;
    s_pixel_timing_valid = err == ESP_OK && s_pixel_chain_queued &&
        s_pixel_started_us > 0 && s_pixel_completed_us >= s_pixel_started_us;
    return err;
}

bool orb_board_panel_timing(int64_t *started_us, int64_t *completed_us)
{
    /* One sender owns write/finish/readback. After finish, the SPI ISR has
     * retired both timestamps, so its 64-bit stores cannot race this copy. */
    bool valid = s_pixel_timing_valid && s_inflight == 0;
    if (started_us) *started_us = valid ? s_pixel_started_us : 0;
    if (completed_us) *completed_us = valid ? s_pixel_completed_us : 0;
    return valid;
}

static esp_err_t lcd_queue(spi_transaction_ext_t *t)
{
    ESP_RETURN_ON_FALSE(s_inflight < LCD_QUEUE, ESP_ERR_INVALID_SIZE, TAG, "transaction capacity");
    ESP_RETURN_ON_ERROR(spi_device_queue_trans(s_lcd, &t->base, portMAX_DELAY), TAG, "queue");
    s_inflight++;
    s_cs_held = (t->base.flags & SPI_TRANS_CS_KEEP_ACTIVE) != 0;
    return ESP_OK;
}

static esp_err_t lcd_command(uint8_t reg, const uint8_t *param, size_t len)
{
    ESP_RETURN_ON_FALSE(s_inflight < LCD_QUEUE && len <= 4, ESP_ERR_INVALID_SIZE, TAG, "command capacity");
    ESP_RETURN_ON_ERROR(lcd_acquire(), TAG, "command bus");
    spi_transaction_ext_t *t = &s_tx[s_inflight];
    *t = (spi_transaction_ext_t){
        /* IDF's override persists on the device; zero would retain the last
         * pixel clock instead of restoring the command's 80 MHz default. */
        .base = { .flags = (len ? SPI_TRANS_USE_TXDATA : 0) | SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL,
                  .cmd = QSPI_OP_CMD, .addr = reg << 8, .length = len * 8,
                  .override_freq_hz = LCD_PCLK_HZ },
    };
    if (len) memcpy(t->base.tx_data, param, len);
    return lcd_queue(t);
}

esp_err_t orb_board_panel_command(uint8_t command)
{
    ESP_RETURN_ON_FALSE(s_lcd, ESP_ERR_INVALID_STATE, TAG, "panel not started");
    ESP_RETURN_ON_ERROR(lcd_recycle(), TAG, "panel command fence");
    esp_err_t err = lcd_command(command, NULL, 0);
    esp_err_t finished = lcd_recycle();
    return err == ESP_OK ? finished : err;
}

esp_err_t orb_board_panel_start(size_t max_transfer, orb_panel_done_cb_t done, void *arg)
{
    const spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(LCD_CLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, max_transfer);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    /* The vendor init sequence through esp_lcd's CO5300 driver; then its I/O
     * steps aside for our own device on the same chip select. */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = LCD_PCLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI, &io_cfg, &io), TAG,
                        "panel io");
    co5300_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(io, &panel_cfg, &panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "on");
    /* Not esp_lcd_panel_del(): it would release the reset line. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(io), TAG, "io del");

    const spi_device_interface_config_t dev = {
        .command_bits = 8,
        .address_bits = 24,
        .clock_speed_hz = LCD_PCLK_HZ,
        .spics_io_num = LCD_CS,
        .queue_size = LCD_QUEUE,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .pre_cb = lcd_pre_cb,
        .post_cb = lcd_post_cb,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_SPI, &dev, &s_lcd), TAG, "lcd device");
    ESP_RETURN_ON_ERROR(spi_bus_get_max_transaction_len(LCD_SPI, &s_chunk_max), TAG, "max len");
    /* Pixel chunks keep CS selected. Sleep releases this acquisition; all
     * command paths reacquire before touching the wire, on the same owner. */
    ESP_RETURN_ON_ERROR(lcd_acquire(), TAG, "initial bus");
    s_done = done;
    s_done_arg = arg;
    return ESP_OK;
}

esp_err_t orb_board_panel_write(int x1, int y1, int x2, int y2, const void *px, size_t bytes)
{
    esp_err_t recycled = lcd_recycle();
    s_pixel_started_us = s_pixel_completed_us = 0;
    s_pixel_chain_queued = s_pixel_timing_valid = false;
    ESP_RETURN_ON_ERROR(recycled, TAG, "previous completion");
    ESP_RETURN_ON_FALSE(s_lcd && s_chunk_max, ESP_ERR_INVALID_STATE, TAG, "panel not started");
    ESP_RETURN_ON_FALSE(px && bytes && x2 >= x1 && y2 >= y1 &&
                        bytes == (size_t)(x2 - x1 + 1) * (y2 - y1 + 1) * 2,
                        ESP_ERR_INVALID_ARG, TAG, "pixel window");
    ESP_RETURN_ON_FALSE(2 + (bytes + s_chunk_max - 1) / s_chunk_max <= LCD_QUEUE,
                        ESP_ERR_INVALID_SIZE, TAG, "window exceeds transaction capacity");
    bool external = esp_ptr_external_ram(px);
    ESP_RETURN_ON_FALSE(!external || esp_ptr_dma_ext_capable(px), ESP_ERR_INVALID_ARG, TAG, "external DMA source");
    x1 += LCD_GAP_X;
    x2 += LCD_GAP_X;
    esp_err_t err = lcd_command(0x2A, (uint8_t[]){ x1 >> 8, x1, x2 >> 8, x2 }, 4);
    if (err != ESP_OK) goto failed;
    err = lcd_command(0x2B, (uint8_t[]){ y1 >> 8, y1, y2 >> 8, y2 }, 4);
    if (err != ESP_OK) goto failed;

    /* TX on S3 GDMA v1 accepts exact lengths (alignment 1). Never round wire
     * length: extra bytes would become pixels outside this window. MANUAL
     * rejects an unsuitable source instead of silently allocating a bounce. */
    const uint8_t *p = px;
    for (bool first = true; bytes > 0; first = false) {
        size_t n = bytes < s_chunk_max ? bytes : s_chunk_max;
        spi_transaction_ext_t *t = &s_tx[s_inflight];
        *t = (spi_transaction_ext_t){
            .base = { .flags = SPI_TRANS_MODE_QIO | SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL,
                      .cmd = QSPI_OP_PIXELS, .addr = 0x2C << 8, .length = n * 8, .tx_buffer = p,
                      .override_freq_hz = external ? LCD_PSRAM_PCLK_HZ : LCD_PCLK_HZ },
        };
        if (external) t->base.flags |= SPI_TRANS_DMA_USE_PSRAM;
        if (first) t->base.user = (void *)(uintptr_t)LCD_PIXEL_FIRST;
        if (!first) t->base.flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
        if (bytes > n) t->base.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
        else t->base.user = (void *)((uintptr_t)t->base.user | LCD_PIXEL_LAST);
        err = lcd_queue(t);
        if (err != ESP_OK) goto failed;
        p += n;
        bytes -= n;
    }
    s_pixel_chain_queued = true;
    return ESP_OK;

failed:
    /* A partially submitted chain still owns its buffers. Drain every queued
     * result even when no final transaction/callback was ever submitted. */
    lcd_recycle();
    return err;
}

#define BRIGHTNESS_HISTORY 48
static orb_board_brightness_write_t s_brightness_history[BRIGHTNESS_HISTORY];
static unsigned s_brightness_writes;

size_t orb_board_brightness_history(orb_board_brightness_write_t *out, size_t capacity)
{
    size_t count = s_brightness_writes < BRIGHTNESS_HISTORY ? s_brightness_writes : BRIGHTNESS_HISTORY;
    if (count > capacity) count = capacity;
    for (size_t i = 0; i < count; i++)
        out[i] = s_brightness_history[(s_brightness_writes - count + i) % BRIGHTNESS_HISTORY];
    return count;
}

esp_err_t orb_board_panel_set_brightness(int pct)
{
    s_brightness_history[s_brightness_writes++ % BRIGHTNESS_HISTORY] =
        (orb_board_brightness_write_t){esp_timer_get_time(), (uint8_t)pct};
    esp_err_t err = lcd_recycle();
    if (err == ESP_OK) err = lcd_command(0x51, (uint8_t[]){ (uint8_t)(pct * 255 / 100) }, 1);
    esp_err_t finished = lcd_recycle();
    if (err == ESP_OK) err = finished;
    return err;
}

void orb_board_panel_brightness(int pct)
{
    esp_err_t err = orb_board_panel_set_brightness(pct);
    if (err != ESP_OK) ESP_LOGE(TAG, "brightness: %s", esp_err_to_name(err));
}

esp_err_t orb_board_touch_new(esp_lcd_touch_handle_t *tp, void (*on_report)(void *arg), void *arg)
{
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    io_cfg.scl_speed_hz = I2C_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io), TAG, "touch io");
    /* Mounted as the panel is: turned half a turn. */
    const esp_lcd_touch_config_t cfg = {
        .x_max = ORB_LCD_W,
        .y_max = ORB_LCD_H,
        .rst_gpio_num = TOUCH_RST,
        .int_gpio_num = TOUCH_INT,
        .flags = { .mirror_x = 1, .mirror_y = 1 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst9217(io, &cfg, tp), TAG, "touch");
    if (!on_report) {
        return ESP_OK;
    }
    /* The driver set the line up (falling edge). Not through esp_lcd_touch, which
     * would install the ISR service again and log an error. */
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(TOUCH_INT, on_report, arg), TAG, "touch isr");
    return gpio_intr_enable(TOUCH_INT);
}


unsigned orb_board_buttons(void)
{
    return (gpio_get_level(PWR_GPIO) ? ORB_BTN_PWR : 0) | (gpio_get_level(BOOT_GPIO) ? 0 : ORB_BTN_BOOT);
}


esp_err_t orb_board_power(orb_power_t *out)
{
    if (!s_pmu) return ESP_ERR_NOT_SUPPORTED;
    if (!out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_pmu_mutex, portMAX_DELAY);
    esp_err_t err = axp2101_read(out);
    xSemaphoreGive(s_pmu_mutex);
    return err;
}

esp_err_t orb_board_battery_detail(orb_battery_detail_t *out)
{
    if (!s_pmu) return ESP_ERR_NOT_SUPPORTED;
    if (!out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_pmu_mutex, portMAX_DELAY);
    esp_err_t err = axp2101_battery_detail(out);
    xSemaphoreGive(s_pmu_mutex);
    return err;
}

esp_err_t orb_board_set_desk_charge_limit(bool enabled)
{
    if (!s_pmu) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(s_pmu_mutex, portMAX_DELAY);
    esp_err_t err = axp2101_set_desk_charge_limit(enabled);
    xSemaphoreGive(s_pmu_mutex);
    return err;
}

bool orb_board_desk_charge_limit(void)
{
    if (!s_pmu) return false;
    xSemaphoreTake(s_pmu_mutex, portMAX_DELAY);
    bool enabled = axp2101_desk_charge_limit();
    xSemaphoreGive(s_pmu_mutex);
    return enabled;
}

esp_err_t orb_board_battery_desk_poll(bool resting, uint64_t now_ms)
{
    if (!s_pmu) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(s_pmu_mutex, portMAX_DELAY);
    esp_err_t err = axp2101_desk_poll(resting, now_ms);
    xSemaphoreGive(s_pmu_mutex);
    return err;
}

esp_err_t orb_board_motion(orb_motion_sample_t *out)
{
    if (!s_imu) return ESP_ERR_NOT_SUPPORTED;
    if (!out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_imu_mutex, portMAX_DELAY);
    esp_err_t err = qmi8658_motion(out);
    xSemaphoreGive(s_imu_mutex);
    return err;
}

esp_err_t orb_board_motion_configure(bool active, bool taps)
{
    if (!s_imu) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(s_imu_mutex, portMAX_DELAY);
    esp_err_t err = qmi8658_configure(active, taps);
    xSemaphoreGive(s_imu_mutex);
    return err;
}

orb_board_imu_mode_t orb_board_motion_mode(void)
{
    if (!s_imu) return ORB_IMU_MODE_UNKNOWN;
    xSemaphoreTake(s_imu_mutex, portMAX_DELAY);
    orb_board_imu_mode_t mode = qmi8658_mode();
    xSemaphoreGive(s_imu_mutex);
    return mode;
}

uint32_t orb_board_motion_generation(void) { return qmi8658_generation(); }

esp_err_t orb_board_accel(orb_accel_t *out)
{
    if (!s_imu) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!out) return ESP_ERR_INVALID_ARG;
    orb_accel_t raw;
    xSemaphoreTake(s_imu_mutex, portMAX_DELAY);
    esp_err_t err = qmi8658_accel(&raw);
    xSemaphoreGive(s_imu_mutex);
    ESP_RETURN_ON_ERROR(err, TAG, "accel");
    float screen[3];
    orb_imu_to_screen((const float[3]){ raw.x, raw.y, raw.z }, screen);
    *out = (orb_accel_t){ screen[0], screen[1], screen[2] };
    return ESP_OK;
}

esp_err_t orb_board_power_off(void)
{
    if (!s_pmu) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(s_pmu_mutex, portMAX_DELAY);
    esp_err_t err = axp2101_power_off();
    xSemaphoreGive(s_pmu_mutex);
    return err;
}
