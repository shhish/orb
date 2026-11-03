/*
 * Developer access over USB or the paired development-channel hub link:
 * screenshots of what LVGL renders and injected touches, driven by
 * scripts/orb-dev.sh from the Mac.
 *
 * The design follows Muse Gadgets' bench screenshots (muse_ui.c
 * send_snapshot, tools/muse/snap.py; Copyright (c) Meta Platforms, Inc. and
 * affiliates, Apache License 2.0): render the screen into a snapshot only when
 * asked, so it costs nothing per frame, and send it as base64 lines through
 * the console. Here every line carries its index and the image a CRC, so log
 * lines that land in the middle can't corrupt it unnoticed.
 *
 * Commands, one per line on the console input:
 *   shot                      screenshot
 *   tap X Y                   press and release
 *   hold X Y MS               press for MS milliseconds
 *   swipe X1 Y1 X2 Y2 MS      drag over MS milliseconds
 *   arc R A0 A1 MS            circular drag; 0 degrees is top, clockwise positive
 *   tilt X Y MS               specific force in g for app input only, then expires
 *   top                       CPU use per task over the next second
 *   heap                      free memory by kind, largest block, low-water mark
 *   power                     PM configuration, locks and cumulative mode residency
 *   buttons                   last 16 raw/debounced button edges
 *   press pwr|boot short|double|hold   synthetic button navigation/feedback
 *   profile MS                capture LVGL ftrace (profiler-enabled builds only)
 *   bench_game ID MS          offline production game renderer; no score writes
 *   bench_nav GESTURE MS      cached real-shell navigation, completed DMA frames
 *
 * Starts itself at boot (no call from the app needed) and waits for LVGL to
 * have a display. Input is read straight from the USB-Serial-JTAG FIFO: the
 * console driver isn't installed, because with it, logging blocks whenever no
 * one on the other end is reading.
 */
#include <stdio.h>
#include <math.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "orb_light_sleep.h"
#include "orb_devtools_power.h"
#include "esp_private/startup_internal.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "lvgl.h"
#include "include/lvgl/draw/lv_draw_sw_profile.h"
#include "orb_display.h"
#include "orb_demo.h"
#include "orb_demo_fixture.h"
#include "orb_devtools.h"
#include "orb_devtools_wire.h"
#include "orb_devtools_diagnostics.h"
#include "orb_devtools_sender.h"
#include "orb_link.h"
#include "orb_open_source.h"
#include "orb_console_reply.h"
#include "orb_button_command.h"
#include "orb_devtools_input.h"
#include "orb_devtools_bench_power.h"
#include "orb_arc_command.h"
#include "orb_tilt_command.h"
#include "orb_scroll_trace_wire.h"
#include "orb_scroll_gates.h"
#include "orb_devtools_round.h"
#include "orb_mem.h"
#include "orb_memory.h"
#include "orb_model.h"
#include "orb_home.h"
#include "orb_nav_admission.h"
#include "orb_shell.h"
#include "orb_fonts.h"
#include "orb_ui.h"
#include "orb_input.h"
#include "orb_apps.h"
#include "orb_game_view.h"
#include "orb_scroll_fixture.h"
#include "orb_devtools_scroll_fixture.h"
#include "orb_games.h"
#include "orb_board.h"
#include "orb_sound.h"
#include "orb_voice.h"
#include "orb_net.h"
#include "esp_timer.h"

static const char *TAG = "devtools";

#define LINE_BYTES 192   /* raw bytes per base64 line: 256 characters */

/* Wi-Fi owns the same executor as USB. Only this task runs commands;
 * LVGL callbacks can emit receipts through a serialized, bounded sink. */
static SemaphoreHandle_t s_executor, s_output_lock, s_output_done;
static portMUX_TYPE s_wifi_lock = portMUX_INITIALIZER_UNLOCKED;
/* Request identity/command are snapshotted under s_wifi_lock. Output counters
 * belong to s_output_lock during execution; the executor owns their reset.
 * file is initialized before any command can publish active=true. */
static struct {
    _Atomic bool pending, active, failed, acknowledged, delivery_ok;
    uint32_t generation, stream, seq;
    _Atomic uint32_t ticket, delivered;
    size_t bytes;
    int64_t deadline;
    char command[ORB_DEVTOOLS_COMMAND_BYTES];
    FILE *file;
} s_wifi;
static uint32_t s_last_generation, s_last_stream;
static _Atomic bool s_bench_active;
static portMUX_TYPE s_buttons_lock = portMUX_INITIALIZER_UNLOCKED;
static orb_devtools_buttons_snapshot_fn s_buttons_snapshot;
static orb_devtools_buttons_inject_fn s_buttons_inject;
static void *s_buttons_ctx;

void orb_devtools_set_buttons(orb_devtools_buttons_snapshot_fn snapshot,
                             orb_devtools_buttons_inject_fn inject, void *ctx)
{
    portENTER_CRITICAL(&s_buttons_lock);
    s_buttons_snapshot = snapshot;
    s_buttons_inject = inject;
    s_buttons_ctx = ctx;
    portEXIT_CRITICAL(&s_buttons_lock);
}

static bool wifi_current(void)
{
    portENTER_CRITICAL(&s_wifi_lock);
    uint32_t generation = s_wifi.generation;
    int64_t deadline = s_wifi.deadline;
    portEXIT_CRITICAL(&s_wifi_lock);
    return orb_link_supports_devtools() && orb_link_generation() == generation &&
           esp_timer_get_time() < deadline;
}

static void wifi_written(esp_err_t err, const cJSON *result, const cJSON *error, void *ctx)
{
    (void)result; (void)error;
    portENTER_CRITICAL(&s_wifi_lock);
    if ((uint32_t)(uintptr_t)ctx == s_wifi.ticket) {
        s_wifi.delivered = (uint32_t)(uintptr_t)ctx;
        s_wifi.delivery_ok = err == ESP_OK;
        if (err != ESP_OK) s_wifi.failed = true;
    }
    portEXIT_CRITICAL(&s_wifi_lock);
    xSemaphoreGive(s_output_done);
}

static bool wifi_event_until(const char *name, cJSON *params, int64_t deadline_us)
{
    if (!params || !wifi_current() || esp_timer_get_time() >= deadline_us) { cJSON_Delete(params); return false; }
    xSemaphoreTake(s_output_done, 0);
    portENTER_CRITICAL(&s_wifi_lock);
    uint32_t ticket = ++s_wifi.ticket;
    if (!ticket) ticket = ++s_wifi.ticket;
    s_wifi.delivered = 0;
    uint32_t generation = s_wifi.generation;
    int64_t deadline = s_wifi.deadline;
    portEXIT_CRITICAL(&s_wifi_lock);
    /* Terminal events share the stream budget too, even when no per-receipt
     * deadline was supplied. Expire queued work before the stream can end. */
    if (deadline_us > deadline) deadline_us = deadline;
    if (orb_link_devtools_notify_until(generation, name, params, deadline_us, wifi_written,
                                      (void *)(uintptr_t)ticket) != ESP_OK) return false;
    int64_t until = esp_timer_get_time() + 6000000;
    if (until > deadline) until = deadline;
    if (until > deadline_us) until = deadline_us;
    while (esp_timer_get_time() < until) {
        portENTER_CRITICAL(&s_wifi_lock);
        bool delivered = s_wifi.delivered == ticket;
        portEXIT_CRITICAL(&s_wifi_lock);
        if (delivered) return wifi_current() && s_wifi.delivery_ok;
        int64_t remaining_us = until - esp_timer_get_time();
        if (remaining_us > 0) {
            uint32_t wait_ms = remaining_us / 1000;
            xSemaphoreTake(s_output_done, pdMS_TO_TICKS(wait_ms < 20 ? wait_ms : 20));
        }
    }
    return false;
}

static bool wifi_event(const char *name, cJSON *params)
{
    return wifi_event_until(name, params, INT64_MAX);
}

static bool wifi_text_until(const char *text, size_t size, int64_t deadline_us)
{
    if (!s_wifi.active || !size) return true;
    portENTER_CRITICAL(&s_wifi_lock);
    uint32_t stream = s_wifi.stream;
    if (deadline_us > s_wifi.deadline) deadline_us = s_wifi.deadline;
    portEXIT_CRITICAL(&s_wifi_lock);
    int64_t remaining_us = deadline_us - esp_timer_get_time();
    uint32_t wait_ms = remaining_us > 0 ? remaining_us / 1000 : 0;
    if (remaining_us <= 0 || xSemaphoreTake(s_output_lock,
        pdMS_TO_TICKS(wait_ms < 5000 ? wait_ms : 5000)) != pdTRUE) {
        s_wifi.failed = true;
        return false;
    }
    bool ok = !s_wifi.failed && wifi_current() && esp_timer_get_time() < deadline_us;
    if (orb_devtools_wire_contains(text, size, "@@ORBDEV ERROR") || orb_devtools_wire_contains(text, size, "@@ORBSHOT FAIL") || orb_devtools_wire_contains(text, size, "@@TOP ERROR"))
        s_wifi.failed = true;
    if (orb_devtools_wire_contains(text, size, "@@ORBDEV OK ")) s_wifi.acknowledged = true;
    while (size && ok) {
        /* Printable console text uses at most two JSON bytes per byte; 1500
         * leaves room for the envelope even for escaped tabs/newlines. */
        size_t part = size < 1500 ? size : 1500;
        /* Each event is valid UTF-8 even when a diagnostic spans envelopes. */
        if (part < size) {
            while (part && ((unsigned char)text[part] & 0xc0) == 0x80) part--;
        }
        if (!part) { ok = false; break; }
        if (s_wifi.bytes + part > ORB_DEVTOOLS_MAX_OUTPUT || s_wifi.seq >= ORB_DEVTOOLS_MAX_EVENTS) {
            ok = false;
            break;
        }
        cJSON *p = cJSON_CreateObject();
        char *fragment = heap_caps_malloc(part + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p || !fragment) { cJSON_Delete(p); heap_caps_free(fragment); ok = false; break; }
        memcpy(fragment, text, part); fragment[part] = '\0';
        bool fields = cJSON_AddNumberToObject(p, "stream", stream) &&
                      cJSON_AddNumberToObject(p, "seq", s_wifi.seq) &&
                      cJSON_AddStringToObject(p, "text", fragment);
        heap_caps_free(fragment);
        if (!fields) { cJSON_Delete(p); ok = false; break; }
        ok = wifi_event_until("devtools.output", p, deadline_us);
        if (ok) { s_wifi.seq++; s_wifi.bytes += part; text += part; size -= part; }
    }
    if (!ok) s_wifi.failed = true;
    xSemaphoreGive(s_output_lock);
    return ok;
}

static bool wifi_text(const char *text, size_t size)
{
    return wifi_text_until(text, size, INT64_MAX);
}

static int wifi_file_write(void *cookie, const char *text, int size)
{
    (void)cookie;
    return size >= 0 && wifi_text(text, (size_t)size) ? size : -1;
}

static int devtools_printf(const char *format, ...) __attribute__((format(__printf__, 1, 2)));
static int devtools_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (!s_wifi.active) {
        int size = vprintf(format, args);
        va_end(args);
        return size;
    }
    va_list copy;
    va_copy(copy, args);
    int size = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    /* The sink chunks long receipts within the event/stream budgets. A single
     * diagnostic line must not fail merely because it exceeds one envelope. */
    char *text = size >= 0 && (size_t)size <= ORB_DEVTOOLS_MAX_OUTPUT ?
                 heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!text) { s_wifi.failed = true; va_end(args); return -1; }
    vsnprintf(text, size + 1, format, args);
    va_end(args);
    bool ok = wifi_text(text, size);
    heap_caps_free(text);
    return ok ? size : -1;
}
#define printf devtools_printf

static TaskHandle_t s_command_task;

static esp_err_t wifi_request(const cJSON *params, cJSON *result, void *ctx)
{
    uint32_t stream;
    char command[ORB_DEVTOOLS_COMMAND_BYTES];
    if (!orb_devtools_wire_command(ctx, params, &stream, command)) return ESP_ERR_INVALID_ARG;
    uint32_t generation = orb_link_generation();
    if (generation == s_last_generation && stream <= s_last_stream) return ESP_ERR_INVALID_ARG;
    if (!s_executor || xSemaphoreTake(s_executor, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    bool active_benchmark = s_bench_active;
    if (active_benchmark) { xSemaphoreGive(s_executor); return ESP_ERR_INVALID_STATE; }
    if (!cJSON_AddBoolToObject(result, "accepted", true)) {
        xSemaphoreGive(s_executor);
        return ESP_ERR_NO_MEM;
    }
    s_last_generation = generation; s_last_stream = stream;
    portENTER_CRITICAL(&s_wifi_lock);
    s_wifi.generation = generation; s_wifi.stream = stream;
    s_wifi.deadline = esp_timer_get_time() + ORB_DEVTOOLS_DEADLINE_US;
    memcpy(s_wifi.command, command, sizeof(command));
    s_wifi.pending = true;
    portEXIT_CRITICAL(&s_wifi_lock);
    if (s_command_task) xTaskNotifyGive(s_command_task);
    return ESP_OK;
}

static void wifi_hello(cJSON *hello, void *ctx)
{
    (void)ctx;
    /* The hub grants access from its saved channel; support is known at boot. */
    cJSON *caps = cJSON_GetObjectItemCaseSensitive(hello, "caps");
    if (!cJSON_AddItemToArray(caps, cJSON_CreateString("devtools"))) cJSON_DeleteItemFromObjectCaseSensitive(hello, "caps");
}

static size_t reply_write(void *ctx, const uint8_t *data, size_t size)
{
    return usb_serial_jtag_ll_write_txfifo(data, size > 64 ? 64 : size);
}

static bool reply_writable(void *ctx) { return usb_serial_jtag_ll_txfifo_writable(); }
static void reply_flush(void *ctx) { usb_serial_jtag_ll_txfifo_flush(); }
static int64_t reply_now(void *ctx) { return esp_timer_get_time(); }
static void reply_yield(void *ctx) { vTaskDelay(1); }

static bool command_reply_vprintf(int64_t deadline_us, const char *format, va_list args)
{
    /* Allocated only after a sample finishes, explicitly outside internal RAM
     * and its benchmark low-water mark. Idle logs retain the nonblocking VFS. */
    char small[128];
    char *allocated = NULL, *line = small;
    va_list copy;
    va_copy(copy, args);
    int size = vsnprintf(small, sizeof(small), format, copy);
    va_end(copy);
    bool valid = size > 0 && size < 4096;
    if (valid && (size_t)size >= sizeof(small)) {
        allocated = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        valid = allocated != NULL;
        if (valid) {
            line = allocated;
            va_copy(copy, args);
            vsnprintf(line, size + 1, format, copy);
            va_end(copy);
        }
    }
    if (!valid) {
        /* Even a failed report must terminate the requester instead of looking
         * like a lost ACK. This short failure needs no heap allocation. */
        line = "\n@@ORBDEV ERROR reply: allocation or oversized receipt\n";
        size = strlen(line);
    }
    if (s_wifi.active) {
        /* Trace emission has one deadline across all of its receipts; moving
         * that receipt to Wi-Fi must not restart its backpressure budget. */
        bool sent = wifi_text_until(line, size, deadline_us);
        heap_caps_free(allocated);
        return sent && valid;
    }
    const orb_console_reply_io_t io = {
        .write = reply_write, .writable = reply_writable, .flush = reply_flush,
        .now_us = reply_now, .yield = reply_yield,
    };
    /* ESP logging uses stdout too. Keep a complete receipt indivisible,
     * including its short-packet terminator, without installing a driver. */
    flockfile(stdout);
    fflush(stdout);
    bool sent = orb_console_reply_write(&io, (const uint8_t *)line, size, deadline_us);
    funlockfile(stdout);
    heap_caps_free(allocated);
    return sent && valid;
}

static bool command_reply_printf_until(int64_t deadline_us, const char *format, ...)
    __attribute__((format(__printf__, 2, 3)));

static bool command_reply_printf_until(int64_t deadline_us, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    bool sent = command_reply_vprintf(deadline_us, format, args);
    va_end(args);
    return sent;
}

static bool command_reply_printf(const char *format, ...) __attribute__((format(__printf__, 1, 2)));

static bool command_reply_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    bool sent = command_reply_vprintf(esp_timer_get_time() + 2000000, format, args);
    va_end(args);
    return sent;
}

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int32_t x, y;
    bool pressed;
} s_touch;

static void set_touch(int32_t x, int32_t y, bool pressed)
{
    portENTER_CRITICAL(&s_lock);
    s_touch.x = x;
    s_touch.y = y;
    s_touch.pressed = pressed;
    portEXIT_CRITICAL(&s_lock);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    portENTER_CRITICAL(&s_lock);
    data->point.x = s_touch.x;
    data->point.y = s_touch.y;
    data->state = s_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    portEXIT_CRITICAL(&s_lock);
}

static size_t base64(const uint8_t *in, size_t n, char *out)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = a[v >> 18 & 63];
        out[o++] = a[v >> 12 & 63];
        out[o++] = i + 1 < n ? a[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? a[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* Blends the top layer (ARGB8888) over the screen (RGB565), for overlays. */
/* Screenshots come straight from PSRAM, outside the LVGL heap's owner
 * budget. A settled UI can leave no single 434 KB block, so the frame lives
 * in parts small enough to fit even a fragmented heap. */
#define SHOT_PARTS 4
typedef struct {
    lv_draw_buf_t *part[SHOT_PARTS];
    uint32_t w, h, part_rows;
} shot_t;

static lv_draw_buf_t *shot_part_alloc(uint32_t w, uint32_t h, lv_color_format_t cf)
{
    uint32_t stride = lv_draw_buf_width_to_stride(w, cf);
    lv_draw_buf_t *buf = heap_caps_calloc(1, sizeof(*buf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *data = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, stride * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !data || lv_draw_buf_init(buf, w, h, cf, stride, data, stride * h) != LV_RESULT_OK) {
        heap_caps_free(data);
        heap_caps_free(buf);
        return NULL;
    }
    return buf;
}

static void shot_free(shot_t *shot)
{
    if (!shot) return;
    for (unsigned i = 0; i < SHOT_PARTS; i++) {
        if (!shot->part[i]) continue;
        heap_caps_free(shot->part[i]->unaligned_data);
        heap_caps_free(shot->part[i]);
    }
    heap_caps_free(shot);
}

static shot_t *shot_alloc(lv_obj_t *obj)
{
    lv_display_t *disp = lv_obj_get_display(obj);
    shot_t *shot = heap_caps_calloc(1, sizeof(*shot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!shot) return NULL;
    shot->w = lv_display_get_horizontal_resolution(disp);
    shot->h = lv_display_get_vertical_resolution(disp);
    shot->part_rows = shot->h / SHOT_PARTS;
    for (unsigned i = 0; i < SHOT_PARTS; i++) {
        uint32_t rows = i == SHOT_PARTS - 1 ? shot->h - i * shot->part_rows : shot->part_rows;
        shot->part[i] = shot_part_alloc(shot->w, rows, LV_COLOR_FORMAT_RGB565);
        if (!shot->part[i]) { shot_free(shot); return NULL; }
    }
    return shot;
}

static bool shot_take(shot_t *shot, lv_obj_t *obj)
{
    for (unsigned i = 0; i < SHOT_PARTS; i++) {
        int32_t y0 = (int32_t)(i * shot->part_rows);
        lv_area_t area = {0, y0, (int32_t)shot->w - 1, y0 + (int32_t)shot->part[i]->header.h - 1};
        if (lv_snapshot_take_area_to_draw_buf(obj, LV_COLOR_FORMAT_RGB565, &area, shot->part[i]) != LV_RESULT_OK)
            return false;
    }
    return true;
}

static void blend_top(lv_draw_buf_t *screen, const lv_draw_buf_t *top, uint32_t y0)
{
    for (uint32_t y = 0; y0 + y < screen->header.h && y < top->header.h; y++) {
        uint16_t *d = (uint16_t *)(screen->data + (y0 + y) * screen->header.stride);
        const uint8_t *s = top->data + y * top->header.stride;
        for (uint32_t x = 0; x < screen->header.w && x < top->header.w; x++, s += 4) {
            uint32_t a = s[3];
            if (!a) {
                continue;
            }
            uint32_t r = (d[x] >> 11) << 3, g = ((d[x] >> 5) & 63) << 2, b = (d[x] & 31) << 3;
            r = (s[2] * a + r * (255 - a)) / 255;
            g = (s[1] * a + g * (255 - a)) / 255;
            b = (s[0] * a + b * (255 - a)) / 255;
            d[x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
        }
    }
}

static SemaphoreHandle_t s_captured;
static shot_t *s_capture;

/* Runs in the LVGL task (as Muse's snapshots do), between refreshes. */
static void capture(void *arg)
{
    if (s_wifi.active && !wifi_current()) {
        s_wifi.failed = true;
        s_capture = NULL;
        xSemaphoreGive(s_captured);
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    shot_t *shot = shot_alloc(scr);
    if (!shot) {
        /* Caches may fill PSRAM down to their margin in small blocks. Trim
         * them and retry once. */
        orb_memory_cache_trim(640 * 1024);
        shot = shot_alloc(scr);
    }
    if (shot && !shot_take(shot, scr)) {
        shot_free(shot);
        shot = NULL;
    }
    if (shot && lv_obj_get_child_count(lv_layer_top()) > 0) {
        /* A whole ARGB8888 top layer is 868 KB, more than PSRAM's largest
         * free block, so overlays such as Always On never reached a
         * screenshot. Render and blend it in 96-row bands instead. */
        enum { BAND = 96 };
        uint32_t w = shot->w, stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888);
        lv_draw_buf_t *band = heap_caps_calloc(1, sizeof(*band), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        void *data = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, stride * BAND, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (band && data) {
            for (uint32_t y0 = 0; y0 < shot->h; ) {
                unsigned i = LV_MIN(y0 / shot->part_rows, SHOT_PARTS - 1);
                uint32_t start = i * shot->part_rows, end = start + shot->part[i]->header.h;
                lv_area_t area = {0, (int32_t)y0, (int32_t)w - 1, (int32_t)LV_MIN(y0 + BAND, end) - 1};
                if (lv_draw_buf_init(band, w, lv_area_get_height(&area), LV_COLOR_FORMAT_ARGB8888, stride,
                                     data, stride * BAND) != LV_RESULT_OK ||
                    lv_snapshot_take_area_to_draw_buf(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888, &area,
                                                      band) != LV_RESULT_OK) break;
                blend_top(shot->part[i], band, y0 - start);
                y0 = (uint32_t)area.y2 + 1;
            }
        }
        heap_caps_free(data);
        heap_caps_free(band);
    }
    uint16_t px0 = 0;
    if (shot) {
        unsigned i = LV_MIN(233 / shot->part_rows, SHOT_PARTS - 1);
        const lv_draw_buf_t *mid = shot->part[i];
        px0 = ((const uint16_t *)mid->data)[(233 - i * shot->part_rows) * mid->header.stride / 2 + 233];
    }
    fprintf(stdout, "\n@@ORBDEV shot scr=%p children=%u bg=%06lx px0=%04x\n", (void *)scr,
           (unsigned)lv_obj_get_child_count(scr), (unsigned long)lv_color_to_u32(lv_obj_get_style_bg_color(scr, 0)) & 0xFFFFFF,
           px0);
    if (px0 == 0xFFFF) {
        /* Board reviews occasionally return a solid white disc that the panel
         * never showed. Name every visible top-level object so the next one
         * identifies its source. */
        lv_obj_t *layers[] = {scr, lv_layer_top()};
        for (unsigned l = 0; l < 2; l++) {
            for (uint32_t i = 0; i < lv_obj_get_child_count(layers[l]); i++) {
                lv_obj_t *o = lv_obj_get_child(layers[l], i);
                if (lv_obj_is_hidden(o)) continue;
                lv_area_t a;
                lv_obj_get_coords(o, &a);
                fprintf(stdout, "@@ORBDEV white %s[%lu] %p %ldx%ld@%ld,%ld bg=%06lx opa=%u/%u\n",
                        l ? "top" : "scr", (unsigned long)i, (void *)o, (long)lv_area_get_width(&a),
                        (long)lv_area_get_height(&a), (long)a.x1, (long)a.y1,
                        (unsigned long)lv_color_to_u32(lv_obj_get_style_bg_color(o, 0)) & 0xFFFFFF,
                        (unsigned)lv_obj_get_style_bg_opa(o, 0), (unsigned)lv_obj_get_style_opa(o, 0));
            }
        }
    }
    s_capture = shot;
    xSemaphoreGive(s_captured);
}

static void send_shot(void) __attribute__((noinline));

static void send_shot(void)
{
    lv_lock();
    xSemaphoreTake(s_captured, 0);
    s_capture = NULL;
    bool queued = lv_async_call(capture, NULL) == LV_RESULT_OK;
    if (queued) orb_display_wake_service();
    lv_unlock();
    if (!queued || xSemaphoreTake(s_captured, pdMS_TO_TICKS(5000)) != pdTRUE) {
        lv_lock();
        lv_async_call_cancel(capture, NULL);
        /* The callback can finish while timeout cleanup waits for LVGL. It
         * must not leave an old image/completion for the next screenshot. */
        xSemaphoreTake(s_captured, 0);
        shot_free(s_capture);
        s_capture = NULL;
        lv_unlock();
        printf("\n@@ORBSHOT FAIL timeout\n");
        return;
    }
    shot_t *shot = s_capture;
    s_capture = NULL;
    if (!shot) {
        printf("\n@@ORBSHOT FAIL no memory\n");
        return;
    }

    /* Pack rows tightly (the snapshot's stride may be padded). */
    uint32_t w = shot->w, h = shot->h, row = w * 2, crc = 0;
    for (unsigned p = 0; p < SHOT_PARTS; p++) {
        lv_draw_buf_t *buf = shot->part[p];
        if (buf->header.stride != row) {
            for (uint32_t y = 1; y < buf->header.h; y++) {
                memmove(buf->data + y * row, buf->data + y * buf->header.stride, row);
            }
        }
        crc = esp_rom_crc32_le(crc, buf->data, row * buf->header.h);
    }
    uint32_t len = row * h, part_len = row * shot->part_rows;

    char *line = heap_caps_malloc(LINE_BYTES * 4 / 3 + 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *chunk = heap_caps_malloc(LINE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line || !chunk) {
        printf("\n@@ORBSHOT FAIL no memory\n");
        heap_caps_free(line);
        heap_caps_free(chunk);
        shot_free(shot);
        return;
    }
    printf("\n@@ORBSHOT BEGIN %lu %lu rgb565le %d %lu %08lx\n", (unsigned long)w, (unsigned long)h, LINE_BYTES,
           (unsigned long)len, (unsigned long)crc);
    char *batch = s_wifi.active ? heap_caps_malloc(1500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    size_t used = 0;
    if (s_wifi.active && !batch) s_wifi.failed = true;
    for (uint32_t off = 0, i = 0; off < len; off += LINE_BYTES, i++) {
        if (s_wifi.active && (!wifi_current() || s_wifi.failed)) break;
        size_t want = len - off < LINE_BYTES ? len - off : LINE_BYTES, got = 0;
        while (got < want) {
            size_t at = off + got;
            unsigned p = LV_MIN(at / part_len, SHOT_PARTS - 1);
            size_t inner = at - p * part_len, n = row * shot->part[p]->header.h - inner;
            if (n > want - got) n = want - got;
            memcpy(chunk + got, shot->part[p]->data + inner, n);
            got += n;
        }
        base64(chunk, want, line);
        if (batch) {
            if (used + 280 >= 1500) { printf("%s", batch); used = 0; }
            used += snprintf(batch + used, 1500 - used, "@@%05lx %s\n", (unsigned long)i, line);
        } else printf("@@%05lx %s\n", (unsigned long)i, line);
    }
    if (used) printf("%s", batch);
    heap_caps_free(batch);
    printf("@@ORBSHOT END\n");
    fflush(stdout);
    heap_caps_free(line);
    heap_caps_free(chunk);
    shot_free(shot);
}

static void print_power(void)
{
    orb_devtools_power_report(s_wifi.active ? s_wifi.file : stdout, devtools_printf);
}

static orb_shell_t *s_shell;

/* Wake can synchronously prepare a cold Face. Run that ordinary guarded
 * power transition on the UI task, never on the 4 KiB command stack. The
 * serialized executor owns this static request until completion or cancellation. */
static SemaphoreHandle_t s_wake_done;
static struct {
    bool navigate, accepted;
    orb_shell_surface_t surface;
} s_wake;

static void wake_on_ui(void *context)
{
    (void)context;
    s_wake.accepted = s_wake.navigate ? orb_devtools_input_goto(s_shell, s_wake.surface) :
        orb_devtools_input_wake(s_shell);
    xSemaphoreGive(s_wake_done);
}

static bool request_wake(bool navigate, orb_shell_surface_t surface)
{
    if (!s_wake_done) return false;
    lv_lock();
    xSemaphoreTake(s_wake_done, 0);
    s_wake.navigate = navigate;
    s_wake.surface = surface;
    s_wake.accepted = false;
    bool queued = lv_async_call(wake_on_ui, NULL) == LV_RESULT_OK;
    if (queued) orb_display_wake_service();
    lv_unlock();
    if (!queued) return false;
    bool completed = xSemaphoreTake(s_wake_done, pdMS_TO_TICKS(5000)) == pdTRUE;
    lv_lock();
    /* Taking the UI lock joins a callback that raced the timeout. Its static
     * request remains alive, but a late result cannot acknowledge this command
     * or the next one. Cancellation also removes callbacks not started yet. */
    if (!completed) lv_async_call_cancel(wake_on_ui, NULL);
    bool accepted = completed && s_wake.accepted;
    xSemaphoreTake(s_wake_done, 0);
    lv_unlock();
    return accepted;
}

static bool wake_display(void) { return request_wake(false, ORB_SHELL_FACE); }

static void glide(int x1, int y1, int x2, int y2, int ms)
{
    wake_display();
    int steps = ms / 10 > 1 ? ms / 10 : 1;
    for (int i = 0; i <= steps; i++) {
        if (s_wifi.active && !wifi_current()) { s_wifi.failed = true; break; }
        set_touch(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, true);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void glide_arc(const orb_devtools_arc_t *arc)
{
    wake_display();
    int steps = arc->duration_ms / 10 > 1 ? arc->duration_ms / 10 : 1;
    int x = 233, y = 233;
    for (int i = 0; i <= steps; i++) {
        if (s_wifi.active && !wifi_current()) { s_wifi.failed = true; break; }
        orb_devtools_arc_point(arc, (float)i / steps, &x, &y);
        set_touch(x, y, true);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    set_touch(x, y, false);
}

/* A bounded, repeatable live scene with networking still active. The app's
 * existing screen and settings survive; no credentials or data are changed. */
static SemaphoreHandle_t s_bench_done;
static lv_timer_t *s_bench_timer;
static bool s_bench_failed;
static int s_bench_ms;
static enum { BENCH_ORIGINAL, BENCH_TRANSITION, BENCH_QUIET, BENCH_SCROLL } s_bench_fixture;
static int64_t s_bench_started, s_bench_tick;
static lv_obj_t *s_bench_screen, *s_saved_screen;
static bool s_saved_reduce_motion;
static const char *s_bench_phase;
/* Shared by sequential benchmark scenes, bounded and retained in PSRAM. */
typedef struct {
    orb_display_counters_t display;
    lv_draw_sw_blur_cache_stats_t cache;
    lv_obj_raster_scale_cache_stats_t scale;
} bench_snapshots_t;
static bench_snapshots_t *s_bench_snapshots;

/* Scroll snapshots are bounded PSRAM metadata. CPU residency uses pinned idle
 * tasks, not affinity labels of migratable worker tasks. No console I/O runs
 * during the timed fling/settle sample. */
typedef struct {
    orb_scroll_fixture_t *fixture;
    orb_scroll_surface_t surface;
    bool ready, previous_moving;
    int64_t warm_started, previous_us;
    uint64_t moving_us, moving_frames, idle_frames;
    orb_display_counters_t initial, previous;
    orb_display_cadence_t cadence;
    orb_ui_scroll_diagnostics_t ui_diagnostics;
    orb_scroll_fixture_diagnostics_t input_diagnostics;
    lv_obj_raster_stats_t raster_initial, raster_now;
    lv_obj_raster_rows_stats_t rows_initial, rows_now;
    orb_display_accounting_t account, account_now;
    orb_display_cpu_snapshot_t cpu, cpu_after, moving_cpu_start;
    multi_heap_info_t memory[3];
    bool memory_active;
    double moving_cpu_work[2];
    uint64_t moving_cpu_us;
    bool moving_cpu_valid;
    int64_t settled_since;
} scroll_bench_t;
static scroll_bench_t *s_scroll_bench;

#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
#define BENCH_ACCOUNT_PHASES 8
/* The draw profiler and this receipt share the architecture's 64 KiB PSRAM
 * metadata budget. No phase snapshots live on the small console/UI stacks. */
typedef struct {
    const char *phase;
    orb_display_accounting_t counters;
    lv_obj_raster_stats_t raster;
    lv_draw_sw_material_stats_t material;
    size_t largest_internal, largest_dma, largest_psram, scratch_peak;
    uint64_t band_start;
} bench_account_phase_t;
typedef struct {
    orb_display_accounting_t previous, now;
    lv_obj_raster_stats_t raster;
    lv_draw_sw_material_stats_t material;
    bench_account_phase_t phases[BENCH_ACCOUNT_PHASES];
    unsigned count;
    bool overflow;
    uint64_t cold_prepare_us;
    uint32_t cold_source_builds, cold_mip_builds;
} bench_account_t;
static bench_account_t *s_bench_account;

static void benchmark_account_sample(void)
{
    if (!s_bench_account) return;
    bench_account_t *account = s_bench_account;
    if (account->count == BENCH_ACCOUNT_PHASES) {
        account->overflow = true;
        return;
    }
    bench_account_phase_t *phase = &account->phases[account->count++];
    phase->phase = s_bench_phase;
    orb_display_accounting(&account->now);
    phase->counters = account->now;
    phase->band_start = account->previous.band_sequence;
#define DELTA(field) phase->counters.field -= account->previous.field
    DELTA(scheduled); DELTA(completed); DELTA(dropped); DELTA(preparation_us);
    DELTA(whole_update_us); DELTA(scene_prepare_us); DELTA(cold_prepare_us);
    DELTA(paced_scheduled); DELTA(paced_on_slot); DELTA(paced_late); DELTA(paced_skipped);
    DELTA(paced_boundaries); DELTA(paced_unpaced); DELTA(paced_admitted_30);
    for (unsigned i = 0; i < ORB_DISPLAY_CADENCE_BUCKETS; i++) {
        phase->counters.completion_interval[i] -= account->previous.completion_interval[i];
        phase->counters.whole_update[i] -= account->previous.whole_update[i];
    }
#undef DELTA
    account->previous = account->now;
    lv_obj_raster_get_stats(&phase->raster);
    uint32_t builds = phase->raster.builds;
    uint32_t mip_builds = phase->raster.mip_builds;
    phase->raster.builds -= account->raster.builds;
    phase->raster.mip_builds -= account->raster.mip_builds;
    account->raster.builds = builds;
    account->raster.mip_builds = mip_builds;
    lv_draw_sw_material_get_stats(&phase->material);
#define DELTA(field) phase->material.field = (uint32_t)(phase->material.field - account->material.field)
    DELTA(map_hits); DELTA(map_builds); DELTA(base_hits); DELTA(base_builds);
    DELTA(source_recaptures); DELTA(blur_kernels); DELTA(fallbacks); DELTA(preparation_us);
#undef DELTA
    phase->material.solid_fills -= account->material.solid_fills;
    phase->material.moving_bypasses -= account->material.moving_bypasses;
    lv_draw_sw_material_get_stats(&account->material);
    phase->largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    phase->largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    phase->largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    lv_draw_scratch_stats_t scratch;
    lv_draw_scratch_get_stats(&scratch);
    phase->scratch_peak = scratch.peak_internal;
}

static void benchmark_account_report(void)
{
    if (!s_bench_account) return;
    printf("@@ORBDEV ACCOUNT_META clock=wall_us active=%d interval_bucket_us=1000 buckets=%u last_unbounded=1 trace_capacity=%u overflow=%d cold_prepare_us=%llu cold_source_builds=%lu cold_mip_builds=%lu\n",
           orb_display_accounting_active(), ORB_DISPLAY_CADENCE_BUCKETS, ORB_DISPLAY_BAND_TRACE, s_bench_account->overflow,
           (unsigned long long)s_bench_account->cold_prepare_us,
           (unsigned long)s_bench_account->cold_source_builds,
           (unsigned long)s_bench_account->cold_mip_builds);
    for (unsigned p = 0; p < s_bench_account->count; p++) {
        const bench_account_phase_t *phase = &s_bench_account->phases[p];
        const orb_display_accounting_t *a = &phase->counters;
        printf("@@ORBDEV ACCOUNT phase=%s scheduled=%llu completed=%llu dropped=%llu preparation_us=%llu scene_prepare_us=%llu whole_update_us=%llu source_builds=%lu mip_builds=%lu mip_bytes=%lu mip_budget=%lu largest_internal=%u largest_dma=%u largest_psram=%u scratch_peak=%u trace_omitted=%llu\n",
               phase->phase, (unsigned long long)a->scheduled, (unsigned long long)a->completed,
               (unsigned long long)a->dropped, (unsigned long long)a->preparation_us,
               (unsigned long long)a->scene_prepare_us,
               (unsigned long long)a->whole_update_us, (unsigned long)phase->raster.builds,
               (unsigned long)phase->raster.mip_builds, (unsigned long)phase->raster.mip_bytes,
               (unsigned long)phase->raster.mip_budget,
               (unsigned)phase->largest_internal, (unsigned)phase->largest_dma,
               (unsigned)phase->largest_psram, (unsigned)phase->scratch_peak,
               (unsigned long long)(a->band_sequence - phase->band_start > ORB_DISPLAY_BAND_TRACE ?
                   a->band_sequence - phase->band_start - ORB_DISPLAY_BAND_TRACE : 0));
        printf("@@ORBDEV PACING phase=%s scheduled=%llu on_slot=%llu late=%llu skipped=%llu boundaries=%llu unpaced=%llu admitted_30=%llu period_us=%lu divisor=%lu max_error_us=%lu\n",
               phase->phase, (unsigned long long)a->paced_scheduled, (unsigned long long)a->paced_on_slot,
               (unsigned long long)a->paced_late, (unsigned long long)a->paced_skipped,
               (unsigned long long)a->paced_boundaries, (unsigned long long)a->paced_unpaced,
               (unsigned long long)a->paced_admitted_30, (unsigned long)a->paced_period_us,
               (unsigned long)a->paced_divisor, (unsigned long)a->paced_max_error_us);
        printf("@@ORBDEV MATERIAL phase=%s map_hits=%llu map_builds=%llu base_hits=%llu base_builds=%llu recaptures=%llu kernels=%llu fallbacks=%llu preparation_us=%llu bytes=%u peak=%u budget=%u entries=%lu solid_fills=%llu moving_bypasses=%llu\n",
               phase->phase, (unsigned long long)phase->material.map_hits,
               (unsigned long long)phase->material.map_builds, (unsigned long long)phase->material.base_hits,
               (unsigned long long)phase->material.base_builds, (unsigned long long)phase->material.source_recaptures,
               (unsigned long long)phase->material.blur_kernels, (unsigned long long)phase->material.fallbacks,
               (unsigned long long)phase->material.preparation_us,
               (unsigned)phase->material.bytes, (unsigned)phase->material.peak_bytes,
               (unsigned)phase->material.budget_bytes, (unsigned long)phase->material.entries,
               (unsigned long long)phase->material.solid_fills, (unsigned long long)phase->material.moving_bypasses);
        for (unsigned i = 0; i < ORB_DISPLAY_CADENCE_BUCKETS; i++) {
            if (a->completion_interval[i] || a->whole_update[i])
                printf("@@ORBDEV CADENCE phase=%s bucket=%u intervals=%lu whole_updates=%lu\n", phase->phase, i,
                       (unsigned long)a->completion_interval[i], (unsigned long)a->whole_update[i]);
        }
        uint64_t count = a->band_sequence - phase->band_start;
        if (count > ORB_DISPLAY_BAND_TRACE) count = ORB_DISPLAY_BAND_TRACE;
        for (uint64_t i = a->band_sequence - count; i < a->band_sequence; i++) {
            const orb_display_band_trace_t *b = &a->bands[i % ORB_DISPLAY_BAND_TRACE];
            printf("@@ORBDEV BAND phase=%s seq=%llu draw_us=%lld queue_us=%lld sender_us=%lld dma_us=%lld done_us=%lld bytes=%lu y1=%d y2=%d ok=%d\n",
                   phase->phase, (unsigned long long)i, (long long)b->draw_start_us, (long long)b->queued_us,
                   (long long)b->sender_us, (long long)b->dma_start_us, (long long)b->dma_end_us,
                   (unsigned long)b->bytes, b->y1, b->y2, b->ok);
        }
    }
    heap_caps_free(s_bench_account);
    s_bench_account = NULL;
}
#endif


#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
/* At most eight 8-second phases fit a 60-second benchmark. Keep extra slots
 * for a phase-boundary sample, and report overflow rather than lose evidence. */
#define BENCH_PROFILE_PHASES 16
typedef struct {
    const char *phase;
    uint64_t draw_us, frames;
    lv_draw_sw_profile_snapshot_t tasks;
    lv_obj_raster_stats_t raster;
    lv_obj_raster_rows_stats_t row_atlas;
    orb_glass_fused_stats_t fused;
} bench_profile_phase_t;
typedef struct {
    lv_draw_sw_profile_snapshot_t previous, current;
    lv_obj_raster_stats_t raster;
    lv_obj_raster_rows_stats_t row_atlas;
    orb_glass_fused_stats_t fused;
    unsigned count;
    bool overflow;
    bench_profile_phase_t phases[BENCH_PROFILE_PHASES];
} bench_profile_t;
static bench_profile_t *s_bench_profile;

static void benchmark_profile_sample(const orb_display_counters_t *now)
{
    if (s_bench_profile->count == BENCH_PROFILE_PHASES) {
        s_bench_profile->overflow = true;
        return;
    }
    bench_profile_phase_t *phase = &s_bench_profile->phases[s_bench_profile->count++];
    phase->phase = s_bench_phase;
    phase->draw_us = now->draw_us - s_bench_snapshots->display.draw_us;
    phase->frames = now->frames - s_bench_snapshots->display.frames;
    lv_draw_sw_profile_snapshot(&s_bench_profile->current);
    for (unsigned i = 0; i < LV_DRAW_SW_PROFILE_SLOTS; i++) {
        phase->tasks.rows[i].calls = s_bench_profile->current.rows[i].calls - s_bench_profile->previous.rows[i].calls;
        phase->tasks.rows[i].elapsed_us = s_bench_profile->current.rows[i].elapsed_us - s_bench_profile->previous.rows[i].elapsed_us;
    }
    for (unsigned i = 0; i < LV_DRAW_SW_PROFILE_STAGE_COUNT; i++) {
        phase->tasks.stages[i].calls = s_bench_profile->current.stages[i].calls - s_bench_profile->previous.stages[i].calls;
        phase->tasks.stages[i].elapsed_us = s_bench_profile->current.stages[i].elapsed_us - s_bench_profile->previous.stages[i].elapsed_us;
        phase->tasks.stages[i].pixels = s_bench_profile->current.stages[i].pixels - s_bench_profile->previous.stages[i].pixels;
    }
    s_bench_profile->previous = s_bench_profile->current;
    lv_obj_raster_get_stats(&phase->raster);
    uint32_t builds = phase->raster.builds;
    uint32_t mip_builds = phase->raster.mip_builds;
    phase->raster.builds -= s_bench_profile->raster.builds;
    phase->raster.mip_builds -= s_bench_profile->raster.mip_builds;
    s_bench_profile->raster.builds = builds;
    s_bench_profile->raster.mip_builds = mip_builds;
    lv_obj_raster_rows_stats_t atlas;
    lv_obj_raster_rows_get_stats(&atlas);
    phase->row_atlas = atlas;
    phase->row_atlas.builds -= s_bench_profile->row_atlas.builds;
    phase->row_atlas.fallbacks -= s_bench_profile->row_atlas.fallbacks;
    phase->row_atlas.draws -= s_bench_profile->row_atlas.draws;
    phase->row_atlas.pixels -= s_bench_profile->row_atlas.pixels;
    s_bench_profile->row_atlas = atlas;
    orb_glass_fused_stats_t fused;
    orb_glass_fused_get_stats(&fused);
    phase->fused = fused;
    /* Native 32-bit atomics avoid cross-core locks in the fused kernel. */
    phase->fused.prepares = (uint32_t)(fused.prepares - s_bench_profile->fused.prepares);
    phase->fused.draws = (uint32_t)(fused.draws - s_bench_profile->fused.draws);
    phase->fused.fallbacks = (uint32_t)(fused.fallbacks - s_bench_profile->fused.fallbacks);
    s_bench_profile->fused = fused;
}

static void benchmark_profile_report(void)
{
    /* The console task prints only after the timed scene has finished. Raster
     * preparation precedes RENDER_START; task sums include that work and both
     * workers, while DRAW is elapsed refresh time. They need not sum to 100%. */
    printf("@@ORBDEV DRAWPROFILE_META clock=wall_us slots=%u bytes=%u overflow=%u stages=%u\n",
           LV_DRAW_SW_PROFILE_SLOTS, (unsigned)sizeof(*s_bench_profile), s_bench_profile->overflow,
           LV_DRAW_SW_PROFILE_STAGE_COUNT);
    for (unsigned p = 0; p < s_bench_profile->count; p++) {
        const bench_profile_phase_t *phase = &s_bench_profile->phases[p];
        printf("@@ORBDEV DRAWPROFILE phase=%s draw_us=%llu frames=%llu\n", phase->phase,
               (unsigned long long)phase->draw_us, (unsigned long long)phase->frames);
        for (unsigned i = 0; i < LV_DRAW_SW_PROFILE_SLOTS; i++) {
            const lv_draw_sw_profile_counter_t *row = &phase->tasks.rows[i];
            if (!row->calls) continue;
            printf("@@ORBDEV DRAWTASK phase=%s type=%s%s class=%s calls=%lu elapsed_us=%lu\n",
                   phase->phase, lv_draw_sw_profile_type(i), lv_draw_sw_profile_transformed(i) ? "_TRANSFORM" : "",
                   lv_draw_sw_profile_class(i), (unsigned long)row->calls, (unsigned long)row->elapsed_us);
        }
        /* Include zero rows: a warmed light-only update with no preparation is
         * evidence, and missing instrumentation must not resemble zero work. */
        for (unsigned i = 0; i < LV_DRAW_SW_PROFILE_STAGE_COUNT; i++) {
            const lv_draw_sw_profile_stage_counter_t *stage = &phase->tasks.stages[i];
            printf("@@ORBDEV HOTSPOT phase=%s stage=%s calls=%lu elapsed_us=%lu pixels=%lu\n",
                   phase->phase, lv_draw_sw_profile_stage_name(i), (unsigned long)stage->calls,
                   (unsigned long)stage->elapsed_us, (unsigned long)stage->pixels);
        }
        printf("@@ORBDEV RASTER phase=%s builds=%lu count=%lu bytes=%lu mip_builds=%lu mip_bytes=%lu mip_budget=%lu\n", phase->phase,
               (unsigned long)phase->raster.builds, (unsigned long)phase->raster.count,
               (unsigned long)phase->raster.bytes, (unsigned long)phase->raster.mip_builds,
               (unsigned long)phase->raster.mip_bytes, (unsigned long)phase->raster.mip_budget);
        printf("@@ORBDEV ROWATLAS phase=%s builds=%lu fallbacks=%lu draws=%lu pixels=%lu "
               "rows=%lu glyphs=%lu bytes=%lu peak=%lu budget=%lu\n", phase->phase,
               (unsigned long)phase->row_atlas.builds, (unsigned long)phase->row_atlas.fallbacks,
               (unsigned long)phase->row_atlas.draws, (unsigned long)phase->row_atlas.pixels,
               (unsigned long)phase->row_atlas.rows, (unsigned long)phase->row_atlas.glyphs,
               (unsigned long)phase->row_atlas.bytes, (unsigned long)phase->row_atlas.peak_bytes,
               (unsigned long)phase->row_atlas.budget);
        printf("@@ORBDEV FUSEDGLASS phase=%s prepares=%llu draws=%llu fallbacks=%llu "
               "maps=%lu bytes=%u peak=%u budget=%u\n", phase->phase,
               (unsigned long long)phase->fused.prepares, (unsigned long long)phase->fused.draws,
               (unsigned long long)phase->fused.fallbacks, (unsigned long)phase->fused.maps,
               (unsigned)phase->fused.bytes, (unsigned)phase->fused.peak_bytes,
               (unsigned)phase->fused.budget_bytes);
    }
    heap_caps_free(s_bench_profile);
    s_bench_profile = NULL;
}
#endif

bool orb_devtools_benchmark_active(void) { return s_bench_active; }

static void scroll_observe(orb_scroll_event_t event, int32_t x, int32_t y, void *ctx)
{
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
    orb_display_scroll_trace_record(event == ORB_SCROLL_INPUT ? ORB_SCROLL_TRACE_INPUT_INJECT :
        event == ORB_SCROLL_RELEASE ? ORB_SCROLL_TRACE_INJECT_RELEASE : ORB_SCROLL_TRACE_SCROLL,
        esp_timer_get_time(), x, y);
#endif
    (void)ctx;
    (void)event; (void)x; (void)y;
}

#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
/* Fixed wire records omit compiler padding. Chunks may split a record, just
 * as screenshot chunks may split a pixel row; the host reassembles bytes. */
static bool scroll_trace_chunk(const orb_display_scroll_trace_t *trace, uint32_t offset,
                               uint8_t raw[ORB_SCROLL_TRACE_CHUNK_BYTES], uint32_t *size)
{
    uint32_t remaining = trace->count * ORB_SCROLL_TRACE_RECORD_BYTES - offset;
    *size = remaining < ORB_SCROLL_TRACE_CHUNK_BYTES ? remaining : ORB_SCROLL_TRACE_CHUNK_BYTES;
    for (uint32_t used = 0; used < *size;) {
        uint32_t index = (offset + used) / ORB_SCROLL_TRACE_RECORD_BYTES;
        uint32_t skip = (offset + used) % ORB_SCROLL_TRACE_RECORD_BYTES;
        uint32_t take = ORB_SCROLL_TRACE_RECORD_BYTES - skip;
        if (take > *size - used) take = *size - used;
        orb_display_scroll_trace_event_t event;
        uint8_t record[ORB_SCROLL_TRACE_RECORD_BYTES];
        if (!orb_display_scroll_trace_read(index, &event) ||
            !orb_scroll_trace_pack(&event, trace->started_us, record)) return false;
        memcpy(raw + used, record + skip, take);
        used += take;
    }
    return true;
}
#endif

#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
/* The console task has a 4 KiB stack, most already occupied by command parsing.
 * Keep the immutable snapshot and wire scratch in PSRAM, outside measurement. */
typedef struct {
    orb_display_scroll_trace_t trace;
    char text[1536];
    uint8_t raw[ORB_SCROLL_TRACE_CHUNK_BYTES];
} scroll_trace_report_t;

static bool scroll_trace_emit(scroll_trace_report_t *report, int64_t emit_started)
{
    const int64_t deadline_us = emit_started + ORB_SCROLL_TRACE_EMIT_TIMEOUT_US;
    orb_display_scroll_trace_t *trace = &report->trace;
    orb_display_scroll_trace_snapshot(trace);
    if (trace->active || trace->count > ORB_SCROLL_TRACE_CAPACITY) return false;
    uint32_t bytes = trace->count * ORB_SCROLL_TRACE_RECORD_BYTES;
    uint32_t chunks = (bytes + ORB_SCROLL_TRACE_CHUNK_BYTES - 1) / ORB_SCROLL_TRACE_CHUNK_BYTES;
    uint32_t crc = 0;
    uint8_t *raw = report->raw;
    uint32_t size;
    /* A bounded pass computes the whole-stream CRC before writing its header.
     * Capture is stopped, so the second pass reads exactly the same prefix. */
    for (uint32_t offset = 0; offset < bytes; offset += size) {
        if (esp_timer_get_time() >= deadline_us || !scroll_trace_chunk(trace, offset, raw, &size)) return false;
        crc = esp_rom_crc32_le(crc, raw, size);
    }
    if (!command_reply_printf_until(deadline_us,
        "@@ORBDEV SCROLLTRACE version=2 encoding=le13 record_bytes=%u active=%u captured_active=%u "
        "count=%lu dropped=%lu bytes=%lu chunks=%lu crc32=%08lx started_us=%lld ended_us=%lld "
        "completions=%lu loops=%lu handler_us=%llu timer_wait_us=%llu fairness_wait_us=%llu\n",
        ORB_SCROLL_TRACE_RECORD_BYTES, trace->available, trace->active,
        (unsigned long)trace->count, (unsigned long)trace->dropped, (unsigned long)bytes,
        (unsigned long)chunks, (unsigned long)crc, (long long)trace->started_us, (long long)trace->ended_us,
        (unsigned long)trace->completions, (unsigned long)trace->loops,
        (unsigned long long)trace->handler_us, (unsigned long long)trace->timer_wait_us,
        (unsigned long long)trace->fairness_wait_us)) return false;
    char *histogram = report->text;
    size_t used = 0;
    for (unsigned i = 0; i < ORB_SCROLL_TRACE_INTERVAL_BUCKETS; i++)
        used += snprintf(histogram + used, sizeof(report->text) - used, "%s%lu", i ? "," : "",
                         (unsigned long)trace->intervals[i]);
    if (!command_reply_printf_until(deadline_us, "@@ORBDEV SCROLLTRACE_HIST bucket_us=1000 counts=%s\n", histogram)) return false;
    /* Histogram is already on the wire; reuse its PSRAM scratch for base64. */
    char *encoded = histogram;
    for (uint32_t offset = 0, seq = 0; offset < bytes; offset += size, seq++) {
        if (!scroll_trace_chunk(trace, offset, raw, &size)) return false;
        base64(raw, size, encoded);
        if (!command_reply_printf_until(deadline_us,
            "@@ORBDEV SCROLLTRACE_CHUNK seq=%lu bytes=%lu crc32=%08lx data=%s\n",
            (unsigned long)seq, (unsigned long)size,
            (unsigned long)esp_rom_crc32_le(0, raw, size), encoded)) return false;
    }
    return command_reply_printf_until(deadline_us,
        "@@ORBDEV SCROLLTRACE_END chunks=%lu bytes=%lu crc32=%08lx emit_us=%lld\n",
        (unsigned long)chunks, (unsigned long)bytes, (unsigned long)crc,
        (long long)(esp_timer_get_time() - emit_started));
}
#endif

static bool scroll_trace_report(void)
{
#if CONFIG_ORB_DISPLAY_SCROLL_TRACE
    const int64_t emit_started = esp_timer_get_time();
    scroll_trace_report_t *report = heap_caps_malloc(sizeof(*report), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!report) return false;
    bool sent = scroll_trace_emit(report, emit_started);
    heap_caps_free(report);
    return sent;
#else
    return true;
#endif
}

/* End scroll trace output. */
static bool benchmark_distribution_report(const char *sample, const char *name,
    const orb_display_counters_t *before, const orb_display_counters_t *after)
{
    return orb_devtools_distribution_report(sample, name, before, after, command_reply_printf);
}

#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
static void benchmark_task_report(const char *name, const lv_draw_sw_profile_snapshot_t *before,
                                   const lv_draw_sw_profile_snapshot_t *after)
{
    /* Worker wall spans include preemption and overlap each other. This is
     * source attribution, never a sum of serial frame latency. */
    for (unsigned i = 0; i < LV_DRAW_SW_PROFILE_SLOTS; i++) {
        uint32_t calls = after->rows[i].calls - before->rows[i].calls;
        if (!calls) continue;
        command_reply_printf("@@ORBDEV DRAWTASK phase=%s type=%s%s class=%s calls=%lu elapsed_us=%lu\n",
            name, lv_draw_sw_profile_type(i), lv_draw_sw_profile_transformed(i) ? "_TRANSFORM" : "",
            lv_draw_sw_profile_class(i), (unsigned long)calls,
            (unsigned long)(uint32_t)(after->rows[i].elapsed_us - before->rows[i].elapsed_us));
    }
    for (unsigned i = 0; i < LV_DRAW_SW_PROFILE_STAGE_COUNT; i++) {
        command_reply_printf("@@ORBDEV HOTSPOT phase=%s stage=%s calls=%lu elapsed_us=%lu pixels=%lu\n",
            name, lv_draw_sw_profile_stage_name(i),
            (unsigned long)(uint32_t)(after->stages[i].calls - before->stages[i].calls),
            (unsigned long)(uint32_t)(after->stages[i].elapsed_us - before->stages[i].elapsed_us),
            (unsigned long)(uint32_t)(after->stages[i].pixels - before->stages[i].pixels));
    }
}
#endif

static void scroll_benchmark_details(scroll_bench_t *bench,
    const orb_display_cadence_t *cadence, const orb_ui_scroll_diagnostics_t *ui, uint64_t frames)
{
    uint32_t viewport_pixels = orb_scroll_fixture_viewport_pixels(bench->fixture);
    bool fast_list = bench->surface == ORB_SCROLL_AGENTS || bench->surface == ORB_SCROLL_SETTINGS ||
                     bench->surface == ORB_SCROLL_NOTIFICATIONS;
    orb_scroll_gates_t gates = orb_scroll_gates(fast_list, frames, bench->moving_frames, bench->moving_us,
                                               viewport_pixels, cadence);
    /* Keep this separate from SCROLL's bounded reply line. Damage is the exact
     * union retained for each refresh; request sums intentionally count overlap.
     * These are observations/assertions, not a crash on a slow diagnostic run. */
    command_reply_printf("@@ORBDEV SCROLL_GATE version=1 surface=%s target_fps=%u fps_pass=%u "
           "stall_limit_us=%u handler_stalls=%llu animation_stalls=%llu stall_pass=%u "
           "viewport_pixels=%lu invalidation_limit_permille=%u damage_frames=%llu damage_pixels=%llu "
           "max_damage_pixels=%llu damage_ratio=%.6f request_invalidation_pass=%u damage_pass=%u invalidation_pass=%u "
           "request_pixels_per_frame=%.3f source_builds=%lu mip_builds=%lu row_builds=%lu row_evictions=%lu "
           "glyph_size_builds=%lu glyph_size_reuses=%lu\n",
           orb_scroll_surface_name(bench->surface), gates.target_fps, gates.fps_pass,
           ORB_SCROLL_STALL_LIMIT_US, (unsigned long long)cadence->handler_stalls,
           (unsigned long long)cadence->animation_stalls, gates.stall_pass,
           (unsigned long)viewport_pixels, ORB_SCROLL_DAMAGE_LIMIT_PERMILLE,
           (unsigned long long)cadence->damage_frames, (unsigned long long)cadence->damage_pixels,
           (unsigned long long)cadence->damage_max_pixels,
           viewport_pixels ? (double)cadence->damage_max_pixels / viewport_pixels : 0,
           gates.request_invalidation_pass, gates.damage_pass, gates.invalidation_pass,
           frames ? (double)cadence->invalidated_pixels / frames : 0,
           (unsigned long)(bench->raster_now.builds - bench->raster_initial.builds),
           (unsigned long)(bench->raster_now.mip_builds - bench->raster_initial.mip_builds),
           (unsigned long)(bench->rows_now.builds - bench->rows_initial.builds),
           (unsigned long)(bench->rows_now.evictions - bench->rows_initial.evictions),
           (unsigned long)(bench->rows_now.size_builds - bench->rows_initial.size_builds),
           (unsigned long)(bench->rows_now.size_reuses - bench->rows_initial.size_reuses));
    orb_devtools_round_receipt(orb_scroll_surface_name(bench->surface), ui, cadence, command_reply_printf);
    for (unsigned i = 0; i < ORB_DISPLAY_SLOW_HANDLERS; i++) {
        const orb_display_slow_handler_t *slow = &cadence->slow[i];
        if (!slow->handler_us) continue;
        unsigned dominant = 0;
        for (unsigned j = 1; j < ORB_DISPLAY_PHASES; j++)
            if (slow->phase_us[j] > slow->phase_us[dominant]) dominant = j;
        command_reply_printf("@@ORBDEV SCROLL_SLOW surface=%s rank=%u sequence=%llu at_us=%llu moving=%u "
            "handler_us=%llu dominant=%s timer_callback=0x%lx timer_us=%llu "
            "other_us=%llu prepare_us=%llu layout_us=%llu damage_us=%llu raster_us=%llu sync_us=%llu draw_us=%llu finish_us=%llu\n",
            orb_scroll_surface_name(bench->surface), i + 1, (unsigned long long)slow->sequence,
            (unsigned long long)slow->at_us, slow->moving, (unsigned long long)slow->handler_us,
            orb_display_phase_name(dominant), (unsigned long)slow->timer_callback, (unsigned long long)slow->timer_us,
            (unsigned long long)slow->phase_us[0], (unsigned long long)slow->phase_us[1],
            (unsigned long long)slow->phase_us[2], (unsigned long long)slow->phase_us[3],
            (unsigned long long)slow->phase_us[4], (unsigned long long)slow->phase_us[5],
            (unsigned long long)slow->phase_us[6], (unsigned long long)slow->phase_us[7]);
    }
    const char *memory_names[3] = { "internal", "dma", "psram" };
    for (unsigned i = 0; i < 3; i++) {
        command_reply_printf("@@ORBDEV SCROLL_MEMORY surface=%s kind=%s window=post_prepare_scroll "
            "minimum_free=%u free=%u largest=%u\n", orb_scroll_surface_name(bench->surface), memory_names[i],
            (unsigned)bench->memory[i].minimum_free_bytes, (unsigned)bench->memory[i].total_free_bytes,
            (unsigned)bench->memory[i].largest_free_block);
    }
}

static void scroll_benchmark_report(const orb_display_counters_t *now)
{
    scroll_bench_t *bench = s_scroll_bench;
    if (!bench || !bench->ready) return;
    if (!benchmark_distribution_report("scroll", orb_scroll_surface_name(bench->surface), &bench->initial, now))
        s_bench_failed = true;
    orb_display_cadence_end();
    const uint32_t memory_caps[3] = { MALLOC_CAP_INTERNAL, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, MALLOC_CAP_SPIRAM };
    if (bench->memory_active) {
        for (unsigned i = 0; i < 3; i++) orb_mem_phase_info(ORB_MEM_SCROLL, &bench->memory[i], memory_caps[i]);
        orb_mem_end(ORB_MEM_SCROLL);
        bench->memory_active = false;
    }
    orb_ui_scroll_diagnostics_end();
    orb_display_cadence_snapshot(&bench->cadence);
    orb_ui_scroll_diagnostics_snapshot(&bench->ui_diagnostics);
    orb_scroll_fixture_diagnostics(bench->fixture, &bench->input_diagnostics);
    const orb_display_cadence_t *cadence = &bench->cadence;
    const orb_ui_scroll_diagnostics_t *ui = &bench->ui_diagnostics;
    const orb_scroll_fixture_diagnostics_t *input = &bench->input_diagnostics;
    lv_obj_raster_get_stats(&bench->raster_now);
    lv_obj_raster_rows_get_stats(&bench->rows_now);
    multi_heap_info_t historical_internal;
    orb_mem_heap_info(&historical_internal, MALLOC_CAP_INTERNAL);
    float busy[2] = {0};
    bool cpu_valid = orb_display_cpu_busy(&bench->cpu, &bench->cpu_after, busy);
    if (!cpu_valid) busy[0] = busy[1] = 0;
    bool moving_cpu_valid = bench->moving_cpu_valid && bench->moving_cpu_us > 0;
    orb_display_accounting(&bench->account_now);
    uint64_t frames = now->frames - bench->initial.frames;
    if (!frames || !bench->moving_frames || !input->offset_changes) {
        s_bench_failed = true;
        command_reply_printf("@@ORBDEV ERROR bench_scroll surface=%s reason=no_moving_frames input_reports=%lu offset_changes=%lu\n",
            orb_scroll_surface_name(bench->surface), (unsigned long)input->reports,
            (unsigned long)input->offset_changes);
    }
    double count = frames ? frames : 1;
    double seconds = (now->at_us - bench->initial.at_us) / 1e6;
    uint64_t bytes = now->bytes - bench->initial.bytes;
    uint64_t blur_us = 0, glass_prepare_us = 0, glass_compose_us = 0;
    bool glass_timing = false;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    glass_timing = true;
    if (s_bench_profile && s_bench_profile->count) {
        const lv_draw_sw_profile_snapshot_t *tasks = &s_bench_profile->phases[s_bench_profile->count - 1].tasks;
        for (unsigned i = 0; i < LV_DRAW_SW_PROFILE_SLOTS; i++)
            if (!strcmp(lv_draw_sw_profile_type(i), "BLUR")) blur_us += tasks->rows[i].elapsed_us;
        glass_prepare_us = tasks->stages[LV_DRAW_SW_PROFILE_GLASS_PREPARE].elapsed_us;
        glass_compose_us = tasks->stages[LV_DRAW_SW_PROFILE_GLASS_COMPOSE].elapsed_us;
    }
#endif
    /* Movement includes real pointer tracking and native momentum/Home spring.
     * Dirty area is pixels actually sent, including round-clipped bands. */
    command_reply_printf("@@ORBDEV SCROLL surface=%s workload=%s seconds=%.3f frames=%llu fps=%.2f overall_fps=%.2f "
           "moving_us=%llu moving_frames=%llu idle_frames=%llu gestures=%u "
           "draw_ms=%.3f wire_ms=%.3f whole_update_ms=%.3f accounting=%d "
           "cpu_available=%d cpu0_pct=%.2f cpu1_pct=%.2f "
           "moving_cpu_available=%d moving_cpu0_pct=%.2f moving_cpu1_pct=%.2f "
           "glass_timing=%d blur_task_ms=%.3f glass_prepare_ms=%.3f glass_compose_ms=%.3f "
           "dirty_pixels=%llu dirty_pct=%.3f minimum_internal=%u errors=%llu failed_frames=%llu "
           "ui_cpu_available=%d ui_cpu_pct=%.2f ui_priority=%lu ui_core=%lu "
           "loops=%llu lock_us=%llu handler_us=%llu handler_max_us=%llu handler_outside_refresh_us=%llu refresh_us=%llu "
           "timer_wait_us=%llu fairness_wait_us=%llu wait_requested_us=%llu wait_overrun_us=%llu wait_max_us=%llu "
           "invalidations=%llu invalidated_pixels=%llu refreshes=%llu render_starts=%llu "
           "anim_ticks=%llu anim_gap_max_us=%llu anim_late_max_ms=%llu anim_period_ms=%lu indev_period_ms=%lu "
           "freq_samples=%llu freq_below_max=%llu freq_min_mhz=%lu freq_max_mhz=%lu "
           "moving_freq_samples=%llu moving_freq_below_max=%llu moving_freq_min_mhz=%lu moving_freq_max_mhz=%lu "
           "round_calls=%llu round_us=%llu round_max_us=%llu round_rows=%llu "
           "readable_calls=%llu readable_us=%llu readable_max_us=%llu readable_rows=%llu readable_layout_calls=%llu "
           "style_writes=%llu redundant_style_writes=%llu font_changes=%llu "
           "input_reports=%lu input_releases=%lu input_skipped=%lu input_gap_max_ms=%lu "
           "scroll_events=%lu offset_changes=%lu offset_gap_max_ms=%lu\n",
           orb_scroll_surface_name(bench->surface), "touch_fling_settle",
           seconds, (unsigned long long)frames,
           bench->moving_us ? 1e6 * bench->moving_frames / bench->moving_us : 0,
           seconds > 0 ? frames / seconds : 0,
           (unsigned long long)bench->moving_us, (unsigned long long)bench->moving_frames,
           (unsigned long long)bench->idle_frames, orb_scroll_fixture_gestures(bench->fixture),
           (now->draw_us - bench->initial.draw_us) / count / 1000,
           (now->send_us - bench->initial.send_us) / count / 1000,
           (bench->account_now.whole_update_us - bench->account.whole_update_us) / count / 1000,
           orb_display_accounting_active(), cpu_valid, (double)busy[0], (double)busy[1],
           moving_cpu_valid,
           moving_cpu_valid ? bench->moving_cpu_work[0] / bench->moving_cpu_us : 0,
           moving_cpu_valid ? bench->moving_cpu_work[1] / bench->moving_cpu_us : 0,
           glass_timing, blur_us / count / 1000, glass_prepare_us / count / 1000, glass_compose_us / count / 1000,
           (unsigned long long)(bytes / 2), bytes * 50.0 / count / (466.0 * 466.0),
           (unsigned)historical_internal.minimum_free_bytes,
           (unsigned long long)(now->transfer_errors - bench->initial.transfer_errors),
           (unsigned long long)(now->failed_frames - bench->initial.failed_frames),
           cadence->ui_cpu_available, (double)cadence->ui_cpu_pct,
           (unsigned long)cadence->ui_priority, (unsigned long)cadence->ui_core,
           (unsigned long long)cadence->loops, (unsigned long long)cadence->lock_us,
           (unsigned long long)cadence->handler_us, (unsigned long long)cadence->handler_max_us,
           (unsigned long long)cadence->handler_outside_refresh_us,
           (unsigned long long)cadence->refresh_us, (unsigned long long)cadence->timer_wait_us,
           (unsigned long long)cadence->fairness_wait_us, (unsigned long long)cadence->wait_requested_us,
           (unsigned long long)cadence->wait_overrun_us, (unsigned long long)cadence->wait_max_us,
           (unsigned long long)cadence->invalidations, (unsigned long long)cadence->invalidated_pixels,
           (unsigned long long)cadence->refreshes, (unsigned long long)cadence->render_starts,
           (unsigned long long)cadence->animation_ticks, (unsigned long long)cadence->animation_gap_max_us,
           (unsigned long long)cadence->animation_late_max_ms, (unsigned long)cadence->animation_period_ms,
           (unsigned long)cadence->indev_period_ms, (unsigned long long)cadence->frequency_samples,
           (unsigned long long)cadence->frequency_below_max, (unsigned long)cadence->frequency_min_mhz,
           (unsigned long)cadence->frequency_max_mhz,
           (unsigned long long)cadence->moving_frequency_samples,
           (unsigned long long)cadence->moving_frequency_below_max,
           (unsigned long)cadence->moving_frequency_min_mhz, (unsigned long)cadence->moving_frequency_max_mhz,
           (unsigned long long)ui->round_calls, (unsigned long long)ui->round_us,
           (unsigned long long)ui->round_max_us, (unsigned long long)ui->round_rows,
           (unsigned long long)ui->readable_calls, (unsigned long long)ui->readable_us,
           (unsigned long long)ui->readable_max_us, (unsigned long long)ui->readable_rows,
           (unsigned long long)ui->readable_layout_calls, (unsigned long long)ui->style_writes,
           (unsigned long long)ui->redundant_style_writes, (unsigned long long)ui->font_changes,
           (unsigned long)input->reports, (unsigned long)input->releases,
           (unsigned long)input->skipped_reports, (unsigned long)input->report_gap_max_ms,
           (unsigned long)input->scroll_events, (unsigned long)input->offset_changes,
           (unsigned long)input->offset_gap_max_ms);
    scroll_benchmark_details(bench, cadence, ui, frames);
}

static void benchmark_report(void)
{
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    benchmark_account_sample();
#endif
    orb_display_counters_t now;
    orb_display_counters(&now);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    benchmark_profile_sample(&now);
#endif
    uint64_t frames = now.frames - s_bench_snapshots->display.frames;
    double count = frames ? frames : 1;
    double seconds = (now.at_us - s_bench_snapshots->display.at_us) / 1e6;
    printf("\n@@ORBDEV BENCH %s seconds=%.2f frames=%llu fps=%.2f frame_ms=%.2f draw_ms=%.2f wait_ms=%.2f hold_ms=%.2f wire_ms=%.2f torn=%llu free_internal=%u largest_internal=%u bytes=%llu bands=%llu\n",
           s_bench_phase, seconds, (unsigned long long)frames, seconds > 0 ? frames / seconds : 0,
           (now.frame_us - s_bench_snapshots->display.frame_us) / count / 1000,
           (now.draw_us - s_bench_snapshots->display.draw_us) / count / 1000,
           (now.wait_us - s_bench_snapshots->display.wait_us) / count / 1000,
           (now.hold_us - s_bench_snapshots->display.hold_us) / count / 1000,
           (now.send_us - s_bench_snapshots->display.send_us) / count / 1000,
           (unsigned long long)(now.torn - s_bench_snapshots->display.torn),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned long long)(now.bytes - s_bench_snapshots->display.bytes),
           (unsigned long long)(now.bands - s_bench_snapshots->display.bands));
    uint64_t target = (frames * 95 + 99) / 100, cumulative = 0;
    unsigned p95 = 0;
    for (; p95 < ORB_DISPLAY_LATENCY_BUCKETS - 1; p95++) {
        cumulative += (uint32_t)(now.frame_latency[p95] - s_bench_snapshots->display.frame_latency[p95]);
        if (cumulative >= target) break;
    }
    printf("@@ORBDEV PIPELINE active=%d copy_ms=%.2f queue_ms=%.2f bands=%llu dropped=%llu errors=%llu failed_frames=%llu p95_upper_ms=%u p95_overflow=%d\n",
           orb_display_pipeline_active(), (now.copy_us - s_bench_snapshots->display.copy_us) / count / 1000,
           (now.queue_wait_us - s_bench_snapshots->display.queue_wait_us) / count / 1000,
           (unsigned long long)(now.pipeline_bands - s_bench_snapshots->display.pipeline_bands),
           (unsigned long long)(now.pipeline_dropped - s_bench_snapshots->display.pipeline_dropped),
           (unsigned long long)(now.transfer_errors - s_bench_snapshots->display.transfer_errors),
           (unsigned long long)(now.failed_frames - s_bench_snapshots->display.failed_frames),
           frames ? (p95 + 1) * ORB_DISPLAY_LATENCY_BUCKET_US / 1000 : 0,
           frames && p95 == ORB_DISPLAY_LATENCY_BUCKETS - 1);
    lv_draw_sw_blur_cache_stats_t cache;
    lv_draw_sw_blur_cache_get_stats(&cache);
    printf("@@ORBDEV CACHE hits=%llu misses=%llu evictions=%llu busy=%llu oversize=%llu alloc_fail=%llu bytes=%u peak=%u budget=%u\n",
           (unsigned long long)(cache.hits - s_bench_snapshots->cache.hits),
           (unsigned long long)(cache.misses - s_bench_snapshots->cache.misses),
           (unsigned long long)(cache.evictions - s_bench_snapshots->cache.evictions),
           (unsigned long long)(cache.busy_bypasses - s_bench_snapshots->cache.busy_bypasses),
           (unsigned long long)(cache.oversize_bypasses - s_bench_snapshots->cache.oversize_bypasses),
           (unsigned long long)(cache.allocation_failures - s_bench_snapshots->cache.allocation_failures),
           (unsigned)cache.bytes, (unsigned)cache.peak_bytes, (unsigned)cache.budget_bytes);
    s_bench_snapshots->cache = cache;
    lv_obj_raster_scale_cache_stats_t scale;
    lv_obj_raster_scale_cache_get_stats(&scale);
    printf("@@ORBDEV SCALECACHE phase=%s hits=%lu misses=%lu builds=%lu evictions=%lu alloc_fail=%lu blits=%lu build_us=%lu bytes=%lu peak=%lu budget=%lu\n",
           s_bench_phase, (unsigned long)(scale.hits - s_bench_snapshots->scale.hits),
           (unsigned long)(scale.misses - s_bench_snapshots->scale.misses),
           (unsigned long)(scale.builds - s_bench_snapshots->scale.builds),
           (unsigned long)(scale.evictions - s_bench_snapshots->scale.evictions),
           (unsigned long)(scale.alloc_fail - s_bench_snapshots->scale.alloc_fail),
           (unsigned long)(scale.blits - s_bench_snapshots->scale.blits),
           (unsigned long)(scale.build_us - s_bench_snapshots->scale.build_us),
           (unsigned long)scale.bytes, (unsigned long)scale.peak_bytes,
           (unsigned long)scale.budget);
    s_bench_snapshots->scale = scale;
    if (s_bench_fixture == BENCH_SCROLL) scroll_benchmark_report(&now);
    s_bench_snapshots->display = now;
}

static void benchmark_finish(void)
{
    if (s_bench_timer) lv_timer_delete(s_bench_timer);
    s_bench_timer = NULL;
    if (s_bench_fixture == BENCH_SCROLL) {
        orb_display_scroll_trace_end();
        orb_display_cadence_end();
        orb_ui_scroll_diagnostics_end();
    }
    if (s_scroll_bench && s_scroll_bench->fixture) {
        if (s_scroll_bench->memory_active) {
            orb_mem_end(ORB_MEM_SCROLL);
            s_scroll_bench->memory_active = false;
        }
        orb_scroll_fixture_destroy(s_scroll_bench->fixture);
        s_scroll_bench->fixture = NULL;
    }
    if (s_saved_screen) lv_screen_load(s_saved_screen);
    if (s_bench_screen) lv_obj_delete(s_bench_screen);
    s_bench_screen = NULL;
    orb_ui_set_reduce_motion(s_saved_reduce_motion);
    s_bench_active = false;
    xSemaphoreGive(s_bench_done);
}

static void benchmark_sample_begin(void)
{
    /* First-use debug arena allocation belongs to setup, not measured motion. */
    if (s_bench_fixture == BENCH_SCROLL) orb_display_scroll_trace_begin();
    s_bench_started = s_bench_tick = esp_timer_get_time();
    s_bench_phase = s_bench_fixture == BENCH_ORIGINAL ? orb_demo_phase() :
                    s_bench_fixture == BENCH_QUIET ? "quiet" : s_bench_fixture == BENCH_SCROLL ?
                    orb_scroll_surface_name(s_scroll_bench->surface) : "transition";
    orb_display_counters(&s_bench_snapshots->display);
    lv_draw_sw_blur_cache_get_stats(&s_bench_snapshots->cache);
    lv_obj_raster_scale_cache_get_stats(&s_bench_snapshots->scale);
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    orb_display_accounting(&s_bench_account->previous);
    lv_obj_raster_get_stats(&s_bench_account->raster);
    lv_draw_sw_material_get_stats(&s_bench_account->material);
#endif
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot(&s_bench_profile->previous);
    lv_obj_raster_get_stats(&s_bench_profile->raster);
    lv_obj_raster_rows_get_stats(&s_bench_profile->row_atlas);
    orb_glass_fused_get_stats(&s_bench_profile->fused);
#endif
    if (s_bench_fixture == BENCH_SCROLL) {
        scroll_bench_t *bench = s_scroll_bench;
        bench->initial = bench->previous = s_bench_snapshots->display;
        orb_display_accounting(&bench->account);
        orb_display_cpu_snapshot(&bench->cpu);
        bench->moving_cpu_start = bench->cpu;
        bench->moving_cpu_valid = true;
        bench->previous_us = s_bench_started;
        bench->ready = true;
        lv_obj_raster_get_stats(&bench->raster_initial);
        lv_obj_raster_rows_get_stats(&bench->rows_initial);
        orb_display_cadence_begin();
        orb_mem_begin(ORB_MEM_SCROLL);
        bench->memory_active = true;
        orb_ui_scroll_diagnostics_begin();
        orb_scroll_fixture_tick(bench->fixture, 0);
        bench->previous_moving = orb_scroll_fixture_moving(bench->fixture);
        orb_display_cadence_set_moving(bench->previous_moving);
    }
}

static void scroll_benchmark_cpu_end(scroll_bench_t *bench, const orb_display_cpu_snapshot_t *now)
{
    float busy[2] = {0};
    if (!orb_display_cpu_busy(&bench->moving_cpu_start, now, busy)) {
        bench->moving_cpu_valid = false; return;
    }
    uint64_t elapsed = now->at_us - bench->moving_cpu_start.at_us;
    bench->moving_cpu_us += elapsed;
    for (unsigned core = 0; core < 2; core++) bench->moving_cpu_work[core] += busy[core] * elapsed;
}

static void scroll_benchmark_sample(int64_t now)
{
    scroll_bench_t *bench = s_scroll_bench;
    orb_display_counters_t current;
    orb_display_counters(&current);
    uint64_t frames = current.frames - bench->previous.frames;
    if (bench->previous_moving) {
        bench->moving_us += now - bench->previous_us;
        bench->moving_frames += frames;
    } else if (bench->settled_since && now - bench->settled_since >= 750000) {
        /* The production scroll mark settles over 750ms after the last row
         * position changes. Count idle only after its complete fade. */
        bench->idle_frames += frames;
    }
    bench->previous = current;
    bench->previous_us = now;
}

static void benchmark_tick(lv_timer_t *timer)
{
    if (s_wifi.active && !wifi_current()) {
        s_wifi.failed = true;
        s_bench_failed = true;
        s_bench_timer = NULL; lv_timer_delete(timer);
        benchmark_finish();
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_bench_fixture == BENCH_SCROLL) {
        scroll_bench_t *bench = s_scroll_bench;
        if (!bench->ready) {
            if (now - bench->warm_started < 800000) return;
            if (!orb_scroll_fixture_prepare(bench->fixture)) {
                if (now - bench->warm_started < 5000000) return;
                s_bench_failed = true;
                s_bench_timer = NULL; lv_timer_delete(timer); benchmark_finish(); return;
            }
            lv_refr_now(NULL);
            if (!orb_display_drain()) {
                s_bench_failed = true;
                s_bench_timer = NULL; lv_timer_delete(timer); benchmark_finish(); return;
            }
            benchmark_sample_begin(); return;
        }
        scroll_benchmark_sample(now);
    }
    if (now - s_bench_started >= s_bench_ms * 1000LL) {
        if (s_bench_fixture == BENCH_SCROLL) {
            if (!orb_display_drain()) s_bench_failed = true;
            scroll_benchmark_sample(esp_timer_get_time());
            orb_display_cpu_snapshot(&s_scroll_bench->cpu_after);
            if (s_scroll_bench->previous_moving) scroll_benchmark_cpu_end(s_scroll_bench, &s_scroll_bench->cpu_after);
            orb_display_scroll_trace_end();
        }
        benchmark_report();
        s_bench_timer = NULL; lv_timer_delete(timer);
        benchmark_finish();
        return;
    }
    /* End belongs to the last sampled scene; do not append an empty next
     * phase if the scene and duration boundaries happen on the same tick. */
    const char *phase = s_bench_fixture == BENCH_ORIGINAL ? orb_demo_phase() : s_bench_phase;
    if (strcmp(phase, s_bench_phase)) {
        benchmark_report();
        s_bench_phase = phase;
    }
    if (s_bench_fixture == BENCH_ORIGINAL) orb_demo_tick(0, 0, 1, (now - s_bench_tick) / 1e6f);
    if (s_bench_fixture == BENCH_SCROLL) {
        orb_scroll_fixture_tick(s_scroll_bench->fixture, (now - s_bench_started) / 1000);
        bool moving = orb_scroll_fixture_moving(s_scroll_bench->fixture);
        if (moving != s_scroll_bench->previous_moving) {
            orb_display_cpu_snapshot_t cpu;
            orb_display_cpu_snapshot(&cpu);
            if (moving) s_scroll_bench->moving_cpu_start = cpu;
            else scroll_benchmark_cpu_end(s_scroll_bench, &cpu);
            s_scroll_bench->settled_since = moving ? 0 : now;
        }
        s_scroll_bench->previous_moving = moving;
        orb_display_cadence_set_moving(moving);
    }
    s_bench_tick = now;
}

static void benchmark_start(void *ctx)
{
    if (s_wifi.active && !wifi_current()) {
        s_wifi.failed = true; s_bench_failed = true; s_bench_active = false;
        xSemaphoreGive(s_bench_done);
        return;
    }
    s_bench_active = true;
    s_saved_screen = lv_screen_active();
    s_saved_reduce_motion = orb_ui_reduce_motion();
    orb_ui_set_reduce_motion(false);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    /* A previous requester can time out before its scene ends. Its immutable
     * samples may be reclaimed now, once the active-scene guard permits us. */
    heap_caps_free(s_bench_profile);
    s_bench_profile = heap_caps_calloc(1, sizeof(*s_bench_profile), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bench_profile) {
        s_bench_failed = true;
        orb_ui_set_reduce_motion(s_saved_reduce_motion);
        s_bench_active = false;
        xSemaphoreGive(s_bench_done);
        return;
    }
#endif
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    heap_caps_free(s_bench_account);
    s_bench_account = heap_caps_calloc(1, sizeof(*s_bench_account), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bench_account) {
        s_bench_failed = true;
        orb_ui_set_reduce_motion(s_saved_reduce_motion);
        s_bench_active = false;
        xSemaphoreGive(s_bench_done);
        return;
    }
#endif
    s_bench_screen = lv_obj_create(NULL);
    if (!s_bench_screen) {
        s_bench_failed = true;
        orb_ui_set_reduce_motion(s_saved_reduce_motion);
        s_bench_active = false;
        xSemaphoreGive(s_bench_done);
        return;
    }
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    lv_obj_raster_stats_t cold_before;
    lv_obj_raster_get_stats(&cold_before);
#endif
    int64_t cold_started = esp_timer_get_time();
    if (s_bench_fixture == BENCH_SCROLL) {
        s_scroll_bench->fixture = orb_devtools_scroll_fixture_create(s_bench_screen, s_scroll_bench->surface);
        if (!s_scroll_bench->fixture) { s_bench_failed = true; benchmark_finish(); return; }
        orb_scroll_fixture_observe(s_scroll_bench->fixture, scroll_observe, NULL);
    } else if (s_bench_fixture == BENCH_ORIGINAL) orb_demo_build(s_bench_screen);
    else orb_demo_fixture_build(s_bench_screen, s_bench_fixture == BENCH_QUIET ?
                                ORB_DEMO_FIXTURE_QUIET : ORB_DEMO_FIXTURE_TRANSITION);
    uint64_t cold_us = esp_timer_get_time() - cold_started;
    orb_display_account_cold_prepare(cold_us);
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    s_bench_account->cold_prepare_us = cold_us;
    lv_obj_raster_get_stats(&s_bench_account->raster);
    s_bench_account->cold_source_builds = s_bench_account->raster.builds - cold_before.builds;
    s_bench_account->cold_mip_builds = s_bench_account->raster.mip_builds - cold_before.mip_builds;
#endif
    lv_screen_load(s_bench_screen);
    if (s_bench_fixture != BENCH_ORIGINAL && s_bench_fixture != BENCH_SCROLL) {
        /* Initial construction and transfer are measured as cold preparation;
         * the optional steady quiet receipt begins only after the SPI fence. */
        lv_refr_now(NULL);
        if (!orb_display_drain()) {
            s_bench_failed = true;
            benchmark_finish();
            return;
        }
    }
    if (s_bench_fixture != BENCH_SCROLL) benchmark_sample_begin();
    else s_scroll_bench->warm_started = esp_timer_get_time();
    s_bench_timer = lv_timer_create(benchmark_tick, s_bench_fixture == BENCH_SCROLL ? 1 : 20, NULL);
    if (!s_bench_timer) {
        s_bench_failed = true;
        benchmark_finish();
    }
}

static bool benchmark(int ms, int fixture, orb_scroll_surface_t surface)
{
    if (!s_bench_done) s_bench_done = xSemaphoreCreateBinary();
    if (!s_bench_done) return false;
    if (fixture == BENCH_SCROLL && !command_reply_printf(
        "@@ORBDEV SCROLL_PROGRESS phase=setup surface=%s duration_ms=%d\n",
        orb_scroll_surface_name(surface), ms)) return false;
    if (!orb_devtools_bench_power_ready(s_shell, wake_display)) {
        command_reply_printf("@@ORBDEV ERROR bench reason=power_wake_unsettled\n");
        return false;
    }
    lv_lock();
    if (s_bench_active) {
        lv_unlock();
        return false;
    }
    if (!s_bench_snapshots) {
        s_bench_snapshots = heap_caps_calloc(1, sizeof(*s_bench_snapshots), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_bench_snapshots) { lv_unlock(); return false; }
    }
    heap_caps_free(s_scroll_bench); s_scroll_bench = NULL;
    if (fixture == BENCH_SCROLL) {
        s_scroll_bench = heap_caps_calloc(1, sizeof(*s_scroll_bench), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scroll_bench) { lv_unlock(); return false; }
        s_scroll_bench->surface = surface;
    }
    /* Reserve before queueing. A timed-out requester must not enqueue another
     * scene while its first callback is still waiting for the UI task. */
    s_bench_active = true;
    xSemaphoreTake(s_bench_done, 0);
    s_bench_ms = ms;
    s_bench_fixture = fixture;
    s_bench_failed = false;
    s_saved_screen = NULL;
    s_bench_screen = NULL;
    lv_result_t queued = lv_async_call(benchmark_start, NULL);
    if (queued == LV_RESULT_OK) orb_display_wake_service();
    if (queued != LV_RESULT_OK) s_bench_active = false;
    lv_unlock();
    bool complete = queued == LV_RESULT_OK &&
                    xSemaphoreTake(s_bench_done, pdMS_TO_TICKS(ms + 10000)) == pdTRUE;
    if (!complete && s_wifi.active) {
        lv_lock();
        lv_async_call_cancel(benchmark_start, NULL);
        s_bench_failed = true;
        if (s_bench_active && s_saved_screen) benchmark_finish();
        else s_bench_active = false;
        xSemaphoreTake(s_bench_done, 0);
        lv_unlock();
    }
    bool reported = true;
    if (complete && fixture == BENCH_SCROLL) {
        reported = command_reply_printf("@@ORBDEV SCROLL_PROGRESS phase=emitting surface=%s\n",
                                        orb_scroll_surface_name(surface)) && scroll_trace_report();
    }
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    if (complete && s_bench_profile) benchmark_profile_report();
#endif
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
    if (complete && s_bench_account) benchmark_account_report();
#endif
    return complete && reported && !s_bench_failed;
}

/* Opening uses the live shell and its normal navigation path. Keep deferred
 * receipts in PSRAM so measuring latency cannot lower the internal-RAM floor. */
typedef struct {
    const char *name;
    orb_shell_surface_t target, source, saved;
    bool back, measured, settled, restoring;
    const char *failure;
    orb_open_source_t setup;
    int64_t started_us, setup_us;
    uint32_t token, sequence;
    uint32_t duration_us, panel_period_us;
    lv_timer_t *timer;
    orb_shell_open_metrics_t metrics;
    orb_display_response_t response;
    orb_display_counters_t before, after;
    orb_fonts_load_stats_t fonts_before, fonts_after;
    lv_obj_raster_stats_t raster_before, raster_after;
    lv_obj_raster_scale_cache_stats_t scale_before, scale_after;
    lv_draw_sw_material_stats_t material_before, material_after;
    orb_glass_build_stats_t glass_before, glass_after;
    orb_display_accounting_t account_before, account_after;
    size_t minimum_internal;
} open_bench_t;
static orb_shell_t *s_shell; /* tentative; also declared above for wake_display() */
static open_bench_t *s_open;
static SemaphoreHandle_t s_open_done;

static void open_render_started(lv_event_t *event)
{
    (void)event;
    if (!s_open || !s_open->measured || s_open->token) return;
    orb_shell_open_metrics_t metrics;
    orb_shell_open_metrics(s_shell, &metrics);
    if (metrics.sequence != s_open->sequence ||
        lv_area_get_width(&metrics.feedback_area) < 2 ||
        lv_area_get_height(&metrics.feedback_area) < 2) return;
    /* Scaffolding is asynchronous. Read its final transformed heading bounds
     * at render start, before any band for this refresh can be queued. */
    s_open->token = orb_display_response_begin(metrics.input_us, &metrics.feedback_area);
}

void orb_devtools_set_shell(orb_shell_t *shell)
{
    if (!s_shell) lv_display_add_event_cb(lv_display_get_default(), open_render_started,
                                         LV_EVENT_RENDER_START, NULL);
    s_shell = shell;
}

static void open_finish(void)
{
    orb_display_counters(&s_open->after);
    orb_fonts_load_stats(&s_open->fonts_after);
    lv_obj_raster_get_stats(&s_open->raster_after);
    lv_obj_raster_scale_cache_get_stats(&s_open->scale_after);
    lv_draw_sw_material_get_stats(&s_open->material_after);
    orb_glass_build_stats(&s_open->glass_after);
    orb_display_accounting(&s_open->account_after);
    s_open->panel_period_us = orb_display_panel_period_us();
    s_open->minimum_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    orb_display_response_end(s_open->token);
    s_open->restoring = true;
    orb_shell_navigate(s_shell, s_open->saved);
    if (!s_open->timer) xSemaphoreGive(s_open_done);
}

static void open_tick(lv_timer_t *timer)
{
    open_bench_t *bench = s_open;
    int64_t now = esp_timer_get_time();
    if (!bench->restoring && s_wifi.active && !wifi_current()) {
        s_wifi.failed = true;
        bench->failure = "cancelled";
        open_finish();
        return;
    }
    if (bench->restoring) {
        bool restored = orb_open_source_settled(s_shell, bench->saved);
        if (!restored && now - bench->started_us <= 9000000) return;
        /* Restoration belongs to this sample's lifetime. Do not acknowledge
         * while its queued redraw can race the next source preparation. */
        if (!restored && !bench->failure) bench->failure = "restore_timeout";
        if (!orb_display_drain() && !bench->failure) bench->failure = "restore_drain";
        lv_timer_delete(timer);
        bench->timer = NULL;
        xSemaphoreGive(s_open_done);
        return;
    }
    if (now - bench->started_us > 8000000) {
        bench->failure = !bench->measured ? "source_timeout" :
                         !bench->token ? "render_timeout" :
                         !bench->settled ? "settle_timeout" : "response_timeout";
        open_finish();
        return;
    }
    orb_shell_open_metrics_t metrics;
    orb_shell_open_metrics(s_shell, &metrics);
    if (!bench->measured) {
        if (!orb_open_source_ready(&bench->setup, s_shell)) return;
        /* Drain before injecting input, never inside the measured interval.
         * Every queued pre-navigation transfer is then ineligible. */
        if (!orb_display_drain()) {
            bench->failure = "source_drain";
            open_finish();
            return;
        }
        bench->setup_us = esp_timer_get_time() - bench->started_us;
        orb_display_counters(&bench->before);
        orb_fonts_load_stats(&bench->fonts_before);
        lv_obj_raster_get_stats(&bench->raster_before);
        lv_obj_raster_scale_cache_get_stats(&bench->scale_before);
        lv_draw_sw_material_get_stats(&bench->material_before);
        orb_glass_build_stats(&bench->glass_before);
        orb_display_accounting(&bench->account_before);
        bench->duration_us = orb_ui_reduce_motion() ? 150000 : 550000;
        if (bench->back) orb_shell_back(s_shell);
        else orb_shell_navigate(s_shell, bench->target);
        orb_shell_open_metrics(s_shell, &bench->metrics);
        bench->sequence = bench->metrics.sequence;
        bench->measured = true;
        return;
    }
    if (metrics.sequence != bench->sequence || metrics.surface != bench->target) {
        bench->failure = "navigation_interrupted";
        open_finish();
        return;
    }
    bench->metrics = metrics;
    if (bench->token && !bench->settled && !metrics.active && metrics.content_ready) {
        bench->settled = true;
        orb_display_response_settled(bench->token);
        /* A final presentation proves settled pixels reached DMA, even if the
         * timer observes the perceptual endpoint just after its own refresh. */
        lv_obj_invalidate(orb_shell_root(s_shell));
    }
    orb_display_response_snapshot(&bench->response);
    /* A 5 ms timer can run before the next RENDER_START. The display keeps
     * the previous receipt after response_end(), so its nonzero timestamps
     * alone must never finish this navigation (especially the second open). */
    if (bench->token && bench->response.token == bench->token && bench->settled &&
        bench->response.settled_frame_us && bench->response.first_frame_us &&
        bench->response.first_band_us) open_finish();
}

static void open_start(void *ctx)
{
    s_open->saved = orb_shell_surface(s_shell);
    if (s_wifi.active && !wifi_current()) {
        s_wifi.failed = true;
        s_open->failure = "cancelled";
        xSemaphoreGive(s_open_done);
        return;
    }
    s_open->started_us = esp_timer_get_time();
    orb_open_source_begin(&s_open->setup, s_shell, s_open->source);
    s_open->timer = lv_timer_create(open_tick, 5, NULL);
    if (!s_open->timer) {
        s_open->failure = "timer_allocation";
        open_finish();
    }
}

static bool open_error(const char *name, const char *reason)
{
    command_reply_printf("\n@@ORBDEV ERROR bench_open surface=%s reason=%s\n", name, reason);
    return false;
}

static void open_failure_report(const open_bench_t *bench, const char *reason)
{
    orb_shell_open_metrics_t live;
    orb_shell_open_metrics(s_shell, &live);
    command_reply_printf("\n@@ORBDEV ERROR bench_open surface=%s reason=%s measured=%d settled=%d restoring=%d "
           "source=%d stage=%d saved=%d live_surface=%d live_sequence=%lu active=%d content_ready=%d "
           "sequence=%lu token=%lu response_token=%lu causal=%d input_us=%lld first_band_us=%lld "
           "first_frame_us=%lld settled_frame_us=%lld transfer_errors=%llu/%llu failed_frames=%llu/%llu\n",
           bench->name, reason, bench->measured, bench->settled, bench->restoring,
           bench->source, bench->setup.stage, bench->saved, live.surface, (unsigned long)live.sequence,
           live.active, live.content_ready, (unsigned long)bench->sequence, (unsigned long)bench->token,
           (unsigned long)bench->response.token, bench->response.causal, (long long)bench->metrics.input_us,
           (long long)bench->response.first_band_us, (long long)bench->response.first_frame_us,
           (long long)bench->response.settled_frame_us,
           (unsigned long long)bench->before.transfer_errors, (unsigned long long)bench->after.transfer_errors,
           (unsigned long long)bench->before.failed_frames, (unsigned long long)bench->after.failed_frames);
}

static bool benchmark_open(const char *name) __attribute__((noinline));

static bool open_success_report(const open_bench_t *bench)
{
    const orb_shell_open_metrics_t *m = &bench->metrics;
    const orb_display_response_t *r = &bench->response;
    return command_reply_printf("\n@@ORBDEV OPEN surface=%s origin=injected_navigation cold=%d sequence=%lu causal=%d first_changed_band_us=%lld first_changed_frame_us=%lld transition_end_us=%lld motion_end_us=%lld motion_duration_us=%lu panel_period_us=%lu source_prepare_us=%lld object_us=%llu max_slice_us=%llu build_slices=%lu font_loads=%lu font_failures=%lu font_us=%llu raster_builds=%lu raster_us=%llu scale_raster_us=%lu raster_timing=%d material_base_builds=%llu glass_mask_builds=%lu glass_mask_failures=%lu glass_mask_us=%llu glass_us=%llu glass_timing=%d material_counters=%d blur_kernels=%llu accounting_timing=%d preparation_us=%llu scene_prepare_us=%llu whole_update_us=%llu draw_us=%llu wire_us=%llu bytes=%llu frames=%llu errors=%llu failed_frames=%llu minimum_internal=%u cache_bytes=%u cache_entries=%lu\n",
           bench->name, m->cold, (unsigned long)m->sequence, r->causal,
           (long long)(r->first_band_us - m->input_us),
           (long long)(r->first_frame_us - m->input_us),
           (long long)(r->settled_frame_us - m->input_us),
           (long long)(m->transition_end_us - m->input_us),
           (unsigned long)bench->duration_us, (unsigned long)bench->panel_period_us,
           (long long)bench->setup_us, (unsigned long long)m->object_us,
           (unsigned long long)m->max_slice_us,
           (unsigned long)m->build_slices,
           (unsigned long)(bench->fonts_after.loads - bench->fonts_before.loads),
           (unsigned long)(bench->fonts_after.failures - bench->fonts_before.failures),
           (unsigned long long)(bench->fonts_after.elapsed_us - bench->fonts_before.elapsed_us),
           (unsigned long)(bench->raster_after.builds - bench->raster_before.builds),
           (unsigned long long)(bench->raster_after.build_us - bench->raster_before.build_us),
           (unsigned long)(uint32_t)(bench->scale_after.build_us - bench->scale_before.build_us),
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
           true,
#else
           false,
#endif
           (unsigned long long)(bench->material_after.base_builds - bench->material_before.base_builds),
           (unsigned long)(bench->glass_after.builds - bench->glass_before.builds),
           (unsigned long)(bench->glass_after.failures - bench->glass_before.failures),
           (unsigned long long)(bench->glass_after.elapsed_us - bench->glass_before.elapsed_us),
           (unsigned long long)(bench->material_after.preparation_us - bench->material_before.preparation_us),
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING
           true,
#else
           false,
#endif
#if CONFIG_ORB_DISPLAY_RENDER_ACCOUNTING || CONFIG_ORB_DISPLAY_RETAINED_MATERIAL
           true,
#else
           false,
#endif
           (unsigned long long)(bench->material_after.blur_kernels - bench->material_before.blur_kernels),
           orb_display_accounting_active(),
           (unsigned long long)(bench->account_after.preparation_us - bench->account_before.preparation_us),
           (unsigned long long)(bench->account_after.scene_prepare_us - bench->account_before.scene_prepare_us),
           (unsigned long long)(bench->account_after.whole_update_us - bench->account_before.whole_update_us),
           (unsigned long long)(bench->after.draw_us - bench->before.draw_us),
           (unsigned long long)(bench->after.send_us - bench->before.send_us),
           (unsigned long long)(bench->after.bytes - bench->before.bytes),
           (unsigned long long)(bench->after.frames - bench->before.frames),
           (unsigned long long)(bench->after.transfer_errors - bench->before.transfer_errors),
           (unsigned long long)(bench->after.failed_frames - bench->before.failed_frames),
           (unsigned)bench->minimum_internal, (unsigned)m->cache_bytes,
           (unsigned long)m->cache_entries);
}

static bool benchmark_open(const char *name)
{
    orb_shell_surface_t target;
    if (!strcmp(name, "play")) target = ORB_SHELL_PLAY;
    else if (!strcmp(name, "dot")) target = ORB_SHELL_DOT;
    else if (!strcmp(name, "agents")) target = ORB_SHELL_AGENTS;
    else if (!strcmp(name, "apps") || !strcmp(name, "back")) target = ORB_SHELL_APPS;
    else return open_error(name, "invalid_surface");
    if (!s_open_done) s_open_done = xSemaphoreCreateBinary();
    if (!s_open_done) return open_error(name, "semaphore_allocation");
    lv_lock();
    if (!s_shell || s_open || s_bench_active) {
        const char *reason = !s_shell ? "no_shell" : "busy";
        lv_unlock();
        return open_error(name, reason);
    }
    s_open = heap_caps_calloc(1, sizeof(*s_open), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_open) { lv_unlock(); return open_error(name, "receipt_allocation"); }
    s_open->name = name;
    s_open->target = target;
    s_open->back = !strcmp(name, "back");
    s_open->source = s_open->back ? ORB_SHELL_PLAY :
                     target == ORB_SHELL_APPS ? ORB_SHELL_FACE : ORB_SHELL_APPS;
    xSemaphoreTake(s_open_done, 0);
    bool queued = lv_async_call(open_start, NULL) == LV_RESULT_OK;
    if (queued) orb_display_wake_service();
    lv_unlock();
    bool completed = queued && xSemaphoreTake(s_open_done, pdMS_TO_TICKS(10000)) == pdTRUE;
    if (!completed) {
        /* Cancel under the UI lock before freeing either an async callback's
         * receipt or a timer's receipt. A late UI recovery cannot use it again. */
        lv_lock();
        orb_display_counters(&s_open->after);
        orb_display_response_snapshot(&s_open->response);
        open_failure_report(s_open, queued ? "wait_timeout" : "async_queue");
        if (queued) {
            lv_async_call_cancel(open_start, NULL);
            if (s_open->timer) lv_timer_delete(s_open->timer);
            if (s_open->started_us) orb_shell_navigate(s_shell, s_open->saved);
        }
        orb_display_response_end(s_open->token);
        heap_caps_free(s_open);
        s_open = NULL;
        lv_unlock();
        return false;
    }
    const open_bench_t *bench = s_open;
    const orb_shell_open_metrics_t *m = &bench->metrics;
    const orb_display_response_t *r = &bench->response;
    const char *failure = bench->failure;
    if (!failure && (!bench->token || r->token != bench->token)) failure = "response_token";
    if (!failure && !r->causal) failure = "noncausal_response";
    if (!failure && (!r->first_band_us || r->first_band_us < m->input_us)) failure = "first_band_order";
    if (!failure && r->first_frame_us < r->first_band_us) failure = "first_frame_order";
    if (!failure && r->settled_frame_us < r->first_frame_us) failure = "settled_frame_order";
    if (!failure && bench->after.transfer_errors != bench->before.transfer_errors) failure = "transfer_errors";
    if (!failure && bench->after.failed_frames != bench->before.failed_frames) failure = "failed_frames";
    bool ok = failure == NULL;
    if (failure) {
        lv_lock();
        open_failure_report(bench, failure);
        lv_unlock();
    }
    if (ok) ok = open_success_report(bench);
    lv_lock();
    heap_caps_free(s_open);
    s_open = NULL;
    lv_unlock();
    return ok;
}

/* End live-shell opening benchmark. */

/* Games use the production logic and view without entering the app registry:
 * a synthetic benchmark must never overwrite a real player's high score. All
 * retained model/receipt memory lives in PSRAM, not the console/UI stack. */
typedef struct {
    orb_game_state_t state;
    orb_game_state_t published_state, render_state;
    orb_game_pose_t pose, published_pose, render_pose;
    orb_game_view_t *view;
    lv_obj_t *screen, *saved_screen;
    lv_timer_t *timer;
    orb_display_counters_t before, after;
    orb_game_view_stats_t view_before, view_after;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot_t profile_before, profile_after;
#endif
    int64_t started_us, previous_us, next_present_us, stopped_us;
    uint64_t accumulator_us, steps, dropped_steps, skipped_slots, cold_us;
    uint64_t logic_us, logic_max_us;
    orb_devtools_game_cpu_t cpu;
    orb_devtools_sender_cpu_t sender;
    uint64_t marble_gate_steps, marble_rewind_steps, marble_stages;
    uint16_t marble_history_peak;
    uint32_t duration_ms, period_us, panel_period_us;
    size_t minimum_internal, largest_internal;
    bool failed, reserved, reduce_motion, parallel_logic, refresh_had_damage, frame_credit;
    TaskHandle_t worker;
    SemaphoreHandle_t publication_lock, worker_done;
    atomic_bool worker_stop;
    uint64_t published_accumulator_us;
    int64_t published_at_us;
} game_bench_t;

static game_bench_t *s_game_bench;
static void game_benchmark_refresh_start(lv_event_t *event);
static void game_benchmark_refresh_ready(lv_event_t *event);

static void game_benchmark_finish(void)
{
    game_bench_t *bench = s_game_bench;
    if (bench->timer) lv_timer_delete(bench->timer);
    bench->timer = NULL;
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(),
        game_benchmark_refresh_start, bench);
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(),
        game_benchmark_refresh_ready, bench);
    if (bench->worker) {
        /* The worker never takes LVGL. Stop before freezing counters or
         * releasing its model, including a timed-out/cancelled command. */
        atomic_store(&bench->worker_stop, true);
        xTaskNotifyGive(bench->worker);
        bool joined = xSemaphoreTake(bench->worker_done, pdMS_TO_TICKS(3000)) == pdTRUE;
        if (!joined) bench->failed = true;
        orb_devtools_game_cpu_stop(&bench->cpu, bench->worker, joined);
        vTaskDelete(bench->worker);
        bench->worker = NULL;
    }
    if (bench->publication_lock) vSemaphoreDelete(bench->publication_lock);
    if (bench->worker_done) vSemaphoreDelete(bench->worker_done);
    bench->publication_lock = bench->worker_done = NULL;
    /* Final refresh plus the SPI fence counts the final sprite update; none of
     * the restored shell's bands may leak into this receipt. */
    if (bench->view) {
        lv_refr_now(NULL);
        if (!orb_display_drain()) bench->failed = true;
        orb_game_view_stats(bench->view, &bench->view_after);
    }
    orb_devtools_sender_end(&bench->sender);
    orb_display_counters(&bench->after);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot(&bench->profile_after);
#endif
    bench->stopped_us = esp_timer_get_time();
    bench->minimum_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    bench->largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (bench->saved_screen) lv_screen_load(bench->saved_screen);
    if (bench->view) orb_game_view_destroy(bench->view);
    if (bench->screen) lv_obj_delete(bench->screen);
    bench->view = NULL;
    bench->screen = NULL;
    if (bench->reserved) orb_games_benchmark_release();
    bench->reserved = false;
    s_bench_active = false;
    xSemaphoreGive(s_bench_done);
}

static void game_benchmark_drive(game_bench_t *bench)
{
    /* Repeatable movement, ring changes, polar turns and taps. Restarts are
     * recorded game events, so a collision cannot turn a long run into idle. */
    if (bench->state.game_over)
        orb_game_action(&bench->state, ORB_GAME_RESTART, 0, 0);
    if (bench->state.kind == ORB_GAME_MARBLE) {
        if (bench->state.game.marble.stage_cleared) {
            ++bench->marble_stages;
            orb_game_action(&bench->state, ORB_GAME_PRIMARY, 0, 0);
        }
        /* Exercise both path directions while gates continue forward. At the
         * end of each six-second fixture, place the marble at the real goal
         * and let production rules complete it. This visits the denser stages
         * without depending on a synthetic tilt trace solving the puzzle. */
        uint64_t phase = bench->steps % 360;
        if (phase == 90) orb_game_action(&bench->state, ORB_GAME_REWIND_START, 0, 0);
        if (phase == 150) orb_game_action(&bench->state, ORB_GAME_REWIND_STOP, 0, 0);
        if (phase == 300) {
            orb_game_action(&bench->state, ORB_GAME_REWIND_STOP, 0, 0);
            bench->state.game.marble.x = bench->state.game.marble.y = 0;
            bench->state.game.marble.vx = bench->state.game.marble.vy = 0;
        }
        for (unsigned i = 0; i < ORB_MARBLE_GATES_MAX; ++i)
            if (bench->state.game.marble.gates[i].active) ++bench->marble_gate_steps;
        if (bench->state.game.marble.rewind_active) ++bench->marble_rewind_steps;
    }
    if (bench->state.kind == ORB_GAME_ORBIT && bench->steps % 90 == 0)
        orb_game_action(&bench->state, ORB_GAME_PRIMARY, 0, 0);
    if (bench->state.kind == ORB_GAME_SNAKE && bench->steps % 120 == 0)
        orb_game_action(&bench->state, ORB_GAME_RIGHT, 0, 0);
    if (bench->state.kind == ORB_GAME_POP && bench->steps % 90 == 0) {
        for (size_t i = 0; i < ORB_GAME_BUBBLES; ++i) {
            const orb_game_bubble_t *bubble = &bench->state.game.pop.bubbles[i];
            if (!bubble->active) continue;
            orb_game_action(&bench->state, ORB_GAME_TAP,
                            ORB_GAME_CX + bubble->x, ORB_GAME_CY + bubble->y);
            break;
        }
    }
    float gx = (bench->steps / 180) % 2 ? -0.32f : 0.32f;
    float gy = (bench->steps / 120) % 2 ? -0.24f : 0.24f;
    if (bench->state.kind == ORB_GAME_BEADS) {
        /* Constant tilt lets a physical pile become still. This fixture
         * measures a continuous pour, not deliberate unchanged pixels. */
        float phase = (float)bench->steps * (ORB_GAME_STEP_US / 1000000.f) *
            (6.28318530718f / 2.4f);
        gx = .55f * cosf(phase);
        gy = .4f * sinf(phase);
        bench->state.motion_moving = true;
        bench->state.game.beads.settle_ticks = 0;
    }
    orb_game_pose_capture(&bench->state, &bench->pose);
    int64_t logic_start = esp_timer_get_time();
    orb_game_step(&bench->state, gx, gy, true, bench->reduce_motion);
    uint64_t elapsed_us = esp_timer_get_time() - logic_start;
    bench->logic_us += elapsed_us;
    if (elapsed_us > bench->logic_max_us) bench->logic_max_us = elapsed_us;
    if (bench->state.kind == ORB_GAME_MARBLE &&
        bench->state.game.marble.history_count > bench->marble_history_peak)
        bench->marble_history_peak = bench->state.game.marble.history_count;
    ++bench->steps;
}

static void game_benchmark_publish(game_bench_t *bench)
{
    xSemaphoreTake(bench->publication_lock, portMAX_DELAY);
    memcpy(&bench->published_state, &bench->state, orb_game_snapshot_bytes(bench->state.kind));
    bench->published_pose = bench->pose;
    bench->published_accumulator_us = bench->accumulator_us;
    bench->published_at_us = bench->previous_us;
    xSemaphoreGive(bench->publication_lock);
}

static void game_benchmark_worker(void *context)
{
    game_bench_t *bench = context;
    /* Creation is cold setup. The UI releases this gate only after the timed
     * snapshots and clocks are initialized. No LVGL calls run on this task. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!atomic_load(&bench->worker_stop)) {
        int64_t now = esp_timer_get_time();
        bench->accumulator_us += now - bench->previous_us;
        bench->previous_us = now;
        unsigned work = 0;
        while (bench->accumulator_us >= ORB_GAME_STEP_US && work < 6 &&
               !atomic_load(&bench->worker_stop)) {
            game_benchmark_drive(bench);
            bench->accumulator_us -= ORB_GAME_STEP_US;
            ++work;
        }
        if (bench->accumulator_us >= ORB_GAME_STEP_US && !atomic_load(&bench->worker_stop)) {
            bench->dropped_steps += bench->accumulator_us / ORB_GAME_STEP_US;
            bench->accumulator_us %= ORB_GAME_STEP_US;
        }
        if (work) game_benchmark_publish(bench);
        int64_t remaining = ORB_GAME_STEP_US - (int64_t)bench->accumulator_us -
            (esp_timer_get_time() - bench->previous_us);
        TickType_t wait = remaining > 1000 ? pdMS_TO_TICKS((uint32_t)(remaining / 1000)) : 0;
        if (wait) ulTaskNotifyTake(pdTRUE, wait);
        else taskYIELD();
    }
    /* Remain alive until the UI deletes the task: a stop notification cannot
     * race a self-deleted task handle. No model access follows this fence. */
    xSemaphoreGive(bench->worker_done);
    vTaskSuspend(NULL);
}

static bool game_benchmark_present_due(game_bench_t *bench, int64_t now)
{
    bool due = now >= bench->next_present_us;
    if (!due && (!bench->frame_credit || !orb_display_te_pacing_active())) return false;
    bench->frame_credit = false;
    /* Early completion credit is valid only behind the live TE gate. With
     * admission off, preserve one absolute producer slot per panel period. */
    if (due) {
        bench->next_present_us += bench->period_us;
        if (bench->next_present_us <= now) {
            uint64_t skipped = (now - bench->next_present_us) / bench->period_us + 1;
            bench->skipped_slots += skipped;
            bench->next_present_us += skipped * bench->period_us;
        }
    }
    return true;
}

static void game_benchmark_render_latest(game_bench_t *bench)
{
    int64_t now = esp_timer_get_time();
    float alpha;
    if (bench->worker) {
        xSemaphoreTake(bench->publication_lock, portMAX_DELAY);
        memcpy(&bench->render_state, &bench->published_state,
            orb_game_snapshot_bytes(bench->published_state.kind));
        bench->render_pose = bench->published_pose;
        alpha = (bench->published_accumulator_us +
            LV_MAX(0, now - bench->published_at_us)) / (float)ORB_GAME_STEP_US;
        xSemaphoreGive(bench->publication_lock);
    } else {
        memcpy(&bench->render_state, &bench->state, orb_game_snapshot_bytes(bench->state.kind));
        bench->render_pose = bench->pose;
        alpha = (bench->accumulator_us + LV_MAX(0, now - bench->previous_us)) / (float)ORB_GAME_STEP_US;
    }
    orb_game_pose_interpolate(&bench->render_state, &bench->render_pose, alpha);
    orb_game_view_update(bench->view, &bench->render_state, 0, false, false);
}

static void game_benchmark_refresh_start(lv_event_t *event)
{
    game_bench_t *bench = lv_event_get_user_data(event);
    bench->refresh_had_damage = bench->view && lv_screen_active() == bench->screen &&
        orb_display_refresh_pending();
    if (bench->refresh_had_damage) game_benchmark_render_latest(bench);
}

static void game_benchmark_refresh_ready(lv_event_t *event)
{
    game_bench_t *bench = lv_event_get_user_data(event);
    if (!bench->refresh_had_damage) return;
    bench->refresh_had_damage = false;
    if (orb_display_te_pacing_active()) {
        bench->frame_credit = true;
        if (bench->timer) lv_timer_ready(bench->timer);
    }
}

static void game_benchmark_tick(lv_timer_t *timer)
{
    if (s_wifi.active && !wifi_current()) {
        s_wifi.failed = true;
        s_game_bench->failed = true;
        game_benchmark_finish();
        return;
    }
    game_bench_t *bench = s_game_bench;
    int64_t now = esp_timer_get_time();
    if (now - bench->started_us >= bench->duration_ms * 1000LL) {
        game_benchmark_finish();
        return;
    }
    if (!bench->worker) {
        bench->accumulator_us += now - bench->previous_us;
        bench->previous_us = now;
        unsigned work = 0;
        while (bench->accumulator_us >= ORB_GAME_STEP_US && work++ < 6) {
            game_benchmark_drive(bench);
            bench->accumulator_us -= ORB_GAME_STEP_US;
        }
        if (bench->accumulator_us >= ORB_GAME_STEP_US) {
            bench->dropped_steps += bench->accumulator_us / ORB_GAME_STEP_US;
            bench->accumulator_us %= ORB_GAME_STEP_US;
        }
    }
    if (game_benchmark_present_due(bench, now)) {
        /* Synthetic input is activity too: idle power/OTA policy must not
         * change the screen or flash workload partway through a 60 s run. */
        lv_display_trigger_activity(NULL);
        /* One armed refresh owns damage until TE admission. Sample its latest
         * pose in REFR_START instead of flooding the pending damage array. */
        if (!orb_display_refresh_pending()) game_benchmark_render_latest(bench);
    }
    /* Fixed-step simulation stays independent of the sampled panel period.
     * The display owns TE admission; this timer only publishes a fresh pose. */
    int64_t remaining = bench->next_present_us - esp_timer_get_time();
    lv_timer_set_period(timer, remaining > 1000 ? (uint32_t)((remaining + 999) / 1000) : 1);
    lv_timer_reset(timer);
}

static void game_benchmark_start(void *context)
{
    game_bench_t *bench = s_game_bench;
    if ((s_wifi.active && !wifi_current()) || orb_apps_active() || !orb_display_drain()) {
        bench->failed = true;
        game_benchmark_finish();
        return;
    }
    bench->saved_screen = lv_screen_active();
    lv_display_trigger_activity(NULL);
    int64_t cold_start = esp_timer_get_time();
    bench->screen = lv_obj_create(NULL);
    if (bench->screen)
        bench->view = orb_game_view_create(bench->screen, bench->state.kind, NULL, NULL);
    if (!bench->view) {
        bench->failed = true;
        game_benchmark_finish();
        return;
    }
    orb_game_view_update(bench->view, &bench->state, 0, false, false);
    lv_screen_load(bench->screen);
    lv_refr_now(NULL);
    if (!orb_display_drain()) {
        bench->failed = true;
        game_benchmark_finish();
        return;
    }
    bench->reduce_motion = orb_ui_reduce_motion();
    orb_devtools_game_cpu_begin(&bench->cpu);
    orb_devtools_sender_begin(&bench->sender);
    if (bench->state.kind == ORB_GAME_BEADS) {
        /* Production advances physics on core 0 while LVGL draws on core 1.
         * Serial solver+draw in a UI timer misreported Beads throughput. */
        bench->publication_lock = xSemaphoreCreateMutex();
        bench->worker_done = xSemaphoreCreateBinary();
        atomic_init(&bench->worker_stop, false);
        if (!bench->publication_lock || !bench->worker_done ||
            xTaskCreatePinnedToCore(game_benchmark_worker, "game_bench", 4096, bench, 1,
                                   &bench->worker, 0) != pdPASS) {
            bench->failed = true;
            game_benchmark_finish();
            return;
        }
        bench->parallel_logic = true;
    }
    if (!lv_display_add_event_cb(lv_display_get_default(), game_benchmark_refresh_start,
                                LV_EVENT_REFR_START, bench) ||
        !lv_display_add_event_cb(lv_display_get_default(), game_benchmark_refresh_ready,
                                LV_EVENT_REFR_READY, bench)) {
        bench->failed = true;
        game_benchmark_finish();
        return;
    }
    bench->cold_us = esp_timer_get_time() - cold_start;
    bench->panel_period_us = orb_display_panel_period_us();
    bench->period_us = bench->panel_period_us ? bench->panel_period_us : ORB_GAME_STEP_US;
    bench->started_us = bench->previous_us = esp_timer_get_time();
    if (bench->worker) game_benchmark_publish(bench);
    bench->next_present_us = bench->started_us + bench->period_us;
    orb_display_counters(&bench->before);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot(&bench->profile_before);
#endif
    orb_game_view_stats(bench->view, &bench->view_before);
    bench->timer = lv_timer_create(game_benchmark_tick, (bench->period_us + 999) / 1000, NULL);
    if (!bench->timer) {
        bench->failed = true;
        game_benchmark_finish();
    } else if (bench->worker) {
        xTaskNotifyGive(bench->worker);
    }
}

static bool benchmark_game(const char *id, int ms) __attribute__((noinline));

static bool benchmark_game(const char *id, int ms)
{
    orb_game_kind_t kind;
    if (!orb_game_kind_from_id(id, &kind)) return false;
    if (!s_bench_done) s_bench_done = xSemaphoreCreateBinary();
    if (!s_bench_done) return false;
    if (!orb_devtools_bench_power_ready(s_shell, wake_display)) return false;
    lv_lock();
    if (s_bench_active || s_open || s_game_bench || orb_apps_active()) {
        lv_unlock();
        return false;
    }
    /* Reserve before the model's entry allocation/LVGL lock: active() is not
     * published until entry returns, so that pointer alone leaves a race. */
    if (!orb_games_benchmark_reserve()) { lv_unlock(); return false; }
    s_game_bench = heap_caps_calloc(1, sizeof(*s_game_bench), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_game_bench) {
        orb_games_benchmark_release();
        lv_unlock();
        return false;
    }
    s_game_bench->reserved = true;
    orb_game_init(&s_game_bench->state, kind, 0x4f5242u);
    s_game_bench->duration_ms = (uint32_t)ms;
    xSemaphoreTake(s_bench_done, 0);
    s_bench_active = true;
    bool queued = lv_async_call(game_benchmark_start, NULL) == LV_RESULT_OK;
    if (queued) orb_display_wake_service();
    if (!queued) {
        orb_games_benchmark_release();
        s_game_bench->reserved = false;
        s_bench_active = false;
    }
    lv_unlock();
    bool complete = queued && xSemaphoreTake(s_bench_done, pdMS_TO_TICKS(ms + 10000)) == pdTRUE;
    lv_lock();
    if (!complete && queued) {
        lv_async_call_cancel(game_benchmark_start, NULL);
        s_game_bench->failed = true;
        game_benchmark_finish();
    }
    const game_bench_t *bench = s_game_bench;
    bool ok = complete && !bench->failed && bench->after.frames > bench->before.frames &&
              bench->after.transfer_errors == bench->before.transfer_errors &&
              bench->after.failed_frames == bench->before.failed_frames;
    if (complete && !bench->failed) {
        uint64_t frames = bench->after.frames - bench->before.frames;
        double count = frames ? frames : 1;
        double seconds = (bench->stopped_us - bench->started_us) / 1e6;
        double dirty = (bench->after.bytes - bench->before.bytes) / (2.0 * count);
        uint64_t updates = bench->view_after.updates - bench->view_before.updates;
        printf("\n@@ORBDEV GAME game=%s seconds=%.3f frames=%llu fps=%.2f frame_ms=%.3f draw_ms=%.3f wait_ms=%.3f hold_ms=%.3f wire_ms=%.3f bands_per_frame=%.3f dirty_px=%.1f dirty_pct=%.3f dirty_basis=panel_transferred_pixels view_updates=%llu view_changed_updates=%llu view_dirty_px=%llu view_update_ms=%.3f logic_mode=%s workload=%s logic_steps=%llu logic_mean_us=%.3f logic_max_us=%llu dropped_steps=%llu skipped_slots=%llu panel_period_us=%lu target_period_us=%lu te_phase_locked=%d cold_us=%llu errors=%llu failed_frames=%llu minimum_internal=%u largest_internal=%u marble_gate_steps=%llu marble_rewind_steps=%llu marble_stages=%llu marble_history_peak=%u\n",
               id, seconds, (unsigned long long)frames, seconds > 0 ? frames / seconds : 0,
               (bench->after.frame_us - bench->before.frame_us) / count / 1000,
               (bench->after.draw_us - bench->before.draw_us) / count / 1000,
               (bench->after.wait_us - bench->before.wait_us) / count / 1000,
               (bench->after.hold_us - bench->before.hold_us) / count / 1000,
               (bench->after.send_us - bench->before.send_us) / count / 1000,
               (bench->after.bands - bench->before.bands) / count,
               dirty, dirty * 100 / (466.0 * 466.0), (unsigned long long)updates,
               (unsigned long long)(bench->view_after.changed_updates - bench->view_before.changed_updates),
               (unsigned long long)(bench->view_after.dirty_pixels - bench->view_before.dirty_pixels),
               (bench->view_after.update_us - bench->view_before.update_us) / (updates ? updates : 1.0) / 1000,
               bench->parallel_logic ? "core0_worker" : "ui_serial",
               bench->state.kind == ORB_GAME_BEADS ? "continuous_pour" : "repeatable_play",
               (unsigned long long)bench->steps, bench->logic_us / (bench->steps ? (double)bench->steps : 1),
               (unsigned long long)bench->logic_max_us, (unsigned long long)bench->dropped_steps,
               (unsigned long long)bench->skipped_slots, (unsigned long)bench->panel_period_us,
               (unsigned long)bench->period_us,
               frames && frames == bench->after.paced_scheduled - bench->before.paced_scheduled &&
                   bench->after.paced_scheduled - bench->before.paced_scheduled ==
                       bench->after.paced_on_slot - bench->before.paced_on_slot &&
                   bench->after.paced_late == bench->before.paced_late &&
                   bench->after.paced_skipped == bench->before.paced_skipped,
               (unsigned long long)bench->cold_us,
               (unsigned long long)(bench->after.transfer_errors - bench->before.transfer_errors),
               (unsigned long long)(bench->after.failed_frames - bench->before.failed_frames),
               (unsigned)bench->minimum_internal, (unsigned)bench->largest_internal,
               (unsigned long long)bench->marble_gate_steps, (unsigned long long)bench->marble_rewind_steps,
               (unsigned long long)bench->marble_stages, (unsigned)bench->marble_history_peak);
        orb_devtools_game_cpu_report(&bench->cpu, bench->steps, devtools_printf);
        orb_devtools_sender_report(&bench->sender, frames, devtools_printf);
        if (bench->state.kind == ORB_GAME_BEADS)
            orb_devtools_game_beads_report(&bench->view_before, &bench->view_after, devtools_printf);
        ok = benchmark_distribution_report("game", id, &bench->before, &bench->after) && ok;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
        benchmark_task_report(id, &bench->profile_before, &bench->profile_after);
#endif
    }
    heap_caps_free(s_game_bench);
    s_game_bench = NULL;
    lv_unlock();
    return ok;
}

/* Navigation uses the shell's production compositor; the timer supplies only
 * position samples. A sample is not a frame. Only the display's successful
 * final-DMA counter contributes to fps (architecture-rendering.md stage 5). */
typedef struct {
    char gesture[16];
    lv_timer_t *timer;
    orb_display_counters_t before, after;
    orb_display_accounting_t account_before, account_after;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot_t profile_before, profile_after;
#endif
    lv_obj_raster_stats_t raster_before, raster_after;
    int64_t started_us, stopped_us;
    uint64_t cold_us, samples;
    uint32_t duration_ms, panel_period_us;
    size_t minimum_internal, largest_internal;
    multi_heap_info_t memory[3];
    bool failed, begun, memory_active;
} nav_bench_t;

static nav_bench_t *s_nav_bench;

static void navigation_failure(orb_nav_admission_reason_t reason)
{
    if (reason != ORB_NAV_ADMISSION_NONE) orb_nav_admission_fail(reason);
    if (s_nav_bench) s_nav_bench->failed = true;
}

static void navigation_receipt(bool error)
{
    lv_lock();
    orb_nav_admission_receipt_t receipt = orb_nav_admission_last();
    lv_unlock();
    command_reply_printf("\n@@ORBDEV %s reason=%s sequence=%lu psram_free=%u psram_largest=%u\n",
        error ? "ERROR bench_nav" : "NAV_ADMISSION", orb_nav_admission_name(receipt.reason),
        (unsigned long)receipt.sequence, (unsigned)receipt.psram_free, (unsigned)receipt.psram_largest);
}

static bool navigation_gesture_valid(const char *name)
{
    return !strcmp(name, "slide-left") || !strcmp(name, "slide-right") ||
           !strcmp(name, "stack-up") || !strcmp(name, "shade-down") ||
           !strcmp(name, "back") || !strcmp(name, "iris") || !strcmp(name, "rotary");
}

static void navigation_benchmark_finish(void)
{
    nav_bench_t *bench = s_nav_bench;
    if (bench->timer) lv_timer_delete(bench->timer);
    bench->timer = NULL;
    if (bench->started_us) {
        /* Capture before restoring the original surface: restoration pixels
         * cannot inflate the navigation receipt. Fence the last submitted frame. */
        lv_refr_now(NULL);
        if (!orb_display_drain() && !bench->failed) navigation_failure(ORB_NAV_ADMISSION_DRAIN);
        orb_display_counters(&bench->after);
        orb_display_accounting(&bench->account_after);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
        lv_draw_sw_profile_snapshot(&bench->profile_after);
#endif
        lv_obj_raster_get_stats(&bench->raster_after);
        bench->stopped_us = esp_timer_get_time();
        const uint32_t caps[] = {MALLOC_CAP_INTERNAL, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, MALLOC_CAP_SPIRAM};
        if (bench->memory_active) {
            for (unsigned i = 0; i < 3; i++) orb_mem_phase_info(ORB_MEM_NAV, &bench->memory[i], caps[i]);
            orb_mem_end(ORB_MEM_NAV);
            bench->memory_active = false;
        }
        multi_heap_info_t historical;
        orb_mem_heap_info(&historical, MALLOC_CAP_INTERNAL);
        bench->minimum_internal = historical.minimum_free_bytes;
        bench->largest_internal = historical.largest_free_block;
    }
    if (bench->begun) {
        if (!orb_shell_nav_bench(s_shell, bench->gesture, 0, true) && !bench->failed)
            navigation_failure(ORB_NAV_ADMISSION_WORKSPACE_HOLD);
        lv_refr_now(NULL);
        if (!orb_display_drain() && !bench->failed) navigation_failure(ORB_NAV_ADMISSION_DRAIN);
    }
    bench->begun = false;
    s_bench_active = false;
    xSemaphoreGive(s_bench_done);
}

static void navigation_benchmark_tick(lv_timer_t *timer)
{
    (void)timer;
    nav_bench_t *bench = s_nav_bench;
    if (s_wifi.active && !wifi_current()) {
        s_wifi.failed = true;
        navigation_failure(ORB_NAV_ADMISSION_BUSY);
        navigation_benchmark_finish();
        return;
    }
    uint32_t elapsed_ms = (esp_timer_get_time() - bench->started_us) / 1000;
    if (elapsed_ms >= bench->duration_ms) {
        navigation_benchmark_finish();
        return;
    }
    lv_display_trigger_activity(NULL);
    if (!orb_shell_nav_bench(s_shell, bench->gesture, elapsed_ms ? elapsed_ms : 1, false)) {
        navigation_failure(ORB_NAV_ADMISSION_WORKSPACE_HOLD);
        navigation_benchmark_finish();
        return;
    }
    bench->samples++;
}

static void navigation_benchmark_start(void *context)
{
    (void)context;
    nav_bench_t *bench = s_nav_bench;
    if (s_wifi.active && !wifi_current()) {
        s_wifi.failed = true;
        navigation_failure(ORB_NAV_ADMISSION_BUSY);
        navigation_benchmark_finish();
        return;
    }
    if (!s_shell || orb_apps_active()) {
        navigation_failure(ORB_NAV_ADMISSION_BUSY);
        navigation_benchmark_finish();
        return;
    }
    if (!orb_display_drain()) {
        navigation_failure(ORB_NAV_ADMISSION_DRAIN);
        navigation_benchmark_finish();
        return;
    }
    int64_t cold_started = esp_timer_get_time();
    /* The fixture creates/caches source surfaces once here. Its continuing
     * elapsed samples follow the same cached-pixel path as a physical drag. */
    bench->begun = true;
    if (!orb_shell_nav_bench(s_shell, bench->gesture, 0, false)) {
        navigation_failure(ORB_NAV_ADMISSION_NONE);
        navigation_benchmark_finish();
        return;
    }
    lv_refr_now(NULL);
    if (!orb_display_drain()) {
        navigation_failure(ORB_NAV_ADMISSION_DRAIN);
        navigation_benchmark_finish();
        return;
    }
    bench->cold_us = esp_timer_get_time() - cold_started;
    bench->panel_period_us = orb_display_panel_period_us();
    orb_display_counters(&bench->before);
    orb_display_accounting(&bench->account_before);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot(&bench->profile_before);
#endif
    lv_obj_raster_get_stats(&bench->raster_before);
    orb_mem_begin(ORB_MEM_NAV);
    bench->memory_active = true;
    bench->started_us = esp_timer_get_time();
    /* Input samples at 125 Hz leave the board fps gate to the renderer; this
     * timer neither caps at 30 Hz nor turns attempted samples into frames. */
    bench->timer = lv_timer_create(navigation_benchmark_tick, 8, NULL);
    if (!bench->timer) {
        navigation_failure(ORB_NAV_ADMISSION_BUSY);
        navigation_benchmark_finish();
    }
}

static double navigation_p95_upper_ms(const uint32_t *before, const uint32_t *after,
                                      unsigned buckets, unsigned bucket_us)
{
    return orb_devtools_percentile_upper_ms(before, after, buckets, bucket_us, 95);
}

static bool benchmark_navigation(const char *gesture, int ms) __attribute__((noinline));

static bool benchmark_navigation(const char *gesture, int ms)
{
    if (!navigation_gesture_valid(gesture)) return false;
    if (!orb_devtools_bench_power_ready(s_shell, wake_display)) return false;
    if (!s_bench_done) s_bench_done = xSemaphoreCreateBinary();
    if (!s_bench_done) {
        lv_lock();
        orb_nav_admission_fail(ORB_NAV_ADMISSION_BUSY);
        lv_unlock();
        return false;
    }
    lv_lock();
    if (!s_shell || s_bench_active || s_open || s_nav_bench || orb_apps_active()) {
        orb_nav_admission_fail(ORB_NAV_ADMISSION_BUSY);
        lv_unlock();
        return false;
    }
    s_nav_bench = heap_caps_calloc(1, sizeof(*s_nav_bench), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_nav_bench) { orb_nav_admission_fail(ORB_NAV_ADMISSION_BUSY); lv_unlock(); return false; }
    strlcpy(s_nav_bench->gesture, gesture, sizeof(s_nav_bench->gesture));
    s_nav_bench->duration_ms = (uint32_t)ms;
    xSemaphoreTake(s_bench_done, 0);
    s_bench_active = true;
    bool queued = lv_async_call(navigation_benchmark_start, NULL) == LV_RESULT_OK;
    if (queued) orb_display_wake_service();
    if (!queued) { s_bench_active = false; navigation_failure(ORB_NAV_ADMISSION_BUSY); }
    lv_unlock();
    bool complete = queued && xSemaphoreTake(s_bench_done, pdMS_TO_TICKS(ms + 10000)) == pdTRUE;
    lv_lock();
    if (!complete && queued) {
        /* Cancel both owners under the UI lock before freeing the receipt. */
        lv_async_call_cancel(navigation_benchmark_start, NULL);
        navigation_failure(ORB_NAV_ADMISSION_BUSY);
        navigation_benchmark_finish();
    }
    const nav_bench_t *bench = s_nav_bench;
    bool ok = complete && !bench->failed && bench->after.frames > bench->before.frames &&
              bench->after.transfer_errors == bench->before.transfer_errors &&
              bench->after.failed_frames == bench->before.failed_frames;
    lv_unlock();
    if (complete && !bench->failed) {
        uint64_t frames = bench->after.frames - bench->before.frames;
        double count = frames ? frames : 1;
        double seconds = (bench->stopped_us - bench->started_us) / 1e6;
        double fps = seconds > 0 ? frames / seconds : 0;
        unsigned target = !strcmp(bench->gesture, "rotary") ? 55 : 58;
        bool written = command_reply_printf("\n@@ORBDEV NAV gesture=%s basis=final_dma seconds=%.3f frames=%llu fps=%.2f target_fps=%u meets_target=%d frame_ms=%.3f draw_ms=%.3f wait_ms=%.3f hold_ms=%.3f wire_ms=%.3f p95_frame_upper_ms=%.3f p95_cadence_upper_ms=%.3f accounting=%d bytes=%llu bands=%llu samples=%llu cold_us=%llu raster_builds=%lu panel_period_us=%lu errors=%llu failed_frames=%llu minimum_internal=%u largest_internal=%u\n",
            bench->gesture, seconds, (unsigned long long)frames, fps, target, fps >= target,
            (bench->after.frame_us - bench->before.frame_us) / count / 1000,
            (bench->after.draw_us - bench->before.draw_us) / count / 1000,
            (bench->after.wait_us - bench->before.wait_us) / count / 1000,
            (bench->after.hold_us - bench->before.hold_us) / count / 1000,
            (bench->after.send_us - bench->before.send_us) / count / 1000,
            navigation_p95_upper_ms(bench->before.frame_latency, bench->after.frame_latency,
                                     ORB_DISPLAY_LATENCY_BUCKETS, ORB_DISPLAY_LATENCY_BUCKET_US),
            navigation_p95_upper_ms(bench->before.completion_interval,
                                     bench->after.completion_interval,
                                     ORB_DISPLAY_INTERVAL_BUCKETS, 1000),
            orb_display_accounting_active(),
            (unsigned long long)(bench->after.bytes - bench->before.bytes),
            (unsigned long long)(bench->after.bands - bench->before.bands),
            (unsigned long long)bench->samples, (unsigned long long)bench->cold_us,
            (unsigned long)(bench->raster_after.builds - bench->raster_before.builds),
            (unsigned long)bench->panel_period_us,
            (unsigned long long)(bench->after.transfer_errors - bench->before.transfer_errors),
            (unsigned long long)(bench->after.failed_frames - bench->before.failed_frames),
            (unsigned)bench->minimum_internal, (unsigned)bench->largest_internal);
        written = benchmark_distribution_report("nav", gesture, &bench->before, &bench->after) && written;
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
        benchmark_task_report(gesture, &bench->profile_before, &bench->profile_after);
#endif
        const char *names[] = {"internal", "dma", "psram"};
        for (unsigned i = 0; i < 3; i++) {
            written = command_reply_printf("@@ORBDEV NAV_MEMORY gesture=%s kind=%s window=post_prepare_nav minimum_free=%u free=%u largest=%u\n",
                bench->gesture, names[i], (unsigned)bench->memory[i].minimum_free_bytes,
                (unsigned)bench->memory[i].total_free_bytes, (unsigned)bench->memory[i].largest_free_block) && written;
        }
        ok = written && ok;
    }
    lv_lock();
    heap_caps_free(s_nav_bench);
    s_nav_bench = NULL;
    lv_unlock();
    return ok;
}

static portMUX_TYPE s_tilt_lock = portMUX_INITIALIZER_UNLOCKED;
static orb_devtools_tilt_override_t s_tilt;

bool orb_devtools_motion_override(int64_t now_us, float *ax, float *ay, float *az)
{
    portENTER_CRITICAL(&s_tilt_lock);
    bool active = orb_devtools_tilt_sample(&s_tilt, now_us, ax, ay, az);
    portEXIT_CRITICAL(&s_tilt_lock);
    return active;
}

static portMUX_TYPE s_senses_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    orb_motion_sample_t sample;
    orb_senses_result_t result;
    uint64_t at_ms;
    unsigned taps;
    int detents;
} s_senses;

void orb_devtools_publish_senses(const orb_motion_sample_t *sample,
                                 const orb_senses_result_t *result, uint64_t now_ms)
{
    portENTER_CRITICAL(&s_senses_lock);
    s_senses.sample = *sample;
    s_senses.result = *result;
    s_senses.at_ms = now_ms;
    s_senses.taps += result->double_tap;
    s_senses.detents += result->detents;
    portEXIT_CRITICAL(&s_senses_lock);
}

static void print_senses(void) __attribute__((noinline));

static void print_senses(void)
{
    portENTER_CRITICAL(&s_senses_lock);
    orb_motion_sample_t m = s_senses.sample;
    orb_senses_result_t result = s_senses.result;
    uint64_t at = s_senses.at_ms;
    unsigned taps = s_senses.taps;
    int detents = s_senses.detents;
    portEXIT_CRITICAL(&s_senses_lock);
    command_reply_printf("@@ORBDEV SENSES device=%s at_ms=%llu accel_g=%.4f,%.4f,%.4f gyro_dps=%.3f,%.3f,%.3f resting=%d double_taps=%u twist_detents=%d hardware_double_tap=%d touching=%d\n",
        orb_net_device_id(), (unsigned long long)at, m.ax, m.ay, m.az, m.gx, m.gy, m.gz,
        result.resting, taps, detents, m.hardware_double_tap, orb_display_touch_pressed());
    orb_weather_snapshot_t weather;
    if (orb_model_weather_snapshot(&weather)) {
        command_reply_printf("@@ORBDEV WEATHER revision=%llu configured=%d available=%d place=\"%s\" label=\"%s\" unit=%s updated_at=%lu stale=%d hours=%u\n",
            (unsigned long long)weather.revision, weather.configured, weather.available,
            weather.place, weather.label, weather.unit == ORB_TEMPERATURE_F ? "F" : "C",
            (unsigned long)weather.updated_at, weather.stale, (unsigned)weather.hourly_count);
        if (weather.available) {
            command_reply_printf("@@ORBDEV WEATHER_CURRENT temperature=%.2f high=%.2f low=%.2f condition=%s is_day=%d\n",
                weather.temperature, weather.high, weather.low, orb_weather_condition_name(weather.condition), weather.is_day);
            for (size_t i = 0; i < weather.hourly_count; ++i) {
                const orb_weather_hour_t *hour = &weather.hourly[i];
                command_reply_printf("@@ORBDEV WEATHER_HOUR index=%u hour=%lu temperature=%.2f condition=%s is_day=%d\n",
                    (unsigned)i, (unsigned long)hour->hour, hour->temperature,
                    orb_weather_condition_name(hour->condition), hour->is_day);
            }
        }
    }
    orb_battery_detail_t b;
    if (orb_board_battery_detail(&b) != ESP_OK) {
        command_reply_printf("@@ORBDEV ERROR senses reason=battery_read\n");
        return;
    }
    command_reply_printf("@@ORBDEV BATTERY present=%d usb=%d pct=%d vbat_mv=%d vbus_mv=%d system_mv=%d charging=%d discharging=%d charge_setting_ma=%d charge_voltage_mv=%d charger_enabled=%d current_valid=%d charge_ma=%d discharge_ma=%d ts_enabled=%d ts_uv=%d ts_source_ua=%d ntc_ohms=%d temperature_valid=%d temperature_c=%.2f health_valid=%d health_pct=%d desk_enabled=%d desk_active=%d\n",
        b.present, b.usb, b.battery_pct, b.battery_mv, b.vbus_mv, b.system_mv,
        b.charging, b.discharging, b.charge_current_setting_ma, b.charge_voltage_setting_mv,
        b.charger_enabled, b.current_valid, b.charge_current_ma, b.discharge_current_ma,
        b.ts_enabled, b.ts_uv, b.ts_source_ua, b.ntc_ohms, b.temperature_valid,
        b.temperature_c, b.health_valid, b.health_pct, b.desk_limit_enabled, b.desk_limit_active);
}

static bool record_microphones(void) __attribute__((noinline));

static bool record_microphones(void)
{
    const size_t bytes = 16000u * 5u * 2u * sizeof(int16_t);
    uint8_t *pcm = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) return false;
    esp_codec_dev_handle_t speaker = NULL, mic = NULL;
    orb_voice_speech_stop(ORB_SPEECH_STOP);
    bool leased = orb_sound_audio_acquire(), ok = false;
    if (!leased) goto finished;
    if (orb_board_audio_new(16000, &speaker, &mic) != ESP_OK) goto finished;
    esp_codec_dev_sample_info_t format = {.sample_rate = 16000, .bits_per_sample = 16,
        .channel = 2, .channel_mask = 3};
    if (esp_codec_dev_open(mic, &format) != ESP_CODEC_DEV_OK ||
        orb_board_audio_set_mic_gain() != ESP_OK) goto finished;
    size_t offset = 0;
    int64_t started = esp_timer_get_time(), deadline = started + 7000000;
    while (offset < bytes && esp_timer_get_time() < deadline) {
        size_t got = 0, wanted = bytes - offset < 2560 ? bytes - offset : 2560;
        esp_err_t err = orb_board_audio_read(mic, pcm + offset, wanted, &got, 100);
        if (got > wanted || (got & 3u) || (err != ESP_OK && !got)) goto finished;
        offset += got;
    }
    ok = offset == bytes;
finished:
    if (speaker || mic) orb_board_audio_delete(speaker, mic);
    if (leased) orb_sound_audio_release();
    if (ok) {
        uint32_t crc = esp_rom_crc32_le(0, pcm, bytes);
        ok = command_reply_printf("@@ORBDEV MIC rate=16000 channels=2 frames=80000 bytes=%u crc32=%08lx\n",
                                   (unsigned)bytes, (unsigned long)crc);
        char encoded[LINE_BYTES / 3 * 4 + 1];
        unsigned sequence = 0;
        for (size_t off = 0; ok && off < bytes; off += LINE_BYTES) {
            size_t count = bytes - off < LINE_BYTES ? bytes - off : LINE_BYTES;
            base64(pcm + off, count, encoded);
            ok = command_reply_printf("@@ORBDEV MIC_CHUNK seq=%u data=%s\n", sequence++, encoded);
        }
        if (ok) ok = command_reply_printf("@@ORBDEV MIC_END chunks=%u bytes=%u crc32=%08lx\n",
                                           sequence, (unsigned)bytes, (unsigned long)crc);
    }
    heap_caps_free(pcm);
    return ok;
}

/* Keep report-only histograms off every command's call chain. Inlining
 * this branch reserved 1648 bytes even for a one-word wake command. */
static void __attribute__((noinline)) print_heap_receipt(void)
{
    orb_devtools_heap_report(devtools_printf, command_reply_printf);
    navigation_receipt(false);
    orb_mem_report_to(s_wifi.active ? s_wifi.file : stdout);
    orb_devtools_heap_detail(devtools_printf);
}

static bool command_tilt(const char *cmd)
{
    orb_devtools_tilt_t tilt;
    if (!orb_devtools_tilt_command(cmd, &tilt)) {
        command_reply_printf("@@ORBDEV ERROR tilt reason=invalid_arguments; X/Y finite -1..1g, X*X+Y*Y<=1, MS 1..5000\n");
        return false;
    }
    if (s_bench_active || !wake_display()) {
        command_reply_printf("@@ORBDEV ERROR tilt reason=busy_or_wake_rejected\n");
        return false;
    }
    portENTER_CRITICAL(&s_tilt_lock);
    orb_devtools_tilt_begin(&s_tilt, &tilt, esp_timer_get_time());
    portEXIT_CRITICAL(&s_tilt_lock);
    return true;
}

static void command_sound_status(void)
{
    orb_sound_preferences_t prefs;
    orb_sound_preferences(&prefs);
    orb_sound_stats_t stats;
    orb_sound_stats(&stats);
    command_reply_printf("@@ORBDEV SOUND volume=%u saving=%d save_error=%d palette_cues=%u rate=32000 ready=%d silent=%d face_down=%d always_on=%d asset_bytes=%lu queued=%lu started=%lu dropped=%lu last_submit_us=%lu max_submit_us=%lu\n",
        prefs.volume, prefs.saving, prefs.save_error, ORB_SOUND_CUE_COUNT, stats.ready, stats.silent, stats.face_down, stats.always_on,
        (unsigned long)stats.asset_bytes, (unsigned long)stats.queued_count, (unsigned long)stats.started_count,
        (unsigned long)stats.dropped_count, (unsigned long)stats.last_start_us, (unsigned long)stats.max_start_us);
}

static bool command_goto(const char *cmd)
{
    /* Lead review: jump to a surface directly instead of steering there
     * with injected presses. Navigation only; nothing is answered. */
    static const struct { const char *name; orb_shell_surface_t surface; } targets[] = {
        {"face", ORB_SHELL_FACE}, {"stack", ORB_SHELL_STACK}, {"apps", ORB_SHELL_APPS},
        {"agents", ORB_SHELL_AGENTS}, {"sessions", ORB_SHELL_SESSIONS}, {"usage", ORB_SHELL_USAGE},
        {"dot", ORB_SHELL_DOT}, {"home", ORB_SHELL_HOME}, {"time", ORB_SHELL_TIME},
        {"play", ORB_SHELL_PLAY}, {"notifications", ORB_SHELL_NOTIFICATIONS},
        {"control", ORB_SHELL_CONTROL_CENTER}, {"settings", ORB_SHELL_SETTINGS},
        {"sound", ORB_SHELL_SOUND}, {"battery", ORB_SHELL_BATTERY}, {"updates", ORB_SHELL_UPDATES},
        {"onboarding", ORB_SHELL_ONBOARDING}, {"weather", ORB_SHELL_WEATHER},
    };
    int found = -1;
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
        if (!strcmp(cmd + 5, targets[i].name)) found = (int)i;
    if (found < 0 || !s_shell) {
        command_reply_printf("@@ORBDEV ERROR goto reason=unknown_surface\n");
        return false;
    }
    bool navigated = request_wake(true, targets[found].surface);
    if (!navigated) {
        command_reply_printf("@@ORBDEV ERROR goto reason=wake_rejected\n");
        return false;
    }
    return true;
}

static bool command_buttons(const char *cmd)
{
    portENTER_CRITICAL(&s_buttons_lock);
    orb_devtools_buttons_snapshot_fn snapshot = s_buttons_snapshot;
    orb_devtools_buttons_inject_fn inject = s_buttons_inject;
    void *ctx = s_buttons_ctx;
    portEXIT_CRITICAL(&s_buttons_lock);
    if (!snapshot || !inject) {
        command_reply_printf("\n@@ORBDEV ERROR buttons reason=not_ready\n");
        return false;
    }
    if (!strcmp(cmd, "buttons")) {
        orb_button_edge_t edges[ORB_BUTTON_HISTORY];
        size_t count = snapshot(edges, ctx);
        if (count > ORB_BUTTON_HISTORY) count = ORB_BUTTON_HISTORY;
        command_reply_printf("@@ORBDEV BUTTONS count=%u\n", (unsigned)count);
        for (size_t i = 0; i < count; i++) {
            const orb_button_edge_t *e = &edges[i];
            command_reply_printf("@@ORBDEV BUTTON button=%s edge=%s phase=%s at_ms=%llu synthetic=%d\n",
                orb_button_name(e->button), e->held ? "down" : "up",
                e->debounced ? "debounced" : "raw", (unsigned long long)e->at_ms, e->synthetic);
        }
    } else {
        orb_button_t button;
        orb_button_gesture_t gesture;
        if (!orb_devtools_button_command(cmd, &button, &gesture)) {
            command_reply_printf("\n@@ORBDEV ERROR press reason=invalid_arguments; pwr|boot short|double|hold\n");
            return false;
        }
        if (s_bench_active || !inject(button, gesture, ctx)) {
            command_reply_printf("\n@@ORBDEV ERROR press reason=busy\n");
            return false;
        }
        command_reply_printf("@@ORBDEV BUTTON_INJECT button=%s gesture=%s synthetic=1\n",
            orb_button_name(button), orb_button_gesture_name(gesture));
    }
    return true;
}

static bool command_benchmark(const char *cmd)
{
    int a[1];
    int end = 0;
    const char *argument = cmd + 5;
    int fixture = BENCH_ORIGINAL;
    if (strncmp(cmd, "bench_transition ", 17) == 0) {
        argument = cmd + 16;
        fixture = BENCH_TRANSITION;
    } else if (strncmp(cmd, "bench_quiet ", 12) == 0) {
        argument = cmd + 11;
        fixture = BENCH_QUIET;
    }
    if (sscanf(argument, " %d%n", &a[0], &end) != 1 || argument[end] != '\0' ||
        a[0] < 1000 || a[0] > 60000 || !benchmark(a[0], fixture, ORB_SCROLL_AGENTS)) {
        printf("\n@@ORBDEV ERROR bench: duration 1000..60000 ms; scene must finish\n");
        return false;
    }
    return true;
}

/* Display sleep for power tests, on the UI task: a power transition overflows
 * the command stack. */
static void sleep_on_ui(void *context) { (void)context; if (s_shell) orb_shell_power_sleep(s_shell); }

static bool run_sound(const char *cmd)
{
    int a;
    if (!strcmp(cmd, "soundstop")) {
        orb_sound_stop_all();
    } else if (!strcmp(cmd, "soundstatus")) {
        command_sound_status();
    } else if (sscanf(cmd, "soundplay %d", &a) == 1) {
        if (a < 0 || a >= ORB_SOUND_CUE_COUNT || !orb_sound_play((orb_sound_cue_t)a)) {
            command_reply_printf("@@ORBDEV ERROR soundplay reason=unavailable_or_suppressed\n");
            return false;
        }
    } else if (sscanf(cmd, "soundvolume %d", &a) == 1 && a >= 0 && a <= 100) {
        orb_sound_set_volume((unsigned)a);
    } else if (sscanf(cmd, "soundsilent %d", &a) == 1 && a >= 0 && a <= 1) {
        /* Through the shell, which owns Silent and re-applies it every tick. */
        lv_lock();
        if (s_shell) orb_shell_set_silent(s_shell, a != 0);
        else orb_sound_set_silent(a != 0);
        lv_unlock();
    } else if (sscanf(cmd, "soundalwayson %d", &a) == 1 && a >= 0 && a <= 1) {
        orb_sound_set_always_on(a != 0);
    } else {
        command_reply_printf("@@ORBDEV ERROR unknown\n");
        return false;
    }
    return true;
}

static void run_ui(char *cmd)
{
    int a[5];
    if (!strncmp(cmd, "tilt", 4)) {
        if (!command_tilt(cmd)) return;
    } else if (!strcmp(cmd, "senses")) {
        print_senses();
    } else if (!strcmp(cmd, "micrecord")) {
        if (!record_microphones()) {
            command_reply_printf("@@ORBDEV ERROR micrecord reason=busy_or_capture_failed\n");
            return;
        }
    } else if (!strncmp(cmd, "sound", 5)) {
        if (!run_sound(cmd)) return;
    } else if (strcmp(cmd, "shot") == 0) {
        send_shot();
    } else if (strncmp(cmd, "goto ", 5) == 0) {
        if (!command_goto(cmd)) return;
    } else if (strcmp(cmd, "sleep") == 0) {
        lv_lock(); lv_async_call(sleep_on_ui, NULL); lv_unlock(); orb_display_wake_service();
    } else if (strcmp(cmd, "wake") == 0) {
        if (!wake_display()) {
            command_reply_printf("@@ORBDEV ERROR wake reason=rejected\n");
            return;
        }
    } else if (sscanf(cmd, "tap %d %d", &a[0], &a[1]) == 2) {
        glide(a[0], a[1], a[0], a[1], 80);
        set_touch(a[0], a[1], false);
    } else if (sscanf(cmd, "hold %d %d %d", &a[0], &a[1], &a[2]) == 3) {
        glide(a[0], a[1], a[0], a[1], a[2]);
        set_touch(a[0], a[1], false);
    } else if (sscanf(cmd, "swipe %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4]) == 5) {
        glide(a[0], a[1], a[2], a[3], a[4]);
        set_touch(a[2], a[3], false);
    } else if (strncmp(cmd, "arc", 3) == 0) {
        orb_devtools_arc_t arc;
        if (!orb_devtools_arc_command(cmd, &arc)) {
            command_reply_printf("@@ORBDEV ERROR arc reason=invalid_arguments; R 1..232, A0/A1 -3600..3600, MS 1..5000\n");
            return;
        }
        glide_arc(&arc);
    } else if (strcmp(cmd, "top") == 0) {
        orb_devtools_top(devtools_printf);
    } else if (strcmp(cmd, "crash") == 0) {
        orb_devtools_crash_report(devtools_printf);
    } else if (strcmp(cmd, "usbreattach") == 0) {
        orb_board_usb_reattach();
    } else if (strcmp(cmd, "power") == 0) {
        print_power();
    } else if (strcmp(cmd, "buttons") == 0 || strncmp(cmd, "press ", 6) == 0) {
        if (!command_buttons(cmd)) return;
    } else if (strcmp(cmd, "heap") == 0) {
        print_heap_receipt();
    } else if (strncmp(cmd, "bench_nav ", 10) == 0) {
        char gesture[16];
        int end = 0;
        /* %s, not a [a-z-] scanset: newlib drops the trailing '-', rejecting
         * every hyphenated name. The whitelist below does the validation. */
        if (sscanf(cmd, "bench_nav %15s %d%n", gesture, &a[0], &end) != 2 ||
            cmd[end] != '\0' || a[0] < 1000 || a[0] > 60000 || !navigation_gesture_valid(gesture)) {
            command_reply_printf("\n@@ORBDEV ERROR bench_nav reason=invalid_arguments; slide-left|slide-right|stack-up|shade-down|back|iris|rotary, 1000..60000 ms\n");
            return;
        }
        if (!benchmark_navigation(gesture, a[0])) {
            navigation_receipt(true);
            return;
        }
    } else if (strncmp(cmd, "bench_game ", 11) == 0) {
        char id[25];
        int end = 0;
        if (sscanf(cmd, "bench_game %24s %d%n", id, &a[0], &end) != 2 ||
            cmd[end] != '\0' || a[0] < 1000 || a[0] > 60000 || !benchmark_game(id, a[0])) {
            printf("\n@@ORBDEV ERROR bench_game: marble|beads|orbit|ring-snake|pop, 1000..60000 ms; exit active app first\n");
            return;
        }
    } else if (strncmp(cmd, "bench_scroll ", 13) == 0) {
        char name[25]; int end = 0; orb_scroll_surface_t surface;
        if (sscanf(cmd, "bench_scroll %24[a-z_-] %d%n", name, &a[0], &end) != 2 ||
            cmd[end] != '\0' || !orb_scroll_surface_parse(name, &surface) ||
            a[0] < 1000 || a[0] > 60000 || !benchmark(a[0], BENCH_SCROLL, surface)) {
            printf("\n@@ORBDEV ERROR bench_scroll: agents|rooms|settings|notifications|favourites, 1000..60000 ms\n");
            return;
        }
    } else if (strncmp(cmd, "bench_open ", 11) == 0) {
        if (!benchmark_open(cmd + 11)) {
            return;
        }
    } else if (strncmp(cmd, "bench", 5) == 0) {
        if (!command_benchmark(cmd)) return;
    } else if (strncmp(cmd, "profile", 7) == 0) {
        int end = 0;
        if (sscanf(cmd, "profile %d%n", &a[0], &end) != 1 || cmd[end] != '\0' || a[0] < 1 || a[0] > 5000) {
            printf("\n@@ORBDEV ERROR profile: duration must be 1..5000 ms\n");
            return;
        }
#if LV_USE_PROFILER_BUILTIN
        orb_display_profile(a[0]);
#else
        printf("\n@@ORBDEV ERROR profile disabled: enable CONFIG_LV_USE_PROFILER_BUILTIN\n");
        return;
#endif
    } else {
        printf("\n@@ORBDEV ? %s\n", cmd);
        return;
    }
    command_reply_printf("\n@@ORBDEV OK %s\n", cmd);
}

static void run(char *cmd)
{
    if (!strncmp(cmd, "lightsleep", 10)) {
        if (orb_devtools_lightsleep_command(cmd, command_reply_printf))
            command_reply_printf("\n@@ORBDEV OK %s\n", cmd);
        return;
    }
    run_ui(cmd);
}

static void devtools_task(void *arg)
{
    s_captured = xSemaphoreCreateBinary();
    s_wake_done = xSemaphoreCreateBinary();
    s_output_lock = xSemaphoreCreateMutex();
    s_output_done = xSemaphoreCreateBinary();
    s_executor = xSemaphoreCreateBinary();
    if (!s_captured || !s_wake_done || !s_output_lock || !s_output_done || !s_executor) vTaskDelete(NULL);
    s_wifi.file = funopen(NULL, NULL, wifi_file_write, NULL, NULL);
    if (!s_wifi.file) vTaskDelete(NULL);
    setvbuf(s_wifi.file, NULL, _IONBF, 0);
    static const char *const methods[] = {
        "devtools.shot", "devtools.tap", "devtools.hold", "devtools.swipe", "devtools.arc", "devtools.tilt",
        "devtools.bench", "devtools.bench_open", "devtools.bench_scroll",
        "devtools.bench_game", "devtools.bench_nav", "devtools.heap", "devtools.top", "devtools.power", "devtools.lightsleep",
        "devtools.buttons", "devtools.press", "devtools.wake", "devtools.sleep", "devtools.crash", "devtools.usbreattach", "devtools.goto",
        "devtools.soundplay", "devtools.soundvolume", "devtools.soundsilent", "devtools.soundalwayson",
        "devtools.soundstop", "devtools.soundstatus", "devtools.senses", "devtools.micrecord",
    };
    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (orb_link_on_method(methods[i], wifi_request, (void *)methods[i]) != ESP_OK) {
            ESP_LOGE(TAG, "no link handler slot for %s; Wi-Fi devtools disabled", methods[i]);
            vTaskDelete(NULL);
        }
    }
    if (orb_link_on_hello(wifi_hello, NULL) != ESP_OK) vTaskDelete(NULL);
    while (!lv_is_initialized() || !lv_display_get_default()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    lv_lock();
    lv_indev_t *indev = lv_indev_create();
    /* USB and Wi-Fi share this pointer; provenance survives all LVGL events. */
    orb_input_mark_synthetic(indev);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_display(indev, lv_display_get_default());
    lv_unlock();
    ESP_LOGI(TAG, "ready: shot, tap, hold, swipe, arc, tilt, top, heap, power, buttons, press, profile, bench, bench_game, bench_nav");

    xSemaphoreGive(s_executor);
    s_command_task = xTaskGetCurrentTaskHandle();
    char cmd[96];
    size_t n = 0;
    for (;;) {
        char wifi_command[ORB_DEVTOOLS_COMMAND_BYTES];
        portENTER_CRITICAL(&s_wifi_lock);
        bool pending = s_wifi.pending;
        uint32_t stream = s_wifi.stream;
        if (pending) memcpy(wifi_command, s_wifi.command, sizeof(wifi_command));
        s_wifi.pending = false;
        portEXIT_CRITICAL(&s_wifi_lock);
        if (pending) {
            s_wifi.seq = 0; s_wifi.bytes = 0;
            s_wifi.failed = false; s_wifi.acknowledged = false;
            if (wifi_current()) {
                s_wifi.active = true;
                run(wifi_command);
                cJSON *done = cJSON_CreateObject();
                bool success = !s_wifi.failed && s_wifi.acknowledged && wifi_current();
                bool fields = done && cJSON_AddNumberToObject(done, "stream", stream) &&
                              cJSON_AddBoolToObject(done, "ok", success) &&
                              (success || cJSON_AddStringToObject(done, "code", "internal"));
                /* Failed execution still sends a terminal failure if this link
                 * is alive; a failed output transport never becomes success. */
                if (fields) wifi_event("devtools.done", done);
                else cJSON_Delete(done);
                s_wifi.active = false;
            }
            xSemaphoreGive(s_executor);
        }
        uint8_t c;
        while (usb_serial_jtag_ll_rxfifo_data_available() && usb_serial_jtag_ll_read_rxfifo(&c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                if (n) {
                    cmd[n] = 0;
                    if (xSemaphoreTake(s_executor, 0) == pdTRUE) {
                        run(cmd);
                        xSemaphoreGive(s_executor);
                    } else printf("\n@@ORBDEV ERROR busy\n");
                    n = 0;
                }
            } else if (n < sizeof(cmd) - 1) {
                cmd[n++] = (char)c;
            }
        }
        orb_light_sleep_status_t sleep;
        orb_light_sleep_status(&sleep);
        /* Wi-Fi commands notify immediately; only the development USB console
         * needs this fallback poll while the device is eligible for Sleep. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(sleep.eligible ? 5000 : 20));
    }
}

ESP_SYSTEM_INIT_FN(orb_devtools_init, SECONDARY, BIT(0), 990)
{
    /* Created now, runs once the scheduler starts; low priority, on the
     * networking core so the UI core isn't disturbed between commands. */
    xTaskCreatePinnedToCore(devtools_task, "devtools", 5120, NULL, 2, NULL, 0);
    return ESP_OK;
}
