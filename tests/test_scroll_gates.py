"""Exercise production acceptance comparisons at rounding and worst-frame boundaries."""

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).parents[1]


def test_production_scroll_gates(tmp_path):
    source = tmp_path / "gates.c"
    source.write_text(
        r"""
#include <assert.h>
#include "orb_scroll_gates.h"
int main(void) {
    orb_display_cadence_t sample = {.loops = 500, .animation_ticks = 300,
        .handler_max_us = 33000, .animation_gap_max_us = 33000,
        .invalidated_pixels = 48000000, .damage_frames = 300, .damage_pixels = 30000000, .damage_max_pixels = 132000};
    orb_scroll_gates_t gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(gate.target_fps == 50 && gate.fps_pass && gate.stall_pass && gate.invalidation_pass);
    gate = orb_scroll_gates(true, 400, 325, 6500001, 120000, &sample);
    assert(!gate.fps_pass); /* The formatted FPS would still read 50.00. */
    gate = orb_scroll_gates(false, 400, 195, 6500000, 120000, &sample);
    assert(gate.target_fps == 30 && gate.fps_pass);
    sample.damage_max_pixels++;
    gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(!gate.invalidation_pass); /* Mean remains below the viewport. */
    sample.damage_max_pixels--;
    sample.invalidated_pixels = 52800000;
    gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(gate.request_invalidation_pass && gate.damage_pass && gate.invalidation_pass);
    sample.invalidated_pixels++;
    gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(!gate.request_invalidation_pass && gate.damage_pass && !gate.invalidation_pass);
    sample.invalidated_pixels = 48000000;
    sample.handler_max_us++;
    gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(!gate.stall_pass);
    sample.handler_max_us--;
    sample.animation_gap_max_us++;
    gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(!gate.stall_pass);
    sample.animation_gap_max_us--;
    sample.animation_stalls = 1;
    gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(!gate.stall_pass);
    sample.animation_stalls = 0;
    sample.handler_stalls = 1;
    gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(!gate.stall_pass);
    sample.handler_stalls = 0;
    sample.animation_ticks = 0;
    sample.animation_gap_max_us = 0;
    gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(gate.stall_pass); /* Home's custom spring uses no lv_anim tick. */
    sample.loops = 0;
    gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(!gate.stall_pass); /* No handler observations cannot pass. */
    sample.loops = 500;
    sample.animation_ticks = 300;
    sample.damage_frames = 0;
    gate = orb_scroll_gates(true, 400, 325, 6500000, 120000, &sample);
    assert(!gate.invalidation_pass);
    gate = orb_scroll_gates(true, 400, 325, 0, 0, &sample);
    assert(!gate.fps_pass && !gate.invalidation_pass);
    return 0;
}
"""
    )
    binary = tmp_path / "gates"
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            f"-I{ROOT / 'firmware/components/orb_devtools'}",
            f"-I{ROOT / 'firmware/components/orb_display/include'}",
            str(source),
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
