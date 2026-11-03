"""Exercise the actual remote migration worker without a serial device."""

import hashlib
import json
import struct
import sys
from types import ModuleType, SimpleNamespace

import migrate_partitions as migration
import pytest


def table(rows=None):
    raw = b"".join(
        struct.pack("<HBBII16sI", 0x50AA, kind, subtype, start, size, name.encode(), 0)
        for name, (kind, subtype, start, size) in (rows or migration.PARTITIONS).items()
    )
    return (
        raw + b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(raw, usedforsecurity=False).digest()
    ).ljust(4096, b"\xff")


@pytest.fixture
def worker(tmp_path, monkeypatch):
    state = SimpleNamespace(
        mac=migration.DEV_MAC,
        writes=[],
        erases=[],
        resets=[],
        reads=[],
        bad_nvs=False,
        write_failure=False,
        verify_failure=False,
        short_read=False,
        saved={},
    )
    state.memory = {
        0x8000: table(),
        0x9000: b"n" * 0x6000,
        0x11000: b"p" * 0x1000,
        0xF000: b"o" * 0x2000,
    }
    manifest = {"board": migration.DEV_MAC, "files": []}
    for name, (offset, _, _) in migration.FILES.items():
        filename = name + ".bin"
        data = b"firmware-" + name.encode()
        (tmp_path / filename).write_bytes(data)
        manifest["files"].append(
            {
                "name": name,
                "file": filename,
                "offset": offset,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    (tmp_path / "manifest.json").write_text(json.dumps(manifest))

    class Device:
        CHIP_NAME = "ESP32-S3"

        def __enter__(self):
            return self

        def __exit__(self, *args):
            pass

        def read_mac(self, kind):
            assert kind == "BASE_MAC"
            return bytes.fromhex(state.mac.replace(":", ""))

        def flash_id(self):
            return 0x1940C8

    def read_flash(device, offset, size, **kwargs):
        state.reads.append((offset, size))
        data = state.memory[offset]
        if state.short_read and offset == 0x9000:
            data = data[:-1]
        if state.bad_nvs and state.writes and offset == 0x9000:
            data = b"X" + data[1:]
        return data

    def write_flash(device, images, **kwargs):
        state.writes.extend(images)
        if state.write_failure:
            raise ValueError("injected write failure")

    def verify_flash(device, images, **kwargs):
        assert state.writes == images
        if state.verify_failure:
            raise ValueError("injected verify failure")

    module = ModuleType("esptool.cmds")
    module.detect_chip = lambda **kwargs: Device()
    module.run_stub = lambda device: device
    module.attach_flash = lambda device: None
    module.read_flash = read_flash
    module.write_flash = write_flash
    module.verify_flash = verify_flash
    module.erase_region = lambda device, start, size: state.erases.append((start, size))
    module.reset_chip = lambda device, mode: state.resets.append(mode)
    monkeypatch.setitem(sys.modules, "esptool", ModuleType("esptool"))
    monkeypatch.setitem(sys.modules, "esptool.cmds", module)
    return tmp_path, state


def test_dry_run_has_no_dependencies_or_transport(monkeypatch, capsys):
    def refuse(*args, **kwargs):
        pytest.fail("dry-run opened a transport or signing tool")

    monkeypatch.setattr(migration.subprocess, "run", refuse)
    assert migration.main(["--dry-run", "--build", "/nonexistent"]) == 0
    out = capsys.readouterr().out
    for _, _, start, size in migration.PARTITIONS.values():
        assert f"offset=0x{start:07x}" in out
        assert f"size=0x{size:07x}" in out


@pytest.mark.parametrize("value", [migration.DEV_MAC, "orb-020000abcdef", "020000ABCDEF"])
def test_board_identifiers(value):
    assert migration.board_mac(value) == migration.DEV_MAC


@pytest.mark.parametrize("value", ["", "other", "orb-020000abcd", "../../tmp"])
def test_invalid_board_identifiers(value):
    with pytest.raises(ValueError, match="--board"):
        migration.board_mac(value)


def test_table_checksum():
    assert migration.parse_table(table()) == migration.PARTITIONS
    raw = bytearray(table())
    raw[5] ^= 1
    with pytest.raises(ValueError, match="checksum"):
        migration.parse_table(raw)


def test_success_preserves_nvs_phy_and_resets_only_after_receipt(worker):
    directory, state = worker
    migration.remote_migrate("fake-port", directory)
    receipt = json.loads((directory / "backup/verified.json").read_text())
    assert receipt["nvs_identical"] and receipt["phy_identical"]
    assert (directory / "backup/nvs.bin").read_bytes() == state.memory[0x9000]
    assert (directory / "backup/nvs-after.bin").read_bytes() == state.memory[0x9000]
    assert (directory / "backup/phy_init-after.bin").read_bytes() == state.memory[0x11000]
    assert state.resets == ["hard-reset"]
    assert state.erases == [
        (migration.PARTITIONS[name][2], migration.PARTITIONS[name][3])
        for name in ("ota_1", "assets", "storage", "coredump")
    ]
    assert {start for start, _ in state.writes} == {row[0] for row in migration.FILES.values()}
    assert all(start not in {0x9000, 0x11000} for start, _ in state.writes + state.erases)


def test_wrong_board_refused_before_any_flash_access(worker):
    directory, state = worker
    state.mac = "aa:bb:cc:dd:ee:ff"
    with pytest.raises(ValueError, match="refusing board"):
        migration.remote_migrate("fake-port", directory)
    assert not state.reads and not state.writes and not state.erases and not state.resets


def test_explicit_other_board_allowed(worker):
    directory, state = worker
    state.mac = "aa:bb:cc:dd:ee:ff"
    manifest = json.loads((directory / "manifest.json").read_text())
    manifest["board"] = state.mac
    (directory / "manifest.json").write_text(json.dumps(manifest))
    migration.remote_migrate("fake-port", directory)
    assert state.resets


def test_old_nvs_geometry_refused(worker):
    directory, state = worker
    rows = dict(migration.PARTITIONS)
    rows["nvs"] = (1, 2, 0x9000, 0x5000)
    state.memory[0x8000] = table(rows)
    with pytest.raises(ValueError, match="existing nvs"):
        migration.remote_migrate("fake-port", directory)
    assert not state.writes and not state.erases and not state.resets


@pytest.mark.parametrize("fault", ["write_failure", "verify_failure", "bad_nvs", "short_read"])
def test_failure_keeps_bootloader_and_backups(worker, fault):
    directory, state = worker
    setattr(state, fault, True)
    with pytest.raises(ValueError):
        migration.remote_migrate("fake-port", directory)
    assert not state.resets
    assert not (directory / "backup/verified.json").exists()
    if fault == "short_read":
        assert not state.writes and not state.erases
    else:
        assert (directory / "backup/nvs.bin").read_bytes() == state.memory[0x9000]


def test_staged_tamper_refused_before_connect(worker):
    directory, state = worker
    (directory / "app.bin").write_bytes(b"tamper")
    with pytest.raises(ValueError, match="mismatch"):
        migration.remote_migrate("fake-port", directory)
    assert not state.reads and not state.writes
