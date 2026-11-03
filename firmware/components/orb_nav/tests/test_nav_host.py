"""Run portable navigation contracts without LVGL, SDK, or board state."""

import os
import subprocess
from pathlib import Path

import pytest

COMPONENT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize(
    "suite",
    [
        "nav",
        "compositor",
        "compositor_esp_allocator",
        "admission",
        "iris_oracle",
        "compositor_work",
    ],
)
def test_navigation_contracts(tmp_path, suite):
    source = COMPONENT / {
        "nav": "orb_nav.c",
        "admission": "orb_nav_admission.c",
    }.get(suite, "orb_nav_compositor.c")
    sources = [source]
    if source.name == "orb_nav_compositor.c":
        sources.append(COMPONENT / "orb_nav_compositor_slots.c")
    binary = tmp_path / f"test_{suite}"
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            *(
                ["-DESP_PLATFORM", f"-I{COMPONENT / 'tests' / 'stubs'}"]
                if suite == "compositor_esp_allocator"
                else []
            ),
            *(
                [
                    "-D__XTENSA__",
                    "-D_FORTIFY_SOURCE=0",
                    "-Dmemcpy=orb_nav_test_memcpy",
                    "-fno-builtin-memcpy",
                ]
                if suite == "compositor_work"
                else []
            ),
            f"-I{COMPONENT / 'include'}",
            f"-I{COMPONENT.parent / 'orb_memory' / 'include'}",
            f"-I{COMPONENT.parent / 'orb_lv_simd' / 'include'}",
            *(str(source) for source in sources),
            str(COMPONENT / "tests" / f"test_{suite}.c"),
            "-lm",
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
