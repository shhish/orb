"""Integration tests against the actual LVGL fork, without a board or network."""

import re
import subprocess
import sys
from pathlib import Path

import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import build  # noqa: E402
import lvgl_check  # noqa: E402


@pytest.fixture(scope="module")
def host_executable():
    assert build.PACK_FILE.is_file(), "Run scripts/fonts.sh before the integration tests"
    return lvgl_check.compile_all("clang", 4)


def test_status_widgets_real_lvgl(host_executable):
    result = subprocess.run(
        [str(host_executable), "--status-tests", str(build.PACK_FILE)],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    summary = re.search(r"orb_status: (\d+) checks, 0 failures", result.stdout)
    assert summary and int(summary[1]) >= 1900, result.stdout


@pytest.fixture(scope="module")
def rendered_previews(host_executable, tmp_path_factory):
    folder = tmp_path_factory.mktemp("status-lvgl")
    sheet, face = folder / "status.ppm", folder / "screen-face.ppm"
    command = [
        str(host_executable),
        "--status-only",
        str(build.PACK_FILE),
        str(sheet),
        str(face),
    ]
    subprocess.run(command, check=True, capture_output=True, text=True, timeout=120)
    return command, sheet, face


def test_status_previews_have_requested_states(rendered_previews):
    _, sheet_path, face_path = rendered_previews
    with Image.open(sheet_path) as sheet:
        assert sheet.size == (1400, 480)
        assert sheet.getpixel((0, 0)) == (0, 0, 0)
        for y in (0, 240):
            colors = set(sheet.crop((0, y, 1400, y + 240)).get_flattened_data())
            assert {(255, 255, 255), (255, 66, 69), (48, 209, 88), (255, 214, 0)} <= colors
            for i in range(7):
                x = 220 + i * 172
                plain = sheet.crop((x - 30, y + 72, x + 30, y + 113)).tobytes()
                percentage = sheet.crop((x - 30, y + 115, x + 30, y + 156)).tobytes()
                assert plain != percentage, (y, i)
    with Image.open(face_path) as face:
        assert face.size == (466, 466)
        assert face.getpixel((233, 440)) == (0, 0, 0)
        assert face.getpixel((0, 0)) == (0, 0, 0)
        assert (255, 146, 48) in set(face.get_flattened_data())


def test_status_previews_reproduce_exactly(rendered_previews):
    command, sheet, face = rendered_previews
    before = (sheet.read_bytes(), face.read_bytes())
    subprocess.run(command, check=True, capture_output=True, text=True, timeout=120)
    assert (sheet.read_bytes(), face.read_bytes()) == before
