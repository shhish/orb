#pragma once

/*
 * The hardware under the OS. One implementation today: Waveshare
 * ESP32-S3-Touch-AMOLED-1.75C. Everything above this header is board-agnostic.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_codec_dev.h"
#include "esp_lcd_touch.h"
#include "orb_senses.h"
#include "orb_battery.h"

#define ORB_LCD_W 466
#define ORB_LCD_H 466
#define ORB_LCD_ROUND 1

/* Both buttons sit on the right edge: PWR above, BOOT below. */
#define ORB_BTN_PWR (1u << 0)
#define ORB_BTN_BOOT (1u << 1)

typedef struct {
    bool usb;
    bool charging;
    int battery_pct;   /* -1 when no battery */
    int battery_mv;
} orb_power_t;

typedef struct {
    float x, y, z;     /* g; +x right, +y up, +z out of the screen */
} orb_accel_t;

/* I2C, power chip, buttons and motion sensor. Call once, before the rest. */
esp_err_t orb_board_init(void);

/* Runs in an interrupt when a window's pixels are all on the wire. */
typedef void (*orb_panel_done_cb_t)(void *arg);

/*
 * The panel, on and dark, ready to take pixels (RGB565, big-endian on the
 * wire) in writes of up to max_transfer bytes. Call from the task that will
 * send to it: the SPI interrupt lands on that task's core.
 */
esp_err_t orb_board_panel_start(size_t max_transfer, orb_panel_done_cb_t done, void *arg);

/*
 * Queues pixels for the window [x1, x2] x [y1, y2] (screen coordinates; the
 * CO5300 wants even x1, y1 and odd x2, y2) and returns at once; `done` runs
 * when they're out. One write at a time: finish() drains all SPI results and
 * reports completion/DMA errors. The callback alone is not the release fence.
 * px stays untouched until finish(), including after partial queue failure.
 * Internal DMA RAM and aligned, DMA-accessible PSRAM are supported.
 */
esp_err_t orb_board_panel_write(int x1, int y1, int x2, int y2, const void *px, size_t bytes);

/* Drains every submitted transaction, retaining the first error. DMA
 * underflow returns ESP_ERR_INVALID_STATE after ownership has been reclaimed. */
esp_err_t orb_board_panel_finish(void);

/* Sender-only, after a successful finish and before the next write. ISR
 * timestamps bracket the first pixel transaction's start and the final pixel
 * transaction's completion, excluding task wakeup/result-cleanup latency.
 * False (and zero outputs) for startup, incomplete/failed chains or while SPI
 * still owns a descriptor. This observation is never a replacement fence. */
bool orb_board_panel_timing(int64_t *started_us, int64_t *completed_us);

/* 0-100 %. Not while a write is on the wire. Completes before returning. */
void orb_board_panel_brightness(int pct);
esp_err_t orb_board_panel_set_brightness(int pct);
/* Sender-only asynchronous sleep/wake. service() advances after the CO5300's
 * 120 ms sleep-command exclusion window without sleeping on the UI thread.
 * A successful request is acceptance; ready() certifies its completion. */
esp_err_t orb_board_panel_awake(bool awake);
void orb_board_panel_service(void);
bool orb_board_panel_ready(bool awake);
uint32_t orb_board_panel_wait_ms(void);
/* The last panel brightness writes, oldest first, for diagnosing flicker. */
typedef struct { int64_t at_us; uint8_t pct; } orb_board_brightness_write_t;
size_t orb_board_brightness_history(orb_board_brightness_write_t *out, size_t capacity);

/* High while the panel isn't scanning (DCS 35h tearing-effect output). */
#define ORB_LCD_TE_GPIO 13

/*
 * The touch controller. on_report (may be NULL) runs in an interrupt whenever
 * it has a new report, so it need only be read then; the GPIO ISR service
 * must be installed first.
 */
esp_err_t orb_board_touch_new(esp_lcd_touch_handle_t *tp, void (*on_report)(void *arg), void *arg);

/*
 * Audio: the ES8311 (speaker, through its amplifier) and the ES7210 (two
 * microphones) on one I2S port at sample_rate, 16-bit mono TX / stereo RX. Open them with
 * esp_codec_dev_open(). On success, the shared interfaces and channels remain
 * until orb_board_audio_delete(); one task owns the pair and all reads. Failure
 * leaves both outputs NULL and releases all resources, so retry is safe.
 * Output pointers must be non-NULL and distinct.
 */
esp_err_t orb_board_audio_new(uint32_t sample_rate, esp_codec_dev_handle_t *speaker, esp_codec_dev_handle_t *mic);

/* Finite read deadline, after opening the microphone with esp_codec_dev_open().
 * Only the audio owner may call read/delete; do not race either operation. */
esp_err_t orb_board_audio_read(esp_codec_dev_handle_t mic, void *buffer, size_t bytes,
                              size_t *read, uint32_t timeout_ms);
esp_err_t orb_board_audio_delete(esp_codec_dev_handle_t speaker, esp_codec_dev_handle_t mic);

/* Sound owner only, with both wrappers open. Idle mutes the PA and stops both
 * I2S channels; resume restores any suspended codecs and primes silent DMA
 * before the PA can emit. On failure the owner may retry or delete the pair. */
esp_err_t orb_board_audio_idle(bool idle);
/* After bounded idle, power down codec analog domains, retaining ALDO1 and
 * register settings. Sound owner only, both wrappers open and already idle. */
esp_err_t orb_board_audio_suspend(void);

/* Audio owner only, after opening the speaker. Programs the ES8311's digital
 * volume in 0.5 dB steps, hard mute at zero, DRC and click-free soft ramp. */
esp_err_t orb_board_audio_set_volume(unsigned volume);
/* Audio owner only, after opening the microphone. The shared capture/playback
 * profile sets both ES7210 PGAs to 36 dB and both ADC direct gains to +16 dB. */
esp_err_t orb_board_audio_set_mic_gain(void);
/* Finite TX deadline. Digital gain/limiting are hardware-owned, so this may
 * write mono PCM straight to the configured I2S output without software gain. */
esp_err_t orb_board_audio_write(esp_codec_dev_handle_t speaker, const void *buffer, size_t bytes,
                               size_t *written, uint32_t timeout_ms);

/* ORB_BTN_* bits for the buttons held right now. */
unsigned orb_board_buttons(void);

/* Physical edges are timestamped in the ISR so a complete short press cannot
 * disappear between service polls. notify runs in the ISR; it may notify a
 * task but must not invoke UI or semantic actions. Drain edges before polling. */
typedef struct { uint64_t at_us; unsigned buttons; } orb_board_button_edge_t;
esp_err_t orb_board_buttons_notify(void (*notify)(void *arg), void *arg);
bool orb_board_button_edge(orb_board_button_edge_t *out);
/* Sleep/recovery only: sample before and after restoring edges, including release
 * during the masked window. Caller signals without yielding. */
void orb_board_buttons_sleep_wake(void);

esp_err_t orb_board_power(orb_power_t *out);

esp_err_t orb_board_battery_detail(orb_battery_detail_t *out);
/* Opt-in setting; board service owns PMU I2C and calls desk_poll at least once
 * a second. This changes charger enable only, never charge current/voltage. */
esp_err_t orb_board_set_desk_charge_limit(bool enabled);
bool orb_board_desk_charge_limit(void);
esp_err_t orb_board_battery_desk_poll(bool resting, uint64_t now_ms);

/* Specific force in g, screen axes (+x right, +y up, +z out). At rest this
 * points away from Earth: +z face up, +y upright. */
esp_err_t orb_board_accel(orb_accel_t *out);
esp_err_t orb_board_motion(orb_motion_sample_t *out);

typedef enum {
    ORB_IMU_MODE_UNKNOWN,
    ORB_IMU_MODE_ACTIVE_224,
    ORB_IMU_MODE_ACCEL_250_TAP,
    ORB_IMU_MODE_ACCEL_21,
} orb_board_imu_mode_t;
/* Service owner only. Gesture consumers reset bias/accumulation when generation
 * changes. Accel-only tap mode keeps >200 Hz as required by the tap engine;
 * with taps disabled the low-power accelerometer runs at 21 Hz. */
esp_err_t orb_board_motion_configure(bool active, bool taps);
orb_board_imu_mode_t orb_board_motion_mode(void);
uint32_t orb_board_motion_generation(void);

/* Cuts every rail. PWR turns the device back on. */
esp_err_t orb_board_power_off(void);
void orb_board_usb_reattach(void);
