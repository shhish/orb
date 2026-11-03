"""Run production SPI pixel timing/fences with virtual ISR and cleanup latency."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    component = Path(__file__).resolve().parents[2]
    source = (component / "board_ws175c.c").read_text()
    assert ".pre_cb = lcd_pre_cb" in source and ".post_cb = lcd_post_cb" in source
    begin = source.index("static spi_device_handle_t s_lcd;")
    end = source.index("\nesp_err_t orb_board_init", begin)
    globals_source = source[begin:end]

    def function(signature):
        start = source.index(signature)
        start = source.rfind("\n", 0, start) + 1
        stop = source.index("\n}", start) + 2
        return source[start:stop]

    fixture = (
        r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef int spi_device_handle_t;
typedef void (*orb_panel_done_cb_t)(void *);
typedef struct { unsigned flags, cmd, addr, length, override_freq_hz;
    uint8_t tx_data[4]; const void *tx_buffer; void *user; } spi_transaction_t;
typedef struct { spi_transaction_t base; } spi_transaction_ext_t;
#define IRAM_ATTR
#define ESP_OK 0
#define ESP_ERR_INVALID_SIZE -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_TIMEOUT -4
#define portMAX_DELAY 7
#define LCD_QUEUE 8
#define LCD_GAP_X 6
#define LCD_PCLK_HZ 80000000
#define LCD_PSRAM_PCLK_HZ 40000000
#define QSPI_OP_CMD 2
#define QSPI_OP_PIXELS 50
#define SPI_TRANS_VARIABLE_CMD 1u
#define SPI_TRANS_VARIABLE_ADDR 2u
#define SPI_TRANS_VARIABLE_DUMMY 4u
#define SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL 8u
#define SPI_TRANS_USE_TXDATA 16u
#define SPI_TRANS_MODE_QIO 32u
#define SPI_TRANS_DMA_USE_PSRAM 64u
#define SPI_TRANS_CS_KEEP_ACTIVE 128u
static const char *TAG = "test";
#define ESP_RETURN_ON_FALSE(condition, error, tag, ...) do { (void)(tag); if (!(condition)) return (error); } while (0)
#define ESP_RETURN_ON_ERROR(expression, tag, ...) do { (void)(tag); esp_err_t result_ = (expression); if (result_ != ESP_OK) return result_; } while (0)
#define ESP_LOGE(tag, ...) ((void)(tag))
static int64_t s_now = 100;
static unsigned s_callbacks, s_queue_attempts, s_queue_count, s_returned, s_release_count;
static int s_fail_queue = -1;
static bool s_fail_final;
static bool s_driver_bus, s_fail_acquire;
static unsigned s_bus_acquires, s_bus_releases;
static spi_transaction_t *s_queued[8];
static unsigned s_seen_flags[8];
static int64_t s_first_isr, s_last_isr;
static void lcd_pre_cb(spi_transaction_t *t);
static void lcd_post_cb(spi_transaction_t *t);
static int64_t esp_timer_get_time(void) { return s_now; }
static bool esp_ptr_external_ram(const void *p) { (void)p; return false; }
static bool esp_ptr_dma_ext_capable(const void *p) { (void)p; return true; }
static int spi_device_acquire_bus(int device, int timeout) {
    assert(device == 1 && timeout == portMAX_DELAY && !s_driver_bus);
    if (s_fail_acquire) { s_fail_acquire = false; return ESP_ERR_INVALID_STATE; }
    s_driver_bus = true; s_bus_acquires++;
    return ESP_OK;
}
static void spi_device_release_bus(int device) {
    assert(device == 1 && s_driver_bus && s_returned == s_queue_count);
    s_driver_bus = false; s_bus_releases++;
}
static int spi_device_queue_trans(int device, spi_transaction_t *t, int timeout) {
    assert(device == 1 && timeout == portMAX_DELAY && s_driver_bus);
    if ((int)s_queue_attempts++ == s_fail_queue) return ESP_ERR_INVALID_STATE;
    if (s_returned == s_queue_count) s_returned = s_queue_count = 0;
    assert(s_queue_count < 8);
    s_seen_flags[s_queue_count] = (unsigned)(uintptr_t)t->user;
    s_queued[s_queue_count++] = t;
    if (!t->cmd && !t->length) s_release_count++;
    return ESP_OK;
}
static int spi_device_get_trans_result(int device, spi_transaction_t **out, int timeout) {
    assert(device == 1 && timeout == portMAX_DELAY && s_returned < s_queue_count);
    spi_transaction_t *t = s_queued[s_returned++];
    s_now += 10;
    lcd_pre_cb(t);
    if ((uintptr_t)t->user & 2u) s_first_isr = s_now;
    s_now += (int64_t)t->length / 8 + 10;
    lcd_post_cb(t);
    if ((uintptr_t)t->user & 1u) s_last_isr = s_now;
    *out = t;
    s_now += 5000; /* CPU result cleanup cannot change the ISR timestamp. */
    if (s_fail_final && ((uintptr_t)t->user & 1u)) { s_fail_final = false; return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}
"""
        + globals_source
        + "\n"
        + "\n".join(
            function(signature)
            for signature in (
                "static void IRAM_ATTR lcd_pre_cb(spi_transaction_t *t)",
                "static void IRAM_ATTR lcd_post_cb(spi_transaction_t *t)",
                "static esp_err_t lcd_drain(void)",
                "static esp_err_t lcd_recycle(void)",
                "static esp_err_t lcd_acquire(void)",
                "esp_err_t orb_board_panel_bus_release(void)",
                "esp_err_t orb_board_panel_finish(void)",
                "bool orb_board_panel_timing(int64_t *started_us, int64_t *completed_us)",
                "static esp_err_t lcd_queue(spi_transaction_ext_t *t)",
                "static esp_err_t lcd_command(uint8_t reg, const uint8_t *param, size_t len)",
                "esp_err_t orb_board_panel_command(uint8_t command)",
                "esp_err_t orb_board_panel_write(int x1, int y1, int x2, int y2, const void *px, size_t bytes)",
            )
        )
        + r"""
static void done(void *arg) { assert(arg == (void *)17); s_callbacks++; }
static void reset(size_t chunk) {
    s_lcd = 1; s_chunk_max = chunk; s_inflight = 0; s_cs_held = false;
    s_bus_acquired = s_driver_bus = true; s_fail_acquire = false;
    s_bus_acquires = s_bus_releases = 0;
    s_pixel_started_us = s_pixel_completed_us = 0;
    s_pixel_chain_queued = s_pixel_timing_valid = false;
    s_done = done; s_done_arg = (void *)17;
    s_now = 100; s_callbacks = s_queue_attempts = s_queue_count = s_returned = s_release_count = 0;
    s_fail_queue = -1; s_fail_final = false; s_first_isr = s_last_isr = 0;
}
int main(void) {
    uint8_t pixels[128] = {0};
    int64_t start = -1, end = -1;
    assert(!orb_board_panel_timing(&start, &end) && !start && !end);
    assert(orb_board_panel_finish() == ESP_OK && !orb_board_panel_timing(NULL, NULL));
    for (unsigned mode = 0; mode < 2; mode++) {
        reset(mode ? 48 : 128);
        assert(orb_board_panel_write(0, 0, 31, 1, pixels, sizeof(pixels)) == ESP_OK);
        assert(!orb_board_panel_timing(&start, &end) && !start && !end);
        assert(!s_callbacks && s_inflight == (mode ? 5 : 3));
        if (mode) assert(s_seen_flags[2] == 2 && s_seen_flags[3] == 0 && s_seen_flags[4] == 1);
        else assert(s_seen_flags[2] == 3);
        assert(orb_board_panel_finish() == ESP_OK);
        assert(orb_board_panel_timing(&start, &end));
        assert(start == s_first_isr && end == s_last_isr && end >= start && s_now - end == 5000);
        assert(s_callbacks == 1 && !s_inflight && !s_cs_held);
        assert(orb_board_panel_write(0, 0, 31, 1, NULL, sizeof(pixels)) == ESP_ERR_INVALID_ARG);
        assert(!orb_board_panel_timing(&start, &end) && !start && !end);
    }
    for (int fail = 0; fail < 5; fail++) {
        reset(48); s_fail_queue = fail;
        assert(orb_board_panel_write(0, 0, 31, 1, pixels, sizeof(pixels)) == ESP_ERR_INVALID_STATE);
        assert(!s_callbacks && !s_inflight && !s_cs_held);
        assert(s_release_count == (unsigned)(fail >= 3));
        assert(!orb_board_panel_timing(&start, &end) && !start && !end);
        assert(orb_board_panel_finish() == ESP_OK && !orb_board_panel_timing(NULL, NULL));
    }
    reset(48); s_fail_final = true;
    assert(orb_board_panel_write(0, 0, 31, 1, pixels, sizeof(pixels)) == ESP_OK);
    assert(orb_board_panel_finish() == ESP_ERR_INVALID_STATE);
    assert(s_callbacks == 1 && !s_inflight);
    assert(!orb_board_panel_timing(&start, &end) && !start && !end);
    assert(orb_board_panel_finish() == ESP_OK && !orb_board_panel_timing(NULL, NULL));
    reset(128);
    assert(orb_board_panel_write(0, 0, 31, 1, pixels, sizeof(pixels)) == ESP_OK);
    assert(s_inflight == 3 && !s_callbacks);
    assert(orb_board_panel_command(0x28) == ESP_OK);
    assert(!s_inflight && s_callbacks == 1 && !s_cs_held);
    assert(s_tx[0].base.cmd == QSPI_OP_CMD && s_tx[0].base.addr == (0x28 << 8));
    assert(!s_tx[0].base.length);
    reset(128); s_fail_queue = 3;
    assert(orb_board_panel_write(0, 0, 31, 1, pixels, sizeof(pixels)) == ESP_OK);
    assert(orb_board_panel_command(0x10) == ESP_ERR_INVALID_STATE);
    assert(!s_inflight && s_callbacks == 1 && !s_cs_held);
    reset(48);
    assert(orb_board_panel_write(0, 0, 31, 1, pixels, sizeof(pixels)) == ESP_OK);
    assert(orb_board_panel_bus_release() == ESP_OK);
    assert(!s_inflight && !s_cs_held && !s_bus_acquired && !s_driver_bus);
    assert(s_callbacks == 1 && s_bus_releases == 1);
    assert(orb_board_panel_bus_release() == ESP_OK && s_bus_releases == 1);
    assert(orb_board_panel_command(0x11) == ESP_OK);
    assert(s_bus_acquired && s_driver_bus && s_bus_acquires == 1);
    assert(orb_board_panel_bus_release() == ESP_OK);
    s_fail_acquire = true;
    assert(orb_board_panel_command(0x29) == ESP_ERR_INVALID_STATE);
    assert(!s_bus_acquired && !s_driver_bus && !s_inflight);
    assert(orb_board_panel_write(0, 0, 31, 1, pixels, sizeof(pixels)) == ESP_OK);
    assert(s_bus_acquired && s_driver_bus && s_bus_acquires == 2);
    assert(orb_board_panel_finish() == ESP_OK && s_callbacks == 2);
    assert(orb_board_panel_bus_release() == ESP_OK);
    assert(!s_driver_bus && s_bus_releases == 3);
    puts("panel timing: ISR provenance, DMA/CS fences, bus release/reacquire and acquisition failure passed");
}
"""
    )
    with tempfile.TemporaryDirectory(prefix="orb-panel-timing-") as temporary:
        directory = Path(temporary)
        path = directory / "timing.c"
        path.write_text(fixture)
        binary = directory / "timing"
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-O1",
                "-g",
                "-fsanitize=address,undefined",
                str(path),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
