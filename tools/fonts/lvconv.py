"""Runs the pinned lv_font_conv and reads back what it made."""

import json
import re
import subprocess
from pathlib import Path

# Installed by scripts/fonts.sh with `npm ci` from package-lock.json, which pins the
# tarball's sha512 (lv_font_conv 1.5.3 bundles its dependencies).
LV_FONT_CONV = Path(__file__).resolve().parent / "node_modules" / "lv_font_conv" / "lv_font_conv.js"


def ranges(codepoints: list[int]) -> str:
    """Compact -r argument: 0x20-0x7E,0xA0-0xFF,0x2013,..."""
    out, cps = [], sorted(set(codepoints))
    i = 0
    while i < len(cps):
        j = i
        while j + 1 < len(cps) and cps[j + 1] == cps[j] + 1:
            j += 1
        out.append(f"0x{cps[i]:X}" if i == j else f"0x{cps[i]:X}-0x{cps[j]:X}")
        i = j + 1
    return ",".join(out)


def _run(args: list[str], cwd: Path) -> None:
    if not LV_FONT_CONV.exists():
        raise SystemExit(f"{LV_FONT_CONV} is missing: run scripts/fonts.sh (it runs npm ci)")
    cmd = ["node", str(LV_FONT_CONV), *args]
    res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if res.returncode:
        raise SystemExit(f"lv_font_conv failed ({' '.join(args)}):\n{res.stdout}{res.stderr}")


def convert(
    cwd: Path,
    ttf: str,
    size: int,
    codepoints: list[int],
    name: str,
    out_c: str,
    compress: bool,
    fallback: str | None,
) -> None:
    """`ttf` and `out_c` are relative to `cwd`, so the "Opts:" line lv_font_conv writes
    into the C file is the same on every machine."""
    args = ["--font", ttf, "--autohint-off", "-r", ranges(codepoints)]
    args += [
        "--size",
        str(size),
        "--bpp",
        "4",
        "--format",
        "lvgl",
        "--lv-include",
        "lvgl.h",
    ]
    args += ["--lv-font-name", name, "-o", out_c]
    if not compress:
        args.append("--no-compress")
    if fallback:
        args += ["--lv-fallback", fallback]
    _run(args, cwd)


def dump(cwd: Path, ttf: str, size: int, codepoints: list[int], out_dir: str) -> dict:
    """Glyph metrics, kerning and 8-bit coverage exactly as lv_font_conv rasterised them."""
    args = [
        "--font",
        ttf,
        "--autohint-off",
        "-r",
        ranges(codepoints),
        "--size",
        str(size),
    ]
    args += ["--bpp", "8", "--format", "dump", "--full-info", "-o", out_dir]
    _run(args, cwd)
    return json.loads((cwd / out_dir / "font_info.json").read_text())


_FIELD = r"\.{}\s*=\s*(-?\d+)"


def read_c(text: str) -> dict:
    """Metrics and storage of a generated font file."""
    info = {}
    for key in (
        "line_height",
        "base_line",
        "underline_position",
        "underline_thickness",
        "bitmap_format",
    ):
        m = re.search(_FIELD.format(key), text)
        info[key] = int(m.group(1)) if m else None

    def array_bytes(pattern: str, item: int) -> int:
        total = 0
        for body in re.findall(pattern + r"[^=]*=\s*\{(.*?)\n\};", text, re.S):
            body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
            total += len([t for t in body.replace("\n", ",").split(",") if t.strip()]) * item
        return total

    info["bitmap_bytes"] = array_bytes(r"glyph_bitmap\[\]", 1)
    info["glyph_count"] = len(re.findall(r"\{\.bitmap_index", text)) - 1
    info["glyph_dsc_bytes"] = (info["glyph_count"] + 1) * 8
    info["kern_bytes"] = (
        array_bytes(r"kern_class_values\[\]", 1)
        + array_bytes(r"kern_left_class_mapping\[\]", 1)
        + array_bytes(r"kern_right_class_mapping\[\]", 1)
        + array_bytes(r"kern_pair_glyph_ids\[\]", 1)
        + array_bytes(r"kern_pair_values\[\]", 1)
    )
    info["cmap_bytes"] = array_bytes(r"unicode_list_\d+\[\]", 2) + 16 * len(
        re.findall(r"\.range_start\s*=", text)
    )
    info["total_bytes"] = (
        info["bitmap_bytes"] + info["glyph_dsc_bytes"] + info["kern_bytes"] + info["cmap_bytes"]
    )
    return info


def set_metrics(text: str, **values: int) -> str:
    for key, value in values.items():
        text, n = re.subn(rf"(\.{key}\s*=\s*)-?\d+", rf"\g<1>{value}", text, count=1)
        assert n == 1, key
    return text
