"""The font pack: every Orb font that isn't in the app image (Larger and AX3 sizes, Bold
Text), stored in the `assets` partition and prepared by orb_fonts_pack.c before
publishing a complete setting. UI lookups never load records.

A font is one record: lv_font_conv's tables for LVGL's built-in font format (glyph
descriptors, character maps, kerning, bitmaps) in a fixed little-endian layout, so the
loader only points LVGL's structures into it. Most records are zlib streams, inflated
into PSRAM. Explicit raw records have stored == size and retain the exact same record
layout; their bitmap bytes can stay mapped in flash while metadata lives in PSRAM.
Raw records precede streams physically without changing their directory indices.

Pack layout (all little-endian, every part 4-byte aligned):

    0   "ORBFONTS"         magic
    8   u32 version        PACK_VERSION
    12  u32 count          records
    16  u8 id[16]          SHA-256 of everything from offset 48 on; the firmware's
                           generated table names the pack it was built with
    32  u32 size           of the whole pack
    36  u32 dir_offset     count x {u32 offset, u32 stored, u32 size, u32 crc32}
    40  u32 notices_offset the licences of the fonts and icons inside (UTF-8)
    44  u32 notices_size

Record layout (RECORD_HEADER, then the arrays at the offsets it gives):

    0   "OFR1", u16 glyph_count, u16 cmap_num, u16 line_height, i16 base_line,
    12  i8 underline_position, u8 underline_thickness, u8 bpp, u8 bitmap_format,
    16  u8 kern_type (0 none, 1 classes, 2 pairs), u8 stride, u16 kern_scale,
    20  i16 fallback font id (-1: none), u16 0,
    24  u32 glyph_dsc, u32 cmaps, u32 kern, u32 bitmap, u32 bitmap_size, u32 0
    glyph_dsc: glyph_count x {u32 bitmap_index, u16 adv_w, u8 box_w, u8 box_h,
               i8 ofs_x, i8 ofs_y, u16 0}
    cmaps:     cmap_num x {u32 range_start, u16 range_length, u16 glyph_id_start,
               u32 unicode_list, u32 glyph_id_ofs_list, u16 list_length, u8 type,
               u8 glyph_id_ofs_size}  (offsets; 0 = none)
    kern 1:    {u8 left_cnt, u8 right_cnt, u16 0, u32 left_map, u32 right_map,
               u32 values}  (maps are glyph_count bytes)
    kern 2:    {u32 pair_cnt, u8 glyph_ids_size, u8 0, u16 0, u32 glyph_ids, u32 values}
"""

import hashlib
import re
import struct
import zlib

PACK_MAGIC = b"ORBFONTS"
PACK_VERSION = 1
PACK_HEADER = struct.Struct("<8sII16sIIII")
DIR_ENTRY = struct.Struct("<IIII")
RECORD_MAGIC = b"OFR1"
RECORD_HEADER = struct.Struct("<4sHHHhbBBBBBHhHIIIIII")
GLYPH = struct.Struct("<IHBBbbH")
CMAP = struct.Struct("<IHHIIHBB")
KERN_CLASSES = struct.Struct("<BBHIII")
KERN_PAIRS = struct.Struct("<IBBHII")

CMAP_TYPES = {
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL": 0,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL": 1,
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY": 2,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY": 3,
}


def _numbers(body: str) -> list[int]:
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return [int(t, 0) for t in re.split(r"[\s,]+", body) if t]


def _array(text: str, name: str) -> tuple[str, list[int]] | None:
    m = re.search(
        r"static\s+(?:LV_ATTRIBUTE_LARGE_CONST\s+)?const\s+(\w+)\s+"
        + re.escape(name)
        + r"\[\]\s*=\s*\{(.*?)\};",
        text,
        re.S,
    )
    return (m.group(1), _numbers(m.group(2))) if m else None


def _field(text: str, key: str, default: int | None = None) -> int:
    m = re.search(rf"\.{key}\s*=\s*(-?\d+)", text)
    if not m:
        if default is None:
            raise ValueError(f"no .{key} in font source")
        return default
    return int(m.group(1))


def parse_c(text: str) -> dict:
    """lv_font_conv 1.5.3's C output, read back into numbers."""
    font: dict = {}
    font["bitmap"] = bytes(_array(text, "glyph_bitmap")[1])
    font["glyphs"] = [
        tuple(int(v) for v in g)
        for g in re.findall(
            r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
            r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}",
            text,
        )
    ]
    cmaps = []
    for body in re.findall(r"\{\s*(\.range_start.*?)\s*\}", text, re.S):
        c = dict(re.findall(r"\.(\w+)\s*=\s*([\w-]+)", body))
        cmap = {
            "range_start": int(c["range_start"]),
            "range_length": int(c["range_length"]),
            "glyph_id_start": int(c["glyph_id_start"]),
            "list_length": int(c["list_length"]),
            "type": CMAP_TYPES[c["type"]],
            "unicode_list": None,
            "glyph_id_ofs_list": None,
            "ofs_size": 0,
        }
        if c["unicode_list"] != "NULL":
            ctype, values = _array(text, c["unicode_list"])
            assert ctype == "uint16_t", ctype
            cmap["unicode_list"] = values
        if c["glyph_id_ofs_list"] != "NULL":
            ctype, values = _array(text, c["glyph_id_ofs_list"])
            cmap["glyph_id_ofs_list"] = values
            cmap["ofs_size"] = {"uint8_t": 1, "uint16_t": 2}[ctype]
        cmaps.append(cmap)
    font["cmaps"] = cmaps
    font["kern"] = None
    classes = re.search(r"lv_font_fmt_txt_kern_classes_t kern_classes =\s*\{(.*?)\};", text, re.S)
    pairs = re.search(r"lv_font_fmt_txt_kern_pair_t kern_pairs =\s*\{(.*?)\};", text, re.S)
    if classes:
        block = classes.group(1)
        font["kern"] = {
            "type": 1,
            "left_cnt": _field(block, "left_class_cnt"),
            "right_cnt": _field(block, "right_class_cnt"),
            "left": _array(text, "kern_left_class_mapping")[1],
            "right": _array(text, "kern_right_class_mapping")[1],
            "values": _array(text, "kern_class_values")[1],
        }
    elif pairs:
        block = pairs.group(1)
        ctype, ids = _array(text, "kern_pair_glyph_ids")
        font["kern"] = {
            "type": 2,
            "pair_cnt": _field(block, "pair_cnt"),
            "ids_size": _field(block, "glyph_ids_size"),
            "ids": ids,
            "values": _array(text, "kern_pair_values")[1],
        }
        assert {"uint8_t": 0, "uint16_t": 1}[ctype] == font["kern"]["ids_size"]
    dsc = re.search(r"font_dsc = \{(.*?)\};", text, re.S).group(1)
    for key in ("kern_scale", "cmap_num", "bpp", "bitmap_format"):
        font[key] = _field(dsc, key)
    font["stride"] = _field(dsc, "stride", 0)
    pub = text[text.index("PUBLIC FONT") :]
    for key in ("line_height", "base_line", "underline_position", "underline_thickness"):
        font[key] = _field(pub, key)
    assert font["cmap_num"] == len(cmaps)
    return font


class _Blob:
    def __init__(self, size: int):
        self.data = bytearray(size)

    def add(self, data: bytes) -> int:
        while len(self.data) % 4:
            self.data.append(0)
        offset = len(self.data)
        self.data += data
        return offset


def record(font: dict, fallback: int) -> bytes:
    """One font as the loader reads it (see the module docstring)."""
    blob = _Blob(RECORD_HEADER.size)
    glyphs = b"".join(GLYPH.pack(*g, 0) for g in font["glyphs"])
    glyph_off = blob.add(glyphs)
    cmap_rows = []
    for c in font["cmaps"]:
        ul = (
            blob.add(struct.pack(f"<{len(c['unicode_list'])}H", *c["unicode_list"]))
            if c["unicode_list"]
            else 0
        )
        go = 0
        if c["glyph_id_ofs_list"]:
            fmt = "B" if c["ofs_size"] == 1 else "H"
            go = blob.add(
                struct.pack(f"<{len(c['glyph_id_ofs_list'])}{fmt}", *c["glyph_id_ofs_list"])
            )
        cmap_rows.append(
            CMAP.pack(
                c["range_start"],
                c["range_length"],
                c["glyph_id_start"],
                ul,
                go,
                c["list_length"],
                c["type"],
                c["ofs_size"],
            )
        )
    cmap_off = blob.add(b"".join(cmap_rows))
    kern_type, kern_off = 0, 0
    k = font["kern"]
    if k and k["type"] == 1:
        n = len(font["glyphs"])
        assert len(k["left"]) == n and len(k["right"]) == n
        left = blob.add(bytes(k["left"]))
        right = blob.add(bytes(k["right"]))
        values = blob.add(struct.pack(f"<{len(k['values'])}b", *k["values"]))
        kern_type = 1
        kern_off = blob.add(
            KERN_CLASSES.pack(k["left_cnt"], k["right_cnt"], 0, left, right, values)
        )
    elif k and k["type"] == 2:
        fmt = "B" if k["ids_size"] == 0 else "H"
        ids = blob.add(struct.pack(f"<{len(k['ids'])}{fmt}", *k["ids"]))
        values = blob.add(struct.pack(f"<{len(k['values'])}b", *k["values"]))
        kern_type = 2
        kern_off = blob.add(KERN_PAIRS.pack(k["pair_cnt"], k["ids_size"], 0, 0, ids, values))
    bitmap_off = blob.add(font["bitmap"])
    blob.data[: RECORD_HEADER.size] = RECORD_HEADER.pack(
        RECORD_MAGIC,
        len(font["glyphs"]),
        len(font["cmaps"]),
        font["line_height"],
        font["base_line"],
        font["underline_position"],
        font["underline_thickness"],
        font["bpp"],
        font["bitmap_format"],
        kern_type,
        font["stride"],
        font["kern_scale"],
        fallback,
        0,
        glyph_off,
        cmap_off,
        kern_off,
        bitmap_off,
        len(font["bitmap"]),
        0,
    )
    while len(blob.data) % 4:
        blob.data.append(0)
    return bytes(blob.data)


def build(
    records: list[bytes], notices: str, *, raw_records: set[int] | None = None
) -> tuple[bytes, list[dict], bytes]:
    """The pack image, its directory and its id."""
    raw_records = set() if raw_records is None else raw_records
    if any(i < 0 or i >= len(records) for i in raw_records):
        raise ValueError("raw record index out of range")
    body = _Blob(PACK_HEADER.size)
    notes = notices.encode()
    notices_off = body.add(notes)
    dir_off = body.add(bytes(DIR_ENTRY.size * len(records)))
    entries = [{} for _ in records]
    for i in sorted(range(len(records)), key=lambda i: (i not in raw_records, i)):
        rec = records[i]
        stored = rec if i in raw_records else zlib.compress(rec, 9)
        if i not in raw_records and len(stored) == len(rec):
            raise ValueError("compressed record size collides with raw record marker")
        offset = body.add(stored)
        entries[i] = {
            "offset": offset,
            "stored": len(stored),
            "size": len(rec),
            "crc32": zlib.crc32(rec),
        }
    for i, e in enumerate(entries):
        DIR_ENTRY.pack_into(
            body.data, dir_off + i * DIR_ENTRY.size, e["offset"], e["stored"], e["size"], e["crc32"]
        )
    while len(body.data) % 4:
        body.data.append(0)
    pack_id = hashlib.sha256(bytes(body.data[PACK_HEADER.size :])).digest()[:16]
    body.data[: PACK_HEADER.size] = PACK_HEADER.pack(
        PACK_MAGIC,
        PACK_VERSION,
        len(records),
        pack_id,
        len(body.data),
        dir_off,
        notices_off,
        len(notes),
    )
    return bytes(body.data), entries, pack_id
