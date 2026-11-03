"""Raw mapped records preserve logical font ids and the existing bitmap data."""

import hashlib
import struct
import zlib

import pytest

import assets
import budget
import build
import pack


def decode(image, entry):
    payload = image[entry["offset"] : entry["offset"] + entry["stored"]]
    return payload if entry["stored"] == entry["size"] else zlib.decompress(payload)


def test_raw_records_first_without_reordering_logical_ids():
    records = [b"compressed-first" * 64, bytes(range(256)), b"compressed-last" * 64]
    image, entries, identity = pack.build(records, "licences", raw_records={1})
    assert entries[1]["offset"] < entries[0]["offset"] < entries[2]["offset"]
    assert entries[1]["stored"] == entries[1]["size"]
    assert entries[0]["stored"] != entries[0]["size"]
    for record, entry in zip(records, entries, strict=True):
        assert entry["offset"] % 4 == 0
        assert decode(image, entry) == record
        assert entry["crc32"] == zlib.crc32(record)
    assert identity == hashlib.sha256(image[pack.PACK_HEADER.size :]).digest()[:16]
    assert pack.build(records, "licences", raw_records={1}) == (image, entries, identity)


@pytest.mark.parametrize("raw_records", [{-1}, {2}])
def test_raw_record_index_must_exist(raw_records):
    with pytest.raises(ValueError, match="raw record index"):
        pack.build([b"font"], "licences", raw_records=raw_records)


def test_compressed_size_cannot_impersonate_raw_marker():
    record = b"a" * 11
    assert len(zlib.compress(record, 9)) == len(record)
    with pytest.raises(ValueError, match="raw record marker"):
        pack.build([record], "licences")
    image, entries, _ = pack.build([record], "licences", raw_records={0})
    assert decode(image, entries[0]) == record


def test_raw_record_cannot_cross_flash_aperture():
    base = build.ASSETS_PARTITION_OFFSET + assets.HEADER.size + assets.BUNDLE.size
    entries = [{"offset": build.MAPPED_FLASH_END - base - 4, "size": 4}]
    build.validate_mapped_layout(entries, {0})
    entries[0]["size"] += 1
    with pytest.raises(ValueError, match="mapped flash aperture"):
        build.validate_mapped_layout(entries, {0})


def test_checked_in_mapped_fonts_and_flash_aperture():
    fonts, sets = build.plan()
    ids = build.font_ids(fonts)
    names = sorted((n for n, f in fonts.items() if f.home == "pack"), key=ids.get)
    image = build.PACK_FILE.read_bytes()
    header = pack.PACK_HEADER.unpack_from(image)
    assert header[0] == pack.PACK_MAGIC and header[1] == 1
    assert header[2] == len(names) and header[4] == len(image)
    entries = [
        dict(
            zip(
                ("offset", "stored", "size", "crc32"),
                pack.DIR_ENTRY.unpack_from(image, header[5] + i * pack.DIR_ENTRY.size),
                strict=True,
            )
        )
        for i in range(len(names))
    ]
    records = {name: decode(image, entry) for name, entry in zip(names, entries, strict=True)}
    selected = build.mapped_record_indices(fonts, sets, names, records)
    assert selected
    assert {i for i, e in enumerate(entries) if e["stored"] == e["size"]} == selected
    assert all(any(names[i] in fs.font_names() for fs in sets) for i in selected)
    assert any(fonts[names[i]].kind == "text" for i in selected)
    assert max(entries[i]["offset"] + entries[i]["stored"] for i in selected) <= min(
        entry["offset"] for i, entry in enumerate(entries) if i not in selected
    )
    font_base = build.ASSETS_PARTITION_OFFSET + assets.HEADER.size + assets.BUNDLE.size
    assert all(
        font_base + entries[i]["offset"] + entries[i]["size"] <= build.MAPPED_FLASH_END
        for i in selected
    )
    for name, entry in zip(names, entries, strict=True):
        assert records[name][:4] == pack.RECORD_MAGIC
        assert zlib.crc32(records[name]) == entry["crc32"]
        assert len(records[name]) == entry["size"]
        assert struct.unpack_from("<I", records[name], 36)[0] % 4 == 0


def test_every_complete_set_switch_has_physical_psram_headroom():
    font_pack = budget.read()
    result = budget.report(font_pack)
    # Conservative supplied device baseline, with optional caches reclaimed.
    # Add existing fonts back: the model includes their starting residency.
    available = 1_056_948 + 1_061_628 + 733 * 1024
    floor = 384 * 1024
    assert len(result["sets"]) == 6 and len(result["switches"]) == 36
    for switch in result["switches"]:
        assert switch["peak"] <= available - floor, switch
        # Retirement precedes loading. Even the worst pair stays within one
        # complete set plus the bounded streaming workspace, not two sets.
        assert switch["peak"] <= 1_250_000, switch
    # A concurrent 128 KiB TLS transient still leaves the 256 KiB reserve.
    assert max(s["peak"] for s in result["switches"]) + 128 * 1024 <= available - 256 * 1024


def test_budget_retains_shared_fallback_before_loading_new_roots():
    font_pack = budget.read()
    # Model a future set whose only root needs an already-loaded fallback;
    # retirement must follow the wire closure, not just top-level set IDs.
    root = next(r for r in font_pack.records.values() if r.fallback in font_pack.records)
    root_id = next(i for i, record in font_pack.records.items() if record is root)
    font_pack.sets["Fallback only"] = [root_id]
    loader = budget.Loader(font_pack)
    loader.load(root.fallback)
    shared = loader.used
    loader.retain("Fallback only")
    assert loader.used == shared
    assert root.fallback in loader.loaded


def test_every_switch_leaves_room_for_tls_and_assets_bitmap_migration():
    font_pack = budget.read()
    result = budget.report(font_pack)
    sets = {row["setting"]: row["shipped"] for row in result["sets"]}
    for switch in result["switches"]:
        loader = budget.Loader(font_pack)
        loader.preload(switch["to"])
        copies = sum(budget.charged(bitmap) for _, bitmap in loader.loaded.values() if bitmap)
        # Supplied post-screen-tour free bytes and optional cache estimate,
        # rounded down to decimal bytes; fixed handles cancel on both sides.
        available = 1_360_000 + 800_000 + sets[switch["from"]]["resident"]
        tls = 128 * 1024
        peak = max(switch["peak"], switch["resident"] + copies) + tls
        assert available - peak >= 384 * 1024, switch


def test_raw_selection_leaves_room_for_alignment_and_signed_sound_bundle():
    image = build.PACK_FILE.read_bytes()
    header = pack.PACK_HEADER.unpack_from(image)
    entries = [
        pack.DIR_ENTRY.unpack_from(image, header[5] + i * pack.DIR_ENTRY.size)
        for i in range(header[2])
    ]
    raw_end = max(off + stored for off, stored, size, _ in entries if stored == size)
    font_base = build.ASSETS_PARTITION_OFFSET + assets.HEADER.size + assets.BUNDLE.size
    assert font_base + raw_end + 8192 <= build.MAPPED_FLASH_END
    # The fixed signed sound bank is measured from its checked-in file, not a guess.
    sounds = (build.REPO / "tools/sounds/out/palette.orbs").read_bytes()
    envelope = assets.envelope(image, 8, sounds)
    assert len(envelope) + assets.SECTOR + 8192 <= assets.LIMIT
