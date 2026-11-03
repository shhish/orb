"""Production panel state machine with virtual SPI completion and deadlines."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    component = Path(__file__).resolve().parents[2]
    source = (component / "board_panel_power.c").read_text()
    source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
    fixture = (
        r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include "orb_panel_power.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
static int64_t s_now;
static unsigned s_commands;
static uint8_t s_log[32], s_fail;
static int64_t s_times[32];
static int64_t esp_timer_get_time(void) { return s_now; }
static bool s_dma;
static bool s_bus_held = true, s_fail_release;
static unsigned s_releases;
"""
        + source
        + r"""
esp_err_t orb_board_panel_command(uint8_t command) {
    assert(!s_dma && s_commands < 32);
    s_bus_held = true;
    s_log[s_commands] = command; s_times[s_commands++] = s_now;
    if (command == s_fail) { s_fail = 0; return -1; }
    return ESP_OK;
}
esp_err_t orb_board_panel_bus_release(void) {
    assert(!s_dma);
    if (s_fail_release) { s_fail_release = false; return -1; }
    if (s_bus_held) { s_bus_held = false; s_releases++; }
    return ESP_OK;
}
static void reset(void) {
    s_panel = (orb_panel_power_t){ .requested = true, .display_on = true };
    s_ready_awake = true; s_ready_asleep = false;
    s_now = s_retry_us = s_commands = s_fail = 0;
    s_bus_held = true; s_fail_release = false; s_releases = 0;
}
static void at(int64_t now) { s_now = now; orb_board_panel_service(); }
static void check_spacing(void) {
    int64_t previous = -120000;
    for (unsigned i = 0; i < s_commands; i++) {
        if (s_log[i] == 0x10 || s_log[i] == 0x11) {
            assert(s_times[i] - previous >= 120000); previous = s_times[i];
        }
    }
}
int main(void) {
    reset();
    assert(orb_board_panel_ready(true));
    assert(orb_board_panel_awake(false) == ESP_OK);
    assert(s_commands == 2 && s_log[0] == 0x28 && s_log[1] == 0x10);
    assert(orb_board_panel_ready(false) && !orb_board_panel_ready(true));
    assert(!s_bus_held && s_releases == 1);
    for (unsigned i = 0; i < 10; i++) orb_board_panel_awake(false);
    assert(s_commands == 2);
    assert(!s_bus_held && s_releases == 1);
    s_now = 1000; orb_board_panel_awake(true);
    assert(!orb_board_panel_ready(true) && orb_board_panel_wait_ms() == 119);
    at(119999); assert(s_commands == 2);
    at(120000); assert(s_commands == 3 && s_log[2] == 0x11 && !orb_board_panel_ready(true));
    assert(s_bus_held);
    at(239999); assert(s_commands == 3);
    at(240000); assert(s_commands == 4 && s_log[3] == 0x29 && orb_board_panel_ready(true));
    for (unsigned i = 0; i < 10; i++) orb_board_panel_awake(true);
    assert(s_commands == 4 && orb_board_panel_wait_ms() == UINT32_MAX);
    check_spacing();
    reset(); s_fail_release = true;
    orb_board_panel_awake(false);
    assert(!orb_board_panel_ready(false) && s_bus_held);
    assert(orb_board_panel_wait_ms() == 50);
    at(49999); assert(s_bus_held);
    at(50000); assert(orb_board_panel_ready(false) && !s_bus_held && s_releases == 1);
    reset(); orb_board_panel_awake(false); at(120000); orb_board_panel_awake(true);
    assert(s_log[2] == 0x11);
    s_now = 130000; orb_board_panel_awake(false);
    at(239999); assert(s_commands == 3 && !orb_board_panel_ready(false));
    at(240000); assert(s_commands == 4 && s_log[3] == 0x10 && orb_board_panel_ready(false));
    /* Latest wake wins even while the reversed sleep is itself pending. */
    s_now = 241000; orb_board_panel_awake(true);
    at(360000); assert(s_log[4] == 0x11);
    at(480000); assert(s_log[5] == 0x29 && orb_board_panel_ready(true));
    check_spacing();
    uint8_t failures[] = {0x28, 0x10, 0x11, 0x29};
    for (unsigned i = 0; i < 4; i++) {
        reset();
        if (i < 2) s_fail = failures[i];
        orb_board_panel_awake(false);
        if (i < 2) assert(!orb_board_panel_ready(false));
        at(50000); at(120000); at(240000);
        assert(orb_board_panel_ready(false));
        if (i >= 2) s_fail = failures[i];
        orb_board_panel_awake(true);
        for (int64_t t = 250000; t <= 800000; t += 10000) at(t);
        assert(orb_board_panel_ready(true));
        check_spacing();
    }
    puts("panel sleep: DCS order, 120ms guards, reversal, retry, bus release and DMA ownership passed");
}
"""
    )
    with tempfile.TemporaryDirectory(prefix="orb-panel-power-") as temporary:
        path = Path(temporary) / "power.c"
        path.write_text(fixture)
        binary = path.with_suffix("")
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                f"-I{component}",
                str(path),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
