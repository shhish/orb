"""Compile the production compact emitter against a throttled USB FIFO."""

import base64
import os
import struct
import subprocess
import zlib
from pathlib import Path

import pytest

ROOT = Path(__file__).parents[1]
COMPONENT = ROOT / "firmware/components/orb_devtools"
DISPLAY = ROOT / "firmware/components/orb_display/include"


@pytest.fixture(scope="module")
def emitter(tmp_path_factory):
    directory = tmp_path_factory.mktemp("scroll-emitter")
    source = (COMPONENT / "orb_devtools.c").read_text()
    reply = source.split("static bool command_reply_vprintf", 1)[1].split(
        "\nstatic portMUX_TYPE s_lock", 1
    )[0]
    codec = source.split("static size_t base64(", 1)[1].split("\n/* Blends the top layer", 1)[0]
    report = source.split("#if CONFIG_ORB_DISPLAY_SCROLL_TRACE\n/* Fixed wire", 1)[1].split(
        "\n/* End scroll trace output. */", 1
    )[0]
    prefix = r"""
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_console_reply.h"
#include "orb_scroll_trace_wire.h"
#define CONFIG_ORB_DISPLAY_SCROLL_TRACE 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static int64_t s_now = 1000000, s_ready;
static unsigned s_writes, s_fail_after = UINT_MAX;
static uint8_t s_wire[100000];
static size_t s_wire_size;
static bool s_bad_timestamp, s_fail_read, s_fail_allocation;
static uint32_t s_count;
/* This fixture exercises serial backpressure. Keep the optional Wi-Fi route
 * inactive and fail if production accidentally sends a serial receipt there. */
static struct { bool active; } s_wifi;
static bool wifi_text_until(const char *text, size_t size, int64_t deadline_us) {
    (void)text; (void)size; (void)deadline_us;
    assert(!"serial fixture entered Wi-Fi reply path");
    return false;
}
static size_t reply_write(void *ctx, const uint8_t *data, size_t size) {
    (void)ctx;
    if (s_now < s_ready || s_writes >= s_fail_after) return 0;
    if (size > 64) size = 64;
    assert(size <= sizeof(s_wire) - s_wire_size);
    memcpy(s_wire + s_wire_size, data, size);
    s_wire_size += size; s_writes++; s_ready = s_now + 10000;
    return size;
}
static bool reply_writable(void *ctx) { (void)ctx; return s_now >= s_ready && s_writes < s_fail_after; }
static void reply_flush(void *ctx) { (void)ctx; }
static int64_t reply_now(void *ctx) { (void)ctx; return s_now; }
static void reply_yield(void *ctx) { (void)ctx; s_now += 10000; }
static int64_t esp_timer_get_time(void) { return s_now; }
static void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return s_fail_allocation ? NULL : malloc(size); }
static void heap_caps_free(void *ptr) { free(ptr); }
static uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data, uint32_t size) {
    crc = ~crc;
    for (uint32_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
    }
    return ~crc;
}
static void orb_display_scroll_trace_snapshot(orb_display_scroll_trace_t *out) {
    memset(out, 0, sizeof(*out));
    out->available = true; out->started_us = 100; out->ended_us = 3000000;
    out->count = s_count; out->dropped = 17;
    out->completions = (s_count + 4) / 16;
    out->intervals[15] = out->completions ? out->completions - 1 : 0;
}
static bool orb_display_scroll_trace_read(unsigned index, orb_display_scroll_trace_event_t *out) {
    if (s_fail_read || index >= s_count) return false;
    *out = (orb_display_scroll_trace_event_t){
        .at_us = s_bad_timestamp ? 99 : 100 + (int64_t)index * 997,
        .a = -(int32_t)index - 1, .b = (int32_t)index * 17,
        .kind = (uint8_t)(index % 16 + 1),
    };
    return true;
}
"""
    # Use the actual writer, encoder and report: corruption/failure assertions
    # exercise production byte ordering and the single whole-emission deadline.
    fixture = prefix.replace(
        "static void orb_display_scroll_trace_snapshot", "void orb_display_scroll_trace_snapshot"
    ).replace("static bool orb_display_scroll_trace_read", "bool orb_display_scroll_trace_read")
    fixture += "static bool command_reply_vprintf" + reply
    fixture += "static size_t base64(" + codec
    fixture += "#if CONFIG_ORB_DISPLAY_SCROLL_TRACE\n/* Fixed wire" + report
    fixture += r"""
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "max")) s_count = ORB_SCROLL_TRACE_CAPACITY;
    else if (!strcmp(argv[1], "single")) s_count = 1;
    else if (!strcmp(argv[1], "empty")) s_count = 0;
    else if (!strcmp(argv[1], "stall")) { s_count = ORB_SCROLL_TRACE_CAPACITY; s_fail_after = 40; }
    else if (!strcmp(argv[1], "bad-time")) { s_count = 1; s_bad_timestamp = true; }
    else if (!strcmp(argv[1], "bad-read")) { s_count = 1; s_fail_read = true; }
    else if (!strcmp(argv[1], "allocation")) { s_count = 1; s_fail_allocation = true; }
    else if (!strcmp(argv[1], "overflow-time")) {
        orb_display_scroll_trace_event_t event = {.at_us = (int64_t)UINT32_MAX + 101};
        uint8_t raw[ORB_SCROLL_TRACE_RECORD_BYTES];
        assert(!orb_scroll_trace_pack(&event, 100, raw));
        event.at_us = (int64_t)UINT32_MAX + 100;
        assert(orb_scroll_trace_pack(&event, 100, raw));
        assert(raw[0] == 255 && raw[1] == 255 && raw[2] == 255 && raw[3] == 255);
        return 0;
    } else return 2;
    bool ok = scroll_trace_report();
    fprintf(stderr, "ok=%d elapsed=%lld writes=%u\n", ok, (long long)(s_now - 1000000), s_writes);
    assert(s_now - 1000000 <= ORB_SCROLL_TRACE_EMIT_TIMEOUT_US);
    fwrite(s_wire, 1, s_wire_size, stdout);
    return ok ? 0 : 3;
}
"""
    path = directory / "emitter.c"
    path.write_text(fixture)
    binary = directory / "emitter"
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-function",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            f"-I{COMPONENT}",
            f"-I{DISPLAY}",
            str(path),
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return binary


def fields(line):
    return dict(word.split("=", 1) for word in line.split()[2:])


@pytest.mark.parametrize("case,count", [("empty", 0), ("single", 1), ("max", 2048)])
def test_production_trace_emitter_crc_sequence_and_bounds(emitter, case, count):
    result = subprocess.run([str(emitter), case], check=True, capture_output=True, text=True)
    lines = result.stdout.splitlines()
    header = fields(lines[0])
    assert header["version"] == "2" and header["encoding"] == "le13"
    assert header["count"] == str(count)
    assert header["dropped"] == "17"
    assert header["record_bytes"] == "13"
    assert len(lines[1].split("counts=", 1)[1].split(",")) == 128
    assert int(header["chunks"]) == (count * 13 + 191) // 192
    raw = bytearray()
    for seq, line in enumerate(lines[2:-1]):
        assert line.startswith("@@ORBDEV SCROLLTRACE_CHUNK ")
        chunk = fields(line)
        payload = base64.b64decode(chunk["data"], validate=True)
        assert int(chunk["seq"]) == seq
        assert len(payload) == int(chunk["bytes"]) <= 192
        assert f"{zlib.crc32(payload):08x}" == chunk["crc32"]
        raw.extend(payload)
    assert len(raw) == int(header["bytes"]) == count * 13
    assert f"{zlib.crc32(raw):08x}" == header["crc32"]
    assert list(struct.iter_unpack("<IiiB", raw)) == [
        (index * 997, -index - 1, index * 17, index % 16 + 1) for index in range(count)
    ]
    assert lines[-1].startswith("@@ORBDEV SCROLLTRACE_END ")
    trailer = fields(lines[-1])
    for key in ("chunks", "bytes", "crc32"):
        assert trailer[key] == header[key]
    assert int(trailer["emit_us"]) < 10_000_000
    # Model the real writer's 64-byte packets and 10 ms FreeRTOS fallback
    # yields: full capture still fits the global budget with this backpressure.
    if count == 2048:
        assert int(trailer["emit_us"]) < 9_000_000
        assert len(result.stdout) < 50_000


@pytest.mark.parametrize("case", ["stall", "bad-time", "bad-read", "allocation"])
def test_production_trace_failure_has_no_success_trailer(emitter, case):
    result = subprocess.run([str(emitter), case], capture_output=True, text=True)
    assert result.returncode == 3
    assert "ok=0" in result.stderr
    assert "@@ORBDEV SCROLLTRACE_END " not in result.stdout
    if case == "stall":
        assert "elapsed=10000000" in result.stderr
        assert "@@ORBDEV SCROLLTRACE_CHUNK " in result.stdout
    else:
        assert not result.stdout


def test_production_trace_timestamp_never_wraps(emitter):
    subprocess.run([str(emitter), "overflow-time"], check=True, capture_output=True, text=True)
