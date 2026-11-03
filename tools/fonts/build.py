"""Generate firmware/components/orb_fonts from pinned upstream sources.

    scripts/fonts.sh                 # everything: fonts, pack, C tables, README sizes, previews
    scripts/fonts.sh --no-previews

Inputs are downloaded once into tools/fonts/.cache/downloads; every run checks them
against the pinned SHA-256s and unpacks what it uses afresh, so nothing unverified is
ever read. Outputs are deterministic, so a second run leaves git clean.

Only a rescue Inter Body face, its icons and compact status markings stay in the app.
All other faces, including the default set and clocks, live in the signed assets pack.
"""

import argparse
import hashlib
import io
import json
import math
import os
import shutil
import tarfile
import urllib.request
import zipfile
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

import assets
import emit
import icons
import inter
import lvconv
import mapped
import pack
import spec
import status_fonts

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
COMPONENT = REPO / "firmware" / "components" / "orb_fonts"
SCREENS = REPO / "docs" / "screens" / "fonts"
CACHE = HERE / ".cache"
WORK = CACHE / "build"
PACK_FILE = COMPONENT / "pack" / "orb_fonts.pack"
ASSETS_PARTITION_OFFSET = 0xC20000
MAPPED_FLASH_END = 16 * 1024 * 1024

SOURCES = {
    "framework7": {
        "url": "https://registry.npmjs.org/framework7-icons/-/framework7-icons-5.0.5.tgz",
        "sha256": "601d32f5d75595025c1a9bdf433cb616c3c20f6026683dea3cdd425b9efefa05",
        "file": "framework7-icons-5.0.5.tgz",
    },
    "inter": {
        "url": "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip",
        "sha256": "9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e",
        "file": "Inter-4.1.zip",
    },
    "phosphor": {
        # npm registry tarball of @phosphor-icons/core 2.1.1 (its sha512 integrity is
        # v4ARvrip4qBCImOE5rmP...; this is the same file's SHA-256).
        "url": "https://registry.npmjs.org/@phosphor-icons/core/-/core-2.1.1.tgz",
        "sha256": "313332be6190b724da24107addd781799b48bf76b13963f24501112ffe1baadd",
        "file": "phosphor-core-2.1.1.tgz",
    },
}


def fetch(key: str) -> bytes:
    """The pinned archive, downloaded if needed and checked on every run."""
    src = SOURCES[key]
    path = CACHE / "downloads" / src["file"]
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        print(f"downloading {src['url']}")
        with urllib.request.urlopen(src["url"], timeout=120) as r:
            data = r.read()
        tmp = path.with_suffix(".part")
        tmp.write_bytes(data)
        tmp.rename(path)
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != src["sha256"]:
        raise SystemExit(f"{path}: SHA-256 {digest}, expected {src['sha256']}")
    return data


@dataclass
class Sources:
    variable: Path  # InterVariable.ttf
    inter_licence: str
    assets: Path  # framework7/*.svg and phosphor/<weight>/*.svg, only icons Orb uses
    phosphor_licence: str
    framework7_licence: str


def unpack() -> Sources:
    """Fresh copies of the members the generator reads, from the verified archives."""
    out = WORK / "src"
    if out.exists():
        shutil.rmtree(out)
    (out / "inter").mkdir(parents=True)
    with zipfile.ZipFile(io.BytesIO(fetch("inter"))) as z:
        variable = out / "inter" / "InterVariable.ttf"
        variable.write_bytes(z.read("InterVariable.ttf"))
        inter_licence = z.read("LICENSE.txt").decode()

    names = {
        i.asset.removeprefix("phosphor:") for i in spec.ICONS if i.asset.startswith("phosphor:")
    }
    wanted = {"package/LICENSE": None}
    for weight in ("fill", *icons.PHOSPHOR_STROKES):
        for n in names:
            file = f"{n}.svg" if weight == "regular" else f"{n}-{weight}.svg"
            wanted[f"package/assets/{weight}/{file}"] = out / "phosphor" / weight / file
    found: dict[str, bytes] = {}
    with tarfile.open(fileobj=io.BytesIO(fetch("phosphor"))) as t:
        for m in t.getmembers():
            if m.isfile() and m.name in wanted:
                found[m.name] = t.extractfile(m).read()
    missing = sorted(set(wanted) - set(found))
    if missing:
        raise SystemExit("Phosphor 2.1.1 lacks " + ", ".join(missing[:5]))
    for name, dest in wanted.items():
        if dest:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(found[name])
    phosphor_licence = found["package/LICENSE"].decode()
    names = {
        spec.icon_asset(asset, macro, fill).split("@")[0]
        for macro, _, asset, fill in spec.icon_codepoints()
        if not asset.startswith("phosphor:")
    } | {"battery_0", "bolt_fill", "exclamationmark"}
    wanted = {f"package/svg/{name}.svg": out / "framework7" / f"{name}.svg" for name in names}
    wanted["package/LICENSE"] = None
    found = {}
    with tarfile.open(fileobj=io.BytesIO(fetch("framework7"))) as t:
        for m in t.getmembers():
            if m.isfile() and m.name in wanted:
                found[m.name] = t.extractfile(m).read()
    missing = sorted(set(wanted) - set(found))
    if missing:
        raise SystemExit("Framework7 5.0.5 lacks " + ", ".join(missing[:5]))
    for name, dest in wanted.items():
        if dest:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(found[name])
    return Sources(
        variable, inter_licence, out, phosphor_licence, found["package/LICENSE"].decode()
    )


@dataclass
class Font:
    name: str  # C symbol
    kind: str  # text, tnum, icons, clock, status
    px: int
    weight: int
    opsz: int
    codepoints: list[int]
    tracking_units: int = 0  # built into the advances
    fallback: str | None = None
    metrics_from: str | None = None  # take line metrics from this font (tnum)
    home: str = "pack"  # app: a C file in the app image; pack: a record in the font pack
    info: dict = field(default_factory=dict)

    @property
    def compress(self) -> bool:
        if self.kind in ("icons", "clock"):
            return True  # a handful per screen: decoding cost doesn't matter, size does
        return self.px >= spec.COMPRESS_MIN_PX

    @property
    def lv_codepoints(self) -> list[int]:
        return (
            [0x20] + [cp for _, cp, _, _ in spec.icon_codepoints()]
            if self.kind == "icons"
            else self.codepoints
        )


def split_tracking(tracking: int, px: int) -> tuple[int, int]:
    """Apple tracking (1/1000 em at px) as (whole pixels for LVGL's letter_space,
    font units built into the advances for the rest)."""
    total = tracking * px / 1000
    letter_space = round(total)
    return letter_space, round((total - letter_space) * 2048 / px)


@dataclass
class FontSet:
    """The fonts of one content size, with or without Bold Text."""

    size: spec.ContentSize
    bold: bool
    styles: list[
        dict
    ]  # per style: style, letter_space, regular/emphasized -> text/tnum/icons/weight
    clocks: list[dict]  # per clock face: clock, font, letter_space

    @property
    def key(self) -> str:
        return f"{self.size.enum.lower()}{'_bold' if self.bold else ''}"

    @property
    def title(self) -> str:
        return self.size.name + (", Bold Text" if self.bold else "")

    def font_names(self) -> list[str]:
        out = []
        for r in self.styles:
            for v in ("regular", "emphasized"):
                out += [r[v]["text"], r[v]["tnum"], r[v]["icons"]]
        out += [c["font"] for c in self.clocks]
        return list(dict.fromkeys(out))


def plan() -> tuple[dict[str, Font], list[FontSet]]:
    fonts: dict[str, Font] = {}

    def add(f: Font) -> str:
        old = fonts.get(f.name)
        if old:
            assert (old.kind, old.px, old.weight, old.opsz, old.tracking_units, old.codepoints) == (
                f.kind,
                f.px,
                f.weight,
                f.opsz,
                f.tracking_units,
                f.codepoints,
            ), f"{f.name} is needed with two different settings"
        else:
            fonts[f.name] = f
        return f.name

    sets = []
    for size in spec.CONTENT_SIZES:
        for bold in (False, True):
            rows = []
            for s in spec.text_styles(size, bold):
                letter_space, baked = split_tracking(s.tracking, s.px)
                row = {"style": s, "letter_space": letter_space, "baked_units": baked}
                for variant, weight in (("regular", s.weight), ("emphasized", s.emphasized_weight)):
                    wname = spec.WEIGHT_NAMES[weight]
                    icon_name = add(
                        Font(f"orb_icons_{s.px}_{wname}", "icons", s.px, weight, s.opsz, [])
                    )
                    text_name = add(
                        Font(
                            f"orb_inter_{s.px}_{wname}",
                            "text",
                            s.px,
                            weight,
                            s.opsz,
                            spec.TEXT_CODEPOINTS,
                            baked,
                            icon_name,
                        )
                    )
                    tnum_name = add(
                        Font(
                            f"orb_inter_{s.px}_{wname}_tnum",
                            "tnum",
                            s.px,
                            weight,
                            s.opsz,
                            spec.TABULAR_CODEPOINTS,
                            baked,
                            text_name,
                            text_name,
                        )
                    )
                    row[variant] = {
                        "text": text_name,
                        "tnum": tnum_name,
                        "icons": icon_name,
                        "weight": weight,
                    }
                rows.append(row)
            clocks = []
            for c in spec.CLOCK_FACES:
                face = c.with_bold_text(bold)
                letter_space, baked = split_tracking(face.tracking, face.px)
                name = add(
                    Font(
                        f"orb_clock_{face.px}_{spec.WEIGHT_NAMES[face.weight]}",
                        "clock",
                        face.px,
                        face.weight,
                        spec.CLOCK_OPSZ,
                        spec.CLOCK_CODEPOINTS,
                        baked,
                    )
                )
                clocks.append({"clock": face, "font": name, "letter_space": letter_space})
            sets.append(FontSet(size, bold, rows, clocks))
    # A signed application always owns a readable rescue face, even when an
    # independently updated assets partition is interrupted or incompatible.
    for name in ("orb_inter_28_regular", "orb_icons_28_regular"):
        fonts[name].home = "app"
    # A pack font's fallback chain stays inside its set, so a set can be freed on its own.
    for fs in sets:
        names = set(fs.font_names())
        for n in names:
            assert fonts[n].fallback is None or fonts[n].fallback in names, n
    status_fonts.extend_plan(Font, fonts, sets)
    return fonts, sets


def font_ids(fonts: dict[str, Font]) -> dict[str, int]:
    """Font ids in the generated table: app fonts first, then the pack's records."""
    order = sorted((f for f in fonts.values() if f.home == "app"), key=lambda f: f.name)
    order += sorted((f for f in fonts.values() if f.home == "pack"), key=lambda f: f.name)
    return {f.name: i for i, f in enumerate(order)}


def default_set(sets: list[FontSet]) -> FontSet:
    return sets[0]


def mapped_record_indices(
    fonts: dict[str, Font], sets: list[FontSet], names: list[str], records: dict[str, bytes]
) -> set[int]:
    """Balance raw flash backing across every old/new accessibility-set union."""
    return {
        i
        for i, name in enumerate(names)
        if mapped.uses_raw_record(fonts[name].kind, fonts[name].px, fonts[name].weight)
    }


def validate_mapped_layout(entries: list[dict], raw_records: set[int]) -> None:
    # The S3 maps physical flash below 16MiB. Keep hot records first in the v2
    # envelope/bundle so later compressed records can use the full partition.
    base = ASSETS_PARTITION_OFFSET + assets.HEADER.size + assets.BUNDLE.size
    if any(
        base + entries[i]["offset"] + entries[i]["size"] > MAPPED_FLASH_END for i in raw_records
    ):
        raise ValueError("raw font record exceeds the mapped flash aperture")


def make_ttf(f: Font, variable: Path, assets: Path, ttf: Path) -> dict:
    """Writes the font's TrueType source for lv_font_conv; returns notes for the manifest."""
    if f.kind == "status":
        return status_fonts.make_ttf(f, variable, assets, ttf)
    info: dict = {}
    family = f"Orb Inter {f.px}"
    if f.kind == "icons":
        text = inter.instance(variable, f.weight, f.opsz)
        cap = text["OS/2"].sCapHeight
        scale = icons.icon_scale(cap, spec.ICON_CIRCLE_CAP_HEIGHTS)
        stroke = spec.ICON_STROKE_RATIO * inter.stem_em(text) * icons.UPM / scale
        info["icon"] = icons.build(
            assets,
            spec.icon_codepoints(),
            spec.icon_groups(),
            stroke,
            scale,
            cap / 2,
            round(spec.ICON_SIDE_BEARING * icons.UPM),
            text["hhea"].ascent,
            text["hhea"].descent,
            text["hmtx"][text.getBestCmap()[0x20]][0],
            f"Orb Icons {f.px} {spec.WEIGHT_NAMES[f.weight]}",
            f.px,
            ttf,
        )
        info["icon"].update(stroke=round(stroke, 2), centre=cap / 2 * f.px / icons.UPM)
        return info
    font = inter.instance(variable, f.weight, f.opsz)
    if f.kind in ("tnum", "clock"):
        contexts = {}
        for cp in f.codepoints:
            c = chr(cp)
            if c in "AMP":
                continue
            if f.kind == "tnum" and c == ":":
                contexts[cp] = "a:a"  # after a word: the baseline colon, tabular width
            elif c == spec.TIME_SEP:
                contexts[cp] = "0:0"  # between figures: raised to their centre
            else:
                contexts[cp] = "0" + c + "0"
        frozen = inter.frozen_glyphs(variable, f.weight, f.opsz, contexts, {"tnum": True})
        inter.remap_cmap(font, frozen)
        info["frozen"] = {f"U+{cp:04X}": g for cp, g in frozen.items()}
    inter.subset_to(font, f.codepoints)
    if f.kind == "text":
        info["stem_em"] = round(inter.stem_em(font), 4)
    inter.track(font, f.tracking_units)
    inter.rename(font, family, f"{spec.WEIGHT_NAMES[f.weight].title()} {f.kind}")
    font.save(ttf)
    return info


def symmetric_box(text: str, centre_px: float) -> str:
    """Icon fonts: make the line box symmetric about the icons' centre, so an icon-only
    label centres its icon (LVGL centres the line box, not the ink)."""
    m = lvconv.read_c(text)
    top, bottom = m["line_height"] - m["base_line"], -m["base_line"]
    half = max(top - centre_px, centre_px - bottom)
    base_line = math.ceil(half - centre_px)
    line_height = math.ceil(centre_px + half) + base_line
    assert line_height - base_line >= top and base_line >= -bottom
    return lvconv.set_metrics(text, line_height=line_height, base_line=base_line)


HEADER = """/*
 * {name}: {what}
 * Generated by tools/fonts/build.py (scripts/fonts.sh). Do not edit.
 * {source}
 */

"""


def describe(f: Font) -> tuple[str, str]:
    if f.kind == "status":
        return (
            f"Tabular battery figures and charging bolt at {f.px} px, {f.weight}",
            "Inter 4.1 (SIL OFL 1.1) and Framework7 Icons 5.0.5 (MIT); see ../licenses/.",
        )
    if f.kind == "icons":
        return (
            f"{len(f.lv_codepoints) - 1} icons at {f.px} px, drawn to match Inter {spec.WEIGHT_NAMES[f.weight]}",
            "Framework7 Icons 5.0.5 and Phosphor 2.1.1 fallbacks (MIT, see ../licenses), weight-matched.",
        )
    kind = {"text": "", "tnum": " tabular figures", "clock": " clock digits"}[f.kind]
    return (
        f"Inter {spec.WEIGHT_NAMES[f.weight].title()} {f.px} px (opsz {f.opsz}){kind}",
        "Inter 4.1 (SIL OFL 1.1, see ../licenses/Inter-OFL.txt), instanced by the generator.",
    )


def _convert(f: Font, work: Path) -> dict:
    ttf = f"ttf/{f.name}.ttf"
    lvconv.convert(
        work,
        ttf,
        f.px,
        f.lv_codepoints,
        f.name,
        f"c/{f.name}.c",
        f.compress,
        f.fallback,
    )
    return lvconv.dump(work, ttf, f.px, f.lv_codepoints, f"dump/{f.name}")


def generate(fonts: dict[str, Font], src: Sources, jobs: int) -> dict[str, bytes]:
    """Builds every font; writes the app's C files and returns every font's pack record."""
    for sub in ("ttf", "c", "dump", "records"):
        if (WORK / sub).exists():
            shutil.rmtree(WORK / sub)
        (WORK / sub).mkdir(parents=True)
    out_dir = COMPONENT / "fonts"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    with ProcessPoolExecutor(jobs) as pool:
        futures = {
            name: pool.submit(make_ttf, f, src.variable, src.assets, WORK / "ttf" / f"{name}.ttf")
            for name, f in fonts.items()
        }
        for name, fut in futures.items():
            fonts[name].info.update(fut.result())
    with ThreadPoolExecutor(jobs) as pool:
        futures = {name: pool.submit(_convert, f, WORK) for name, f in fonts.items()}
        for name, fut in futures.items():
            fonts[name].info["dump"] = fut.result()

    ids = font_ids(fonts)
    records = {}
    # Text fonts first: tabular fonts copy their line metrics.
    for f in sorted(fonts.values(), key=lambda f: (f.kind == "tnum", f.name)):
        text = (WORK / "c" / f"{f.name}.c").read_text()
        if f.kind == "icons":
            text = symmetric_box(text, f.info["icon"]["centre"])
        if f.metrics_from:
            src_c = fonts[f.metrics_from].info["c"]
            text = lvconv.set_metrics(
                text,
                line_height=src_c["line_height"],
                base_line=src_c["base_line"],
                underline_position=src_c["underline_position"],
                underline_thickness=src_c["underline_thickness"],
            )
        f.info["c"] = lvconv.read_c(text)
        rec = pack.record(pack.parse_c(text), ids[f.fallback] if f.fallback else -1)
        records[f.name] = rec
        (WORK / "records" / f"{f.name}.rec").write_bytes(rec)
        f.info["record_bytes"] = len(rec)
        if f.home == "app":
            what, source = describe(f)
            (out_dir / f"{f.name}.c").write_text(
                HEADER.format(name=f.name, what=what, source=source) + text
            )
    return records


NOTICES = """Orb's fonts and icons contain Inter 4.1 (instanced and subset as "Orb Inter"),
Framework7 Icons 5.0.5 (offset at other weights), and Phosphor Icons 2.1.1 fallbacks,
under the licences below.

=== Inter ===

{inter}
=== Framework7 Icons ===

{framework7}
=== Phosphor Icons ===

{phosphor}"""


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--no-previews", action="store_true", help="skip docs/screens/fonts")
    ap.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 4))
    args = ap.parse_args()

    spec.check()
    src = unpack()
    fonts, sets = plan()
    print(f"{len(fonts)} fonts ({sum(f.home == 'app' for f in fonts.values())} in the app image)")
    records = generate(fonts, src, args.jobs)

    notices = NOTICES.format(
        inter=src.inter_licence.strip() + "\n",
        framework7=src.framework7_licence.strip() + "\n",
        phosphor=src.phosphor_licence.strip() + "\n",
    )
    ids = font_ids(fonts)
    pack_names = sorted((n for n, f in fonts.items() if f.home == "pack"), key=ids.get)
    raw_records = mapped_record_indices(fonts, sets, pack_names, records)
    image, entries, pack_id = pack.build(
        [records[n] for n in pack_names], notices, raw_records=raw_records
    )
    validate_mapped_layout(entries, raw_records)
    PACK_FILE.parent.mkdir(parents=True, exist_ok=True)
    PACK_FILE.write_bytes(image)
    for name, e in zip(pack_names, entries, strict=True):
        fonts[name].info["pack"] = e

    lic = COMPONENT / "licenses"
    lic.mkdir(exist_ok=True)
    (lic / "Inter-OFL.txt").write_text(src.inter_licence)
    (lic / "Phosphor-MIT.txt").write_text(src.phosphor_licence)
    (lic / "Framework7-MIT.txt").write_text(src.framework7_licence)
    emit.icons_header(COMPONENT / "include" / "orb_icons.h")
    emit.font_table(
        COMPONENT / "src" / "orb_fonts_table.c",
        fonts,
        sets,
        ids,
        pack_names,
        entries,
        pack_id,
        len(image),
    )
    emit.notices(COMPONENT / "src" / "orb_fonts_notices.c", notices)
    status_fonts.emit_selector(COMPONENT / "src" / "orb_fonts_status.c", fonts, sets)
    emit.readme_sizes(COMPONENT / "README.md", fonts, sets, len(image))
    emit.readme_styles(COMPONENT / "README.md", sets)
    emit.coverage_header(COMPONENT / "test_app" / "main" / "orb_fonts_coverage.h")
    manifest = {
        name: {k: v for k, v in f.info.items() if k not in ("dump",)}
        | {"kind": f.kind, "px": f.px, "home": f.home}
        for name, f in sorted(fonts.items())
    }
    (WORK / "manifest.json").write_text(json.dumps(manifest, indent=1, default=str))
    total = {
        h: sum(f.info["c"]["total_bytes"] for f in fonts.values() if f.home == h)
        for h in ("app", "pack")
    }
    print(
        f"  app image: {total['app']} B of fonts; pack: {len(image)} B ({total['pack']} B loaded)"
    )

    if not args.no_previews:
        import preview

        preview.render_all(SCREENS, WORK, fonts, sets)


if __name__ == "__main__":
    main()
