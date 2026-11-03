"""Crash extraction and decoding with synthetic IDF artifacts and local fake tools."""

import csv
import hashlib
import json
import struct
import sys
from pathlib import Path

import pytest
from orb_devtools import crash

ROOT = Path(__file__).resolve().parents[1]


def _aligned(value: bytes) -> bytes:
    return value + bytes((-len(value)) % 4)


def core_fixture(identity="a" * 64, *, names=(b"ESP_CORE_DUMP_INFO",), note_version=None):
    """Independent ELF32 PT_NOTE construction matching IDF's published structs."""
    descriptor = struct.pack("<I", crash.VERSION if note_version is None else note_version)
    descriptor += identity.encode("ascii") + b"\0\0"
    notes = b"".join(
        struct.pack("<III", len(name) + 1, len(descriptor), 8266)
        + _aligned(name + b"\0")
        + _aligned(descriptor)
        for name in names
    )
    elf = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x01\x01" + bytes(9),
        4,
        94,
        1,
        0,
        52,
        0,
        0,
        52,
        32,
        1,
        0,
        0,
        0,
    )
    elf += struct.pack("<IIIIIIII", 4, 84, 0, 0, len(notes), len(notes), 0, 4)
    elf += notes
    content = struct.pack("<III", 12 + len(elf) + 32, crash.VERSION, 2) + elf
    return content + hashlib.sha256(content).digest()


def mutated_core(raw, offset, fmt, *values):
    content = bytearray(raw[:-32])
    struct.pack_into(fmt, content, offset, *values)
    return bytes(content) + hashlib.sha256(content).digest()


def partition_entry(kind=1, subtype=3, offset=0x1EF0000, size=0x100000, flags=0):
    return struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size, b"coredump", flags)


def partition_fixture(entries=None):
    entries = [partition_entry()] if entries is None else entries
    data = b"".join(entries)
    data += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(data, usedforsecurity=False).digest()
    return data.ljust(4096, b"\xff")


def fake_executable(path, code):
    path.write_text(f"#!{sys.executable}\n" + code)
    path.chmod(0o755)
    return path


@pytest.fixture
def elf(tmp_path):
    path = tmp_path / "orb.elf"
    path.write_bytes(b"synthetic matching application ELF bytes")
    return path


def elf_sha(elf):
    return hashlib.sha256(elf.read_bytes()).hexdigest()


def test_core_identity_integrity_and_flash_padding():
    raw = core_fixture("AB" * 32)
    parsed, identity = crash.core_identity(raw + b"\xff" * 4096)
    assert parsed == raw
    assert identity == "ab" * 32


@pytest.mark.parametrize(
    "raw, message",
    [
        (b"", "truncated"),
        (b"\xff" * 100, "no core dump"),
        (bytes(100), "no core dump"),
        (core_fixture()[:-1], "length"),
        (core_fixture()[:-1] + b"\0", "integrity"),
        (mutated_core(core_fixture(), 0, "<I", 95), "length"),
        (mutated_core(core_fixture(), 4, "<I", 0x90103), "unsupported"),
        (mutated_core(core_fixture(), 12 + 4, "<B", 2), "32-bit"),
        (mutated_core(core_fixture(), 12 + 18, "<H", 243), "Xtensa"),
        (mutated_core(core_fixture(), 12 + 28, "<I", 0xFFFFFFF0), "program headers"),
        (mutated_core(core_fixture(), 12 + 42, "<H", 31), "program headers"),
        (mutated_core(core_fixture(), 12 + 52 + 4, "<I", 0xFFFFFFF0), "segment outside"),
        (mutated_core(core_fixture(), 12 + 84, "<I", 0xFFFFFFF0), "note outside"),
        (core_fixture(names=(b"OTHER_INFO",)), "exactly one"),
        (core_fixture(names=(b"ESP_CORE_DUMP_INFO", b"ESP_CORE_DUMP_INFO")), "exactly one"),
        (core_fixture(note_version=0x90103), "identity note"),
        (core_fixture("a" * 16), "identity note"),
        (core_fixture("z" * 64), "full ELF SHA"),
    ],
)
def test_core_rejects_corrupt_or_unsupported_artifacts(raw, message):
    with pytest.raises(ValueError, match=message):
        crash.core_identity(raw)


def test_verify_elf_full_hash_and_wrong_build(elf):
    identity = elf_sha(elf)
    assert crash.verify_elf(elf, identity.upper()) == identity
    with pytest.raises(ValueError, match="mismatch"):
        crash.verify_elf(elf, "0" * 64)
    with pytest.raises(ValueError, match="64-character"):
        crash.verify_elf(elf, identity[:16])


def test_core_partition_uses_actual_table_offset():
    table = partition_fixture([partition_entry(offset=0x1800000, size=0x200000)])
    assert crash.coredump_partition(table) == (0x1800000, 0x200000)


@pytest.mark.parametrize(
    "table, message",
    [
        (partition_fixture([]), "unique coredump"),
        (partition_fixture([partition_entry(), partition_entry()]), "unique coredump"),
        (partition_fixture([partition_entry(flags=1)]), "encrypted"),
        (partition_fixture([partition_entry(size=0)]), "bounds"),
        (partition_fixture([partition_entry(offset=0x8000)]), "bounds"),
        (partition_fixture([partition_entry(offset=0x1F00000, size=0x200000)]), "bounds"),
        (partition_fixture()[:48] + bytes(16), "MD5 mismatch"),
        (partition_entry() + b"\xff" * 32, "missing MD5"),
        (partition_entry(), "missing checksum"),
        (b"\0" * 64, "magic"),
    ],
)
def test_partition_table_rejects_unsafe_or_incomplete_data(table, message):
    with pytest.raises(ValueError, match=message):
        crash.coredump_partition(table)


def test_backtrace_pc_sp_pairs_and_continuations():
    text = """PC: 0x42000001 PS: 0x00060330
Backtrace: 0x42000001:0x3fc90000 0x40380002:0x3fc90040
  0x42000003:0x3fc90080 0x42000001:0x3fc900c0
ELF file SHA256: unrelated
0x42000004:0x3fc90100
"""
    assert crash.backtrace_addresses(text) == ["0x42000001", "0x40380002", "0x42000003"]
    with pytest.raises(ValueError, match="no ESP-IDF"):
        crash.backtrace_addresses("PC: 0x42000001 SP: 0x3fc90000")


def test_log_identity_requires_one_complete_application_hash():
    identity = "ab" * 32
    assert crash.log_identity(f"ELF file SHA256: {identity}\nELF SHA256: {identity}") == identity
    for text in (
        f"ELF SHA256: {identity[:16]}",
        f"ELF SHA256: {identity}ff",
        f"ELF SHA256: {identity}\nELF SHA256: {'1' * 64}",
    ):
        with pytest.raises(ValueError, match="unambiguous"):
            crash.log_identity(text)


def test_elf_mismatch_stops_before_decoder_or_symbolizer(tmp_path, elf, monkeypatch):
    artifact = tmp_path / "core.raw"
    artifact.write_bytes(core_fixture("0" * 64))
    panic = tmp_path / "panic.log"
    panic.write_text(f"ELF file SHA256: {'0' * 64}\nBacktrace: 0x42000001:0x3fc90000\n")
    calls = []
    monkeypatch.setattr(crash.subprocess, "run", lambda *a, **k: calls.append(a))
    assert (
        crash.main(
            [
                "decode",
                str(artifact),
                "--elf",
                str(elf),
                "--decoder",
                "/not/run",
                "--gdb",
                "/not/run",
            ]
        )
        == 1
    )
    assert crash.main(["backtrace", str(panic), "--elf", str(elf), "--addr2line", "/not/run"]) == 1
    assert calls == []
    assert not artifact.with_suffix(".raw.verified.raw").exists()


def test_fake_decoder_receives_verified_core_and_creates_receipt(tmp_path, elf):
    artifact = tmp_path / "core.raw"
    raw = core_fixture(elf_sha(elf))
    artifact.write_bytes(raw + b"\xff" * 500)
    argv_path = tmp_path / "decoder-argv.json"
    decoder = fake_executable(
        tmp_path / "decoder",
        "import json, pathlib, sys\n"
        + f"pathlib.Path({str(argv_path)!r}).write_text(json.dumps(sys.argv[1:]))\n"
        + "print('Crashed task: ui; frame main.c:42')\n",
    )
    assert (
        crash.main(
            [
                "decode",
                str(artifact),
                "--elf",
                str(elf),
                "--decoder",
                str(decoder),
                "--gdb",
                "/fake/gdb",
            ]
        )
        == 0
    )
    args = json.loads(argv_path.read_text())
    assert args[:5] == ["--chip", "esp32s3", "info_corefile", "-t", "raw"]
    verified = Path(args[args.index("-c") + 1])
    assert verified.read_bytes() == raw
    assert args[-3:] == ["--gdb", "/fake/gdb", str(elf)]
    receipt = json.loads(artifact.with_suffix(".raw.json").read_text())
    assert receipt["elf_sha256"] == elf_sha(elf)
    assert "main.c:42" in artifact.with_suffix(".raw.txt").read_text()


def test_jsonl_incident_symbolization_uses_full_continued_backtrace(tmp_path, elf):
    artifact = tmp_path / "incident.jsonl"
    lines = [
        f"ELF file SHA256: {elf_sha(elf)}",
        "Guru Meditation Error",
        "Backtrace: 0x42000001:0x3fc90000",
        "  0x42000002:0x3fc90040",
    ]
    artifact.write_text(
        "\n".join(
            json.dumps({"time": "2026-10-26T12:00:00Z", "boot": "fixture", "text": line})
            for line in lines
        )
        + "\n"
    )
    argv_path = tmp_path / "symbolizer-argv.json"
    symbolizer = fake_executable(
        tmp_path / "addr2line",
        "import json, pathlib, sys\n"
        + f"pathlib.Path({str(argv_path)!r}).write_text(json.dumps(sys.argv[1:]))\n"
        + "print('ui_task at orb_display.c:301')\n",
    )
    assert (
        crash.main(["backtrace", str(artifact), "--elf", str(elf), "--addr2line", str(symbolizer)])
        == 0
    )
    assert json.loads(argv_path.read_text()) == [
        "-pfiaC",
        "-e",
        str(elf),
        "0x42000001",
        "0x42000002",
    ]
    assert "orb_display.c:301" in artifact.with_suffix(".jsonl.txt").read_text()


def test_explicit_full_app_sha_allows_old_panic_log(tmp_path, elf):
    artifact = tmp_path / "panic.log"
    artifact.write_text("Backtrace: 0x42000001:0x3fc90000\n")
    symbolizer = fake_executable(tmp_path / "addr2line", "print('main at main.c:1')\n")
    assert (
        crash.main(
            [
                "backtrace",
                str(artifact),
                "--elf",
                str(elf),
                "--app-sha",
                elf_sha(elf),
                "--addr2line",
                str(symbolizer),
            ]
        )
        == 0
    )


def test_capture_uses_single_transport_and_preserves_original_bytes(tmp_path, elf):
    table = partition_fixture([partition_entry(size=0x1000)])
    raw = core_fixture(elf_sha(elf)).ljust(0x1000, b"\xff")
    packet = b"ORBCORE1" + struct.pack("<II", len(table), len(raw)) + table + raw
    packet_path = tmp_path / "packet.bin"
    packet_path.write_bytes(packet)
    calls = tmp_path / "transport-args.json"
    transport = fake_executable(
        tmp_path / "transport",
        "import json, pathlib, sys\n"
        + f"pathlib.Path({str(calls)!r}).write_text(json.dumps(sys.argv[1:]))\n"
        + f"sys.stdout.buffer.write(pathlib.Path({str(packet_path)!r}).read_bytes())\n",
    )
    output = tmp_path / "capture/core.raw"
    crash.capture(output, transport)
    assert json.loads(calls.read_text()) == ["--read-core"]
    assert output.read_bytes() == raw
    assert output.with_suffix(".raw.partitions.bin").read_bytes() == table


@pytest.mark.parametrize(
    "packet",
    [
        b"",
        b"ORBCORE1",
        b"WRONGHDR" + bytes(8),
        b"ORBCORE1" + struct.pack("<II", 4096, 4096) + bytes(12),
    ],
)
def test_capture_rejects_incomplete_transport_framing(tmp_path, packet):
    packet_path = tmp_path / "packet.bin"
    packet_path.write_bytes(packet)
    transport = fake_executable(
        tmp_path / "transport",
        "import pathlib, sys\n"
        + f"sys.stdout.buffer.write(pathlib.Path({str(packet_path)!r}).read_bytes())\n",
    )
    with pytest.raises(ValueError):
        crash.capture(tmp_path / "core.raw", transport)


def test_capture_failure_keeps_invalid_core_for_forensics(tmp_path, elf):
    table = partition_fixture([partition_entry(size=0x1000)])
    raw = b"\xff" * 4096
    packet_path = tmp_path / "packet.bin"
    packet_path.write_bytes(b"ORBCORE1" + struct.pack("<II", len(table), len(raw)) + table + raw)
    transport = fake_executable(
        tmp_path / "transport",
        "import pathlib, sys\n"
        + f"sys.stdout.buffer.write(pathlib.Path({str(packet_path)!r}).read_bytes())\n",
    )
    output = tmp_path / "core.raw"
    assert (
        crash.main(
            ["capture", "--elf", str(elf), "--output", str(output), "--transport", str(transport)]
        )
        == 1
    )
    assert output.read_bytes() == raw
    assert output.with_suffix(".raw.partitions.bin").read_bytes() == table


def test_partition_layout_preserves_existing_offsets_and_shrinks_storage():
    def number(value):
        value = value.strip()
        return int(value[:-1]) * 1024 * 1024 if value.endswith("M") else int(value, 0)

    rows = {}
    next_offset = 0
    with (ROOT / "firmware/partitions.csv").open() as source:
        for row in csv.reader(line for line in source if not line.lstrip().startswith("#")):
            if not row:
                continue
            name, kind, _subtype, start, size = [cell.strip() for cell in row[:5]]
            alignment = 0x10000 if kind == "app" else 0x1000
            offset = (
                number(start) if start else (next_offset + alignment - 1) // alignment * alignment
            )
            length = number(size)
            assert offset >= next_offset
            rows[name] = (offset, length)
            next_offset = offset + length
    assert rows["nvs"] == (0x9000, 0x6000)
    assert rows["otadata"] == (0xF000, 0x2000)
    assert rows["phy_init"] == (0x11000, 0x1000)
    assert rows["ota_0"] == (0x20000, 0x600000)
    assert rows["ota_1"] == (0x620000, 0x600000)
    assert rows["assets"] == (0xC20000, 0x800000)
    assert rows["storage"] == (0x1420000, 0xAE0000)
    assert rows["coredump"] == (0x1F00000, 0x100000)
    assert next_offset == 32 * 1024 * 1024
    config = (ROOT / "firmware/sdkconfig.defaults").read_text()
    assert "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y" in config
    assert "CONFIG_APP_RETRIEVE_LEN_ELF_SHA=64" in config


def test_capture_and_decode_complete_fake_pipeline(tmp_path, elf):
    table = partition_fixture([partition_entry(size=0x1000)])
    original = core_fixture(elf_sha(elf))
    raw = original.ljust(0x1000, b"\xff")
    packet = tmp_path / "packet.bin"
    packet.write_bytes(b"ORBCORE1" + struct.pack("<II", len(table), len(raw)) + table + raw)
    transport = fake_executable(
        tmp_path / "transport",
        "import pathlib, sys\n"
        "assert sys.argv[1:] == ['--read-core']\n"
        f"sys.stdout.buffer.write(pathlib.Path({str(packet)!r}).read_bytes())\n",
    )
    decoder = fake_executable(
        tmp_path / "decoder",
        "import pathlib, sys\n"
        "core = pathlib.Path(sys.argv[sys.argv.index('-c') + 1]).read_bytes()\n"
        f"assert len(core) == {len(original)}\n"
        "print('Core capture decoded successfully')\n",
    )
    output = tmp_path / "capture.raw"
    assert (
        crash.main(
            [
                "capture",
                "--elf",
                str(elf),
                "--output",
                str(output),
                "--transport",
                str(transport),
                "--decoder",
                str(decoder),
                "--gdb",
                "/fake/gdb",
            ]
        )
        == 0
    )
    assert output.read_bytes() == raw
    assert "decoded successfully" in output.with_suffix(".raw.txt").read_text()
    assert json.loads(output.with_suffix(".raw.json").read_text())["elf_sha256"] == elf_sha(elf)


def test_capture_will_not_overwrite_existing_evidence(tmp_path, elf, monkeypatch):
    output = tmp_path / "capture.raw"
    output.write_bytes(b"irreplaceable evidence")
    calls = []
    monkeypatch.setattr(crash.subprocess, "run", lambda *args, **kwargs: calls.append(args))
    assert crash.main(["capture", "--elf", str(elf), "--output", str(output)]) == 1
    assert output.read_bytes() == b"irreplaceable evidence"
    assert calls == []


def test_transfer_partition_size_mismatch_rejected(tmp_path):
    table = partition_fixture([partition_entry(size=0x2000)])
    raw = bytes(0x1000)
    packet = tmp_path / "packet.bin"
    packet.write_bytes(b"ORBCORE1" + struct.pack("<II", len(table), len(raw)) + table + raw)
    transport = fake_executable(
        tmp_path / "transport",
        "import pathlib, sys\n"
        f"sys.stdout.buffer.write(pathlib.Path({str(packet)!r}).read_bytes())\n",
    )
    output = tmp_path / "core.raw"
    with pytest.raises(ValueError, match="differs from partition"):
        crash.capture(output, transport)
    assert not output.exists()


def test_failed_decoder_reports_error_and_retains_verified_core(tmp_path, elf, capsys):
    artifact = tmp_path / "core.raw"
    artifact.write_bytes(core_fixture(elf_sha(elf)))
    decoder = fake_executable(
        tmp_path / "decoder",
        "import sys\nprint('fake GDB failure', file=sys.stderr)\nraise SystemExit(7)\n",
    )
    assert (
        crash.main(
            [
                "decode",
                str(artifact),
                "--elf",
                str(elf),
                "--decoder",
                str(decoder),
                "--gdb",
                "/fake/gdb",
            ]
        )
        == 1
    )
    assert "fake GDB failure" in capsys.readouterr().err
    assert artifact.with_suffix(".raw.verified.raw").is_file()
    assert not artifact.with_suffix(".raw.json").exists()


def test_elf_selection_refuses_ambiguous_build(tmp_path, monkeypatch):
    monkeypatch.setattr(crash, "ROOT", tmp_path)
    build = tmp_path / "firmware/build"
    build.mkdir(parents=True)
    with pytest.raises(ValueError, match="expected one"):
        crash.resolve_elf(None)
    one = build / "one.elf"
    one.write_bytes(b"one")
    assert crash.resolve_elf(None) == one
    (build / "two.elf").write_bytes(b"two")
    with pytest.raises(ValueError, match="expected one"):
        crash.resolve_elf(None)
    assert crash.resolve_elf(one) == one


def test_console_text_preserves_plain_and_unrelated_json():
    assert (
        crash.console_text('plain\n{"text":"Backtrace: first\\nsecond"}\n{"event":1}\nnull')
        == 'plain\nBacktrace: first\nsecond\n{"event":1}\nnull'
    )
