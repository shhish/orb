"""Report the checked-in pack/table's Xtensa font PSRAM costs, without rasterizing.

The model follows orb_fonts_pack.c's recursive load order. Values include the
16-byte orb_memory allocation header, 4-byte payload alignment and the pointer
table; ESP-IDF backend headers/MMU bookkeeping remain board measurements. Raw
and compressed columns are hypothetical storage choices for the same records.
"""

import argparse
import json
import re
import struct
import zlib
from dataclasses import dataclass
from functools import cached_property
from pathlib import Path

import pack

ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "firmware/components/orb_fonts"
OWNER_HEADER = 16
RECORD_FONT = 108
NATIVE_CMAP = 24
NATIVE_GLYPH = 8
BITMAP_LEASE = 20
INFLATER = 10992
STREAM_INPUT = 1024


def charged(payload: int) -> int:
    return (payload + 3) // 4 * 4 + OWNER_HEADER


@dataclass(frozen=True)
class Record:
    name: str
    offset: int
    stored: int
    size: int
    data: bytes

    @property
    def raw(self) -> bool:
        return self.stored == self.size

    @property
    def prefix(self) -> int:
        return struct.unpack_from("<I", self.data, 36)[0]

    @property
    def bitmap(self) -> int:
        return struct.unpack_from("<I", self.data, 40)[0]

    @property
    def fallback(self) -> int:
        return struct.unpack_from("<h", self.data, 20)[0]

    @cached_property
    def compressed_size(self) -> int:
        return len(zlib.compress(self.data, 9))

    def metadata(self, owned_glyphs: bool) -> int:
        glyphs, cmaps = struct.unpack_from("<HH", self.data, 4)
        return RECORD_FONT + cmaps * NATIVE_CMAP + (0 if owned_glyphs else glyphs * NATIVE_GLYPH)


@dataclass(frozen=True)
class FontPack:
    records: dict[int, Record]
    sets: dict[str, list[int]]
    size: int


def read(
    font_pack: Path = COMPONENT / "pack/orb_fonts.pack",
    table: Path = COMPONENT / "src/orb_fonts_table.c",
) -> FontPack:
    image, source = font_pack.read_bytes(), table.read_text()
    header = pack.PACK_HEADER.unpack_from(image)
    if header[0] != pack.PACK_MAGIC or header[4] != len(image):
        raise ValueError("invalid font pack header")
    rows = re.findall(r"\{(\d+), (\d+), (\d+)\}, /\* (\d+) (orb_\w+) \*/", source)
    if len(rows) != header[2]:
        raise ValueError("font table/pack count mismatch")
    records = {}
    for index, (off, stored, size, font_id, name) in enumerate(rows):
        off, stored, size, font_id = map(int, (off, stored, size, font_id))
        entry = pack.DIR_ENTRY.unpack_from(image, header[5] + index * pack.DIR_ENTRY.size)
        if entry[:3] != (off, stored, size):
            raise ValueError("font table/pack directory mismatch")
        blob = image[off : off + stored]
        data = blob if stored == size else zlib.decompress(blob)
        if len(data) != size or zlib.crc32(data) != entry[3]:
            raise ValueError("invalid font record")
        records[font_id] = Record(name, off, stored, size, data)
    sets = {}
    for title, styles, clocks in re.findall(
        r"\[[01]\] = \{ /\* (.*?) \*/\s*\.styles = \{(.*?)\n\s*\},\s*\.clocks = (.*?),\n",
        source,
        re.S,
    ):
        ids = []
        for row in re.findall(r"\{\{(\d+, \d+, \d+)\}, \{(\d+, \d+, \d+)\}\}", styles):
            for face in row:
                ids.extend(map(int, face.split(", ")))
        ids.extend(int(font_id) for font_id in re.findall(r"\{(\d+), \d+\}", clocks))
        sets[title] = list(dict.fromkeys(ids))
    if len(sets) != 6 or any(len(ids) < 50 for ids in sets.values()):
        raise ValueError("expected six complete generated font sets")
    return FontPack(records, sets, len(image))


class Loader:
    def __init__(
        self,
        font_pack: FontPack,
        storage: str = "shipped",
        *,
        owned_glyphs: bool = True,
        streaming: bool = True,
    ):
        self.pack, self.storage = font_pack, storage
        self.owned_glyphs, self.streaming = owned_glyphs, streaming
        self.used = charged(len(font_pack.records) * 4)
        self.peak, self.largest = self.used, len(font_pack.records) * 4
        self.loaded: dict[int, tuple[int, int]] = {}

    def allocate(self, size: int) -> int:
        amount = charged(size)
        self.used += amount
        self.peak, self.largest = max(self.peak, self.used), max(self.largest, size)
        return amount

    def load(self, font_id: int) -> None:
        if font_id not in self.pack.records or font_id in self.loaded:
            return
        record = self.pack.records[font_id]
        raw = self.storage == "raw" or (self.storage == "shipped" and record.raw)
        retained = self.allocate(BITMAP_LEASE) if raw else 0
        retained += self.allocate(record.prefix if raw else record.size)
        if not raw:
            transient = self.allocate(STREAM_INPUT if self.streaming else record.compressed_size)
            transient += self.allocate(INFLATER)
            self.used -= transient
        self.load(record.fallback)
        retained += self.allocate(record.metadata(self.owned_glyphs))
        self.loaded[font_id] = retained, record.bitmap if raw else 0

    def preload(self, title: str) -> None:
        for font_id in self.pack.sets[title]:
            self.load(font_id)

    def retain(self, title: str) -> None:
        selected = set(self.pack.sets[title])
        # A requested root may not be loaded yet. Its wire fallback still has
        # to survive retirement when shared with the previous set.
        pending = list(selected)
        while pending:
            record = self.pack.records.get(pending.pop())
            if record and record.fallback >= 0 and record.fallback not in selected:
                selected.add(record.fallback)
                pending.append(record.fallback)
        for font_id in list(self.loaded):
            if font_id not in selected:
                self.used -= self.loaded.pop(font_id)[0]


def report(font_pack: FontPack, *, owned_glyphs: bool = True, streaming: bool = True) -> dict:
    result = {"pack_bytes": font_pack.size, "sets": [], "switches": []}
    for title, ids in font_pack.sets.items():
        records = [font_pack.records[i] for i in ids if i in font_pack.records]
        row = {
            "setting": title,
            "records": len(records),
            "decoded_bytes": sum(r.size for r in records),
            "compressed_storage": sum(r.compressed_size for r in records),
            "shipped_storage": sum(r.stored for r in records),
        }
        for storage in ("compressed", "raw", "shipped"):
            loader = Loader(font_pack, storage, owned_glyphs=owned_glyphs, streaming=streaming)
            loader.preload(title)
            row[storage] = {
                "resident": loader.used,
                "preload_peak": loader.peak,
                "mapped": sum(bitmap for _, bitmap in loader.loaded.values()),
                "largest_allocation": loader.largest,
            }
        result["sets"].append(row)
    for previous in font_pack.sets:
        for requested in font_pack.sets:
            loader = Loader(font_pack, owned_glyphs=owned_glyphs, streaming=streaming)
            loader.preload(previous)
            loader.peak = loader.used
            loader.retain(requested)
            loader.preload(requested)
            peak = loader.peak
            result["switches"].append(
                {"from": previous, "to": requested, "peak": peak, "resident": loader.used}
            )
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    parser.add_argument(
        "--legacy", action="store_true", help="duplicate glyph metadata/full compressed input"
    )
    args = parser.parse_args()
    result = report(read(), owned_glyphs=not args.legacy, streaming=not args.legacy)
    if args.json:
        print(json.dumps(result, indent=2))
        return
    print(
        "All values are bytes, including known owner headers; backend heap/MMU costs are unmeasured."
    )
    print(
        "Raw columns assume every record can map; that hypothetical image exceeds the partition/aperture."
    )
    print(
        "| Setting | Decoded | Zlib storage | Shipped storage | Zlib resident / peak | Raw resident / peak | Shipped resident / peak | Mapped |"
    )
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for row in result["sets"]:
        costs = [
            f"{row[storage]['resident']:,} / {row[storage]['preload_peak']:,}"
            for storage in ("compressed", "raw", "shipped")
        ]
        print(
            f"| {row['setting']} | {row['decoded_bytes']:,} | {row['compressed_storage']:,} | "
            f"{row['shipped_storage']:,} | {' | '.join(costs)} | {row['shipped']['mapped']:,} |"
        )
    worst = max(result["switches"], key=lambda switch: switch["peak"])
    print(f"Worst switch: {worst['from']} -> {worst['to']}: {worst['peak']:,} bytes.")


if __name__ == "__main__":
    main()
