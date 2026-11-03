"""Check the generated fonts in real LVGL, on this Mac (scripts/fonts.sh check).

Builds Orb's LVGL fork, orb_fonts and the test app's self-test for the host with the
system C compiler, then (lvgl_check/main.c): checks every pack record against the C font
it was made from, decodes RLE glyphs on two threads at once as the firmware's two draw
units do, runs the self-test with the font pack, and has LVGL render every preview line
(each content size with and without Bold Text, each style and variant, the tabular and
icon lines, the clocks). Then compares LVGL's pixels with the Pillow previews of the same
text. It also checks the live battery and Wi-Fi widgets through every font setting,
then renders status.png and screen-face.png directly with LVGL. --status-only skips
the checks and renders those two previews (used after generation). Run scripts/fonts.sh
first to generate the font assets.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image, ImageChops

import build
import preview
import spec

LVGL = build.REPO / "firmware" / "third_party" / "lvgl__lvgl"
TEST_APP = build.COMPONENT / "test_app" / "main"
WORK = build.CACHE / "lvgl_check"
STATUS = build.REPO / "firmware" / "components" / "orb_status"
W, H = 1400, 480

LV_CONF = """#ifndef LV_CONF_H
#define LV_CONF_H
/* LVGL for the orb_fonts host check: like the firmware's, but 32-bit colour and no OS
 * (the two-thread decoding test calls the font decoder directly). */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_USE_FLOAT 1
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_USE_FONT_COMPRESSED 1
#define LV_USE_OBSERVER 1
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#endif
"""


def compile_all(cc: str, jobs: int) -> Path:
    conf = WORK / "conf"
    conf.mkdir(parents=True, exist_ok=True)
    if not (conf / "lv_conf.h").exists() or (conf / "lv_conf.h").read_text() != LV_CONF:
        (conf / "lv_conf.h").write_text(LV_CONF)
    flags = [
        "-O1",
        "-std=gnu11",
        "-w",
        "-DLV_CONF_INCLUDE_SIMPLE",
        "-DLV_KCONFIG_IGNORE",
        f"-I{conf}",
        f"-I{LVGL}",
    ]
    flags += [
        f"-I{build.COMPONENT / 'include'}",
        f"-I{build.COMPONENT / 'src'}",
        f"-I{TEST_APP}",
        f"-I{STATUS / 'include'}",
        f"-I{build.REPO / 'firmware/components/orb_memory/include'}",
        f"-I{build.REPO / 'firmware/components/orb_ui/include'}",
        f"-I{build.REPO / 'firmware/components/orb_tokens/include'}",
    ]
    key = hashlib.sha256((cc + " ".join(flags) + LV_CONF).encode()).hexdigest()[:12]
    obj_dir = WORK / f"obj-{key}"
    sources = sorted((LVGL / "src").rglob("*.c"))
    sources += sorted((build.COMPONENT / "src").glob("*.c")) + sorted(
        (build.COMPONENT / "fonts").glob("*.c")
    )
    sources += sorted((STATUS / "src").glob("*.c"))
    sources += [build.REPO / "firmware/components/orb_memory/orb_memory.c"]
    sources += [build.REPO / "firmware/components/orb_ui/tokens/orb_palette.c"]
    sources += [TEST_APP / "orb_fonts_selftest.c"]
    sources += sorted((build.HERE / "lvgl_check").glob("*.c"))

    def obj(src: Path) -> Path:
        rel = src.relative_to(build.REPO)
        return obj_dir / (str(rel).replace("/", "__") + ".o")

    def compile_one(src: Path) -> None:
        o = obj(src)
        if o.exists() and o.stat().st_mtime >= src.stat().st_mtime and "lvgl__lvgl" in str(src):
            return  # LVGL sources rarely change; Orb's are always rebuilt (headers change)
        o.parent.mkdir(parents=True, exist_ok=True)
        source_flags = flags
        if src.is_relative_to(STATUS):
            # The new component must also build as plain C99, without IDF headers.
            source_flags = [f for f in flags if f not in ("-std=gnu11", "-w")]
            source_flags += ["-std=c99", "-Wall", "-Wextra", "-Werror"]
        res = subprocess.run(
            [cc, *source_flags, "-c", str(src), "-o", str(o)], capture_output=True, text=True
        )
        if res.returncode:
            raise SystemExit(f"{src}:\n{res.stderr}")

    print(f"compiling {len(sources)} files")
    with ThreadPoolExecutor(jobs) as pool:
        list(pool.map(compile_one, sources))
    exe = WORK / "orb_fonts_check"
    result = subprocess.run(
        [cc, *[str(obj(s)) for s in sources], "-lm", "-lz", "-lpthread", "-o", str(exe)],
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise SystemExit(f"host link failed:\n{result.stderr}")
    return exe


def load_fonts() -> tuple[dict, list]:
    """The generator's plan with the metrics of the fonts it last wrote."""
    fonts, sets = build.plan()
    manifest = json.loads((build.WORK / "manifest.json").read_text())
    for name, f in fonts.items():
        dump = build.WORK / "dump" / name / "font_info.json"
        if not dump.exists() or name not in manifest:
            raise SystemExit(f"{dump} is missing: run scripts/fonts.sh first")
        f.info["dump"] = json.loads(dump.read_text())
        f.info["c"] = manifest[name]["c"]
    return fonts, sets


def jobs(fonts: dict, sets: list) -> list[dict]:
    out = []
    lines = preview.SAMPLES + [preview.TABULAR_SAMPLE]
    icon_row = " ".join(preview.icon(m) for m in preview.ICON_ROW)
    inline = preview.icon("TERMINAL") + " Sessions " + preview.icon("CHEVRON_RIGHT")
    sizes = [c.enum for c in spec.CONTENT_SIZES]
    for fs in sets:
        where = {"size": sizes.index(fs.size.enum), "bold": int(fs.bold)}
        for i, row in enumerate(fs.styles):
            for flags, variant in ((0, "regular"), (1, "emphasized")):
                v = row[variant]
                for j, text in enumerate(lines + [inline]):
                    tab = j == len(preview.SAMPLES)
                    font = v["tnum"] if tab else v["text"]
                    out.append(
                        dict(
                            where,
                            kind="text",
                            index=i,
                            flags=flags | (2 if tab else 0),
                            text=text,
                            font=font,
                            ls=row["letter_space"],
                        )
                    )
                out.append(
                    dict(
                        where,
                        kind="icons",
                        index=i,
                        flags=flags,
                        text=icon_row,
                        font=v["icons"],
                        ls=0,
                    )
                )
        if fs.size.enum == "DEFAULT":
            for i, c in enumerate(fs.clocks):
                for text in ("10:09", "12:45 PM", "12" + spec.TIME_SEP + "45"):
                    out.append(
                        dict(
                            where,
                            kind="clock",
                            index=i,
                            flags=0,
                            text=text,
                            font=c["font"],
                            ls=c["letter_space"],
                        )
                    )
    for n, job in enumerate(out):
        job["out"] = str(WORK / "renders" / f"{n:04d}.pgm")
    return out


def compare(job: dict, ctx: "preview.Ctx") -> tuple[list[int], Image.Image, Image.Image]:
    """Inked pixels of LVGL's render vs the Pillow preview of the same line, counted by
    difference in 4 bpp steps (blending may add one level)."""
    lv = Image.open(job["out"]).convert("L")
    font = ctx.font(job["font"])
    ours = Image.new("RGB", (W, H))
    preview.draw(
        ours,
        10,
        10 + preview.ascent(font),
        preview.Run(job["text"], font, preview.WHITE, job["ls"]),
    )
    ours = ours.getchannel("R")
    inked = ImageChops.lighter(lv, ours).point(lambda v: 255 if v else 0)
    hist = ImageChops.difference(lv, ours).histogram(mask=inked)
    steps = [0] * 16
    for v, n in enumerate(hist):
        steps[min(15, (max(0, v - 1)) // 17)] += n
    return steps, lv, ours


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 4))
    ap.add_argument(
        "--status-only", action="store_true", help="render live status and face previews only"
    )
    args = ap.parse_args()

    spec.check()
    exe = compile_all(args.cc, args.jobs)
    status_ppm = WORK / "status.ppm"
    face_ppm = WORK / "screen-face.ppm"
    if args.status_only:
        subprocess.run(
            [str(exe), "--status-only", str(build.PACK_FILE), str(status_ppm), str(face_ppm)],
            check=True,
        )
        save_status_previews(status_ppm, face_ppm)
        return

    fonts, sets = load_fonts()
    todo = jobs(fonts, sets)
    renders = WORK / "renders"
    if renders.exists():
        shutil.rmtree(renders)
    renders.mkdir(parents=True)
    job_file = WORK / "jobs.tsv"
    job_file.write_text(
        "".join(
            f"{j['out']}\t{j['size']}\t{j['bold']}\t{j['kind']}\t{j['index']}\t{j['flags']}\t{j['text']}\n"
            for j in todo
        )
    )
    ids = build.font_ids(fonts)
    records = WORK / "records.tsv"
    records.write_text(
        "".join(
            f"{ids[n]}\t{n}\t{build.WORK / 'records' / (n + '.rec')}\n"
            for n, f in sorted(fonts.items(), key=lambda kv: ids[kv[0]])
            if f.home == "app"
        )
    )
    res = subprocess.run(
        [
            str(exe),
            str(build.PACK_FILE),
            str(records),
            str(job_file),
            str(status_ppm),
            str(face_ppm),
        ],
        capture_output=True,
        text=True,
    )
    print(res.stdout.strip())
    if res.returncode:
        print(res.stderr)
        raise SystemExit(f"host check failed ({res.returncode})")
    save_status_previews(status_ppm, face_ppm)

    ctx = preview.Ctx(build.WORK, fonts, sets)
    total = [0] * 16
    bad = 0
    for job in todo:
        steps, lv, ours = compare(job, ctx)
        total = [a + b for a, b in zip(total, steps)]
        far = sum(steps[3:])
        if far > 0.001 * sum(steps):  # more than 0.1% of a line's ink 3+ steps off
            bad += 1
            stem = Path(job["out"]).with_suffix("")
            Image.merge("RGB", (lv, ours, ours)).save(f"{stem}-diff.png")
            print(
                f"  differs: {job['font']} {job['text']!r}: {far} pixels 3+ steps apart (see {stem}-diff.png)"
            )
    n = sum(total) or 1
    worst = max(i for i, c in enumerate(total) if c)
    print(
        f"LVGL vs Pillow previews, {len(todo)} lines: {total[0] / n:.1%} of inked pixels identical, "
        f"{(total[0] + total[1]) / n:.2%} within one 4 bpp step, worst {worst} steps"
    )
    if bad:
        sys.exit(1)


def save_status_previews(status_ppm: Path, face_ppm: Path) -> None:
    """Losslessly encode LVGL's RGB framebuffer; Pillow does no drawing here."""
    build.SCREENS.mkdir(parents=True, exist_ok=True)
    for source in (status_ppm, face_ppm):
        target = build.SCREENS / source.with_suffix(".png").name
        with Image.open(source) as rendered:
            rendered.save(target, optimize=True)
        print(f"LVGL preview: {target}")


if __name__ == "__main__":
    main()
