"""Offline plan and one-lease, identity-checked USB partition migration.

Only --execute opens the transport. The remote worker stays in one esptool
session until the NVS comparison finishes, so the new app cannot change NVS
between the two reads. Backups contain credentials: never print their bytes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import struct
import subprocess
import sys
import tempfile
from datetime import UTC, datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _load_dev_env() -> None:
    """Personal dev settings live outside the repo in ~/.config/orb/dev.env
    (KEY=VALUE lines). Variables already set in the environment win."""
    path = Path(os.environ.get("ORB_DEV_ENV", Path.home() / ".config/orb/dev.env"))
    if path.is_file():
        for line in path.read_text().splitlines():
            key, sep, value = line.partition("=")
            if sep and key.strip().isidentifier() and not line.lstrip().startswith("#"):
                os.environ.setdefault(key.strip(), value.strip())


_load_dev_env()

DEV_MAC = os.environ.get("ORB_DEV_BOARD", "02:00:00:ab:cd:ef")  # Synthetic dry-run default.
FLASH_SIZE = 0x2000000
# Explicit offsets also make a dry run useful before a build exists.
PARTITIONS = {
    "nvs": (1, 2, 0x9000, 0x6000),
    "otadata": (1, 0, 0xF000, 0x2000),
    "phy_init": (1, 1, 0x11000, 0x1000),
    "ota_0": (0, 0x10, 0x20000, 0x600000),
    "ota_1": (0, 0x11, 0x620000, 0x600000),
    "assets": (1, 0x40, 0xC20000, 0x800000),
    "storage": (1, 0x83, 0x1420000, 0xAE0000),
    "coredump": (1, 3, 0x1F00000, 0x100000),
}
FILES = {
    "bootloader": (0, "bootloader/bootloader.bin", 0x8000),
    "partition-table": (0x8000, "partition_table/partition-table.bin", 0x1000),
    "otadata": (0xF000, "ota_data_initial.bin", 0x2000),
    "app": (0x20000, "orb.signed.bin", 0x600000),
    "assets": (0xC20000, "orb_assets-signed.bin", 0x800000),
}


def board_mac(value: str) -> str:
    compact = value.lower().removeprefix("orb-").replace(":", "")
    if not re.fullmatch("[0-9a-f]{12}", compact):
        raise ValueError("--board must be a MAC address or orb-<12 MAC hex digits>")
    return ":".join(compact[i : i + 2] for i in range(0, 12, 2))


def parse_table(raw: bytes) -> dict:
    rows = {}
    for pos in range(0, len(raw) - 31, 32):
        magic, kind, subtype, offset, size, name, flags = struct.unpack_from("<HBBII16sI", raw, pos)
        if magic == 0xEBEB:
            if hashlib.md5(raw[:pos], usedforsecurity=False).digest() != raw[pos + 16 : pos + 32]:
                raise ValueError("partition table checksum mismatch")
            return rows
        if magic != 0x50AA or flags or not size or offset + size > FLASH_SIZE:
            raise ValueError("invalid/encrypted partition table")
        label = name.rstrip(b"\0").decode("ascii")
        if label in rows:
            raise ValueError("duplicate partition label")
        rows[label] = (kind, subtype, offset, size)
    raise ValueError("partition table has no checksum")


def print_plan(board: str) -> None:
    print(f"Expected board: {board} (orb-{board.replace(':', '')})")
    for name, (_, _, offset, size) in PARTITIONS.items():
        action = "PRESERVE" if name in {"nvs", "phy_init"} else "WRITE"
        if name in {"ota_1", "storage", "coredump"}:
            action = "ERASE"
        print(f"{action:8} {name:16} offset=0x{offset:07x} size=0x{size:07x}")
    print("WRITE    bootloader       offset=0x0000000 max=0x0008000")
    print("WRITE    partition-table  offset=0x0008000 size=0x0001000")
    print("Back up NVS, PHY, old table and otadata before any erase/write.")
    print("Verify all writes and byte-identical NVS + PHY before resetting.")


def verify_signature(path: Path, public_key: Path, python: str) -> None:
    data = path.read_bytes()
    if len(data) < 8192 or len(data) % 4096 or data[-4096:-4094] != b"\xe7\x02":
        raise ValueError(f"{path.name}: missing Secure Boot v2 signature block zero")
    # espsecure accepts any valid signature block. Match firmware's block-zero
    # policy by excluding blocks one and two from the verification copy.
    with tempfile.TemporaryDirectory(prefix="orb-verify-") as directory:
        image = Path(directory) / "block-zero.bin"
        image.write_bytes(data[:-4096] + data[-4096:-2880] + b"\xff" * 2880)
        subprocess.run(
            [
                python,
                "-m",
                "espsecure",
                "verify-signature",
                "--version",
                "2",
                "--keyfile",
                str(public_key),
                str(image),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )


def prepare(args, directory: Path) -> dict:
    public_key = Path(args.public_key).resolve()
    if not public_key.is_file():
        raise ValueError("--public-key must name the public key trusted by the dev board")
    manifest = {"board": board_mac(args.board), "files": []}
    for name, (offset, default, limit) in FILES.items():
        path = Path(getattr(args, name.replace("-", "_"), None) or args.build / default)
        data = path.read_bytes()
        if not data or len(data) > limit:
            raise ValueError(f"{name}: size outside reserved flash range")
        if name == "partition-table" and parse_table(data) != PARTITIONS:
            raise ValueError("build partition table is not the wave 2 layout; reconfigure/build")
        if name == "otadata" and data != b"\xff" * 0x2000:
            raise ValueError("initial otadata must be erased so boot starts ota_0")
        if name == "app" and data[0] != 0xE9:
            raise ValueError("app is not an ESP image")
        if name == "assets" and data[:8] != b"ORBASSET":
            raise ValueError("assets lacks the versioned signed envelope")
        if name == "assets":
            subprocess.run(
                [
                    args.signing_python,
                    str(ROOT / "tools/fonts/assets.py"),
                    "inspect",
                    "--signed",
                    str(path),
                ],
                check=True,
                stdout=subprocess.DEVNULL,
            )
        if name in {"app", "assets"}:
            verify_signature(path, public_key, args.signing_python)
        filename = name + ".bin"
        (directory / filename).write_bytes(data)
        manifest["files"].append(
            {
                "name": name,
                "file": filename,
                "offset": offset,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    (directory / "manifest.json").write_text(json.dumps(manifest))
    (directory / "worker.py").write_bytes(Path(__file__).read_bytes())
    return manifest


def remote_migrate(port: str, directory: Path) -> None:
    """Invoked only under orb-transport's exclusive lease on the USB host."""
    from esptool.cmds import (
        attach_flash,
        detect_chip,
        erase_region,
        read_flash,
        reset_chip,
        run_stub,
        verify_flash,
        write_flash,
    )

    os.umask(0o077)
    manifest = json.loads((directory / "manifest.json").read_text())
    images = []
    for item in manifest["files"]:
        expected_offset, _, maximum = FILES[item["name"]]
        data = (directory / item["file"]).read_bytes()
        if (
            item["offset"] != expected_offset
            or len(data) != item["size"]
            or not 0 < len(data) <= maximum
            or hashlib.sha256(data).hexdigest() != item["sha256"]
        ):
            raise ValueError("staged migration file mismatch")
        images.append((expected_offset, data))
    if len(images) != len(FILES) or {x[0] for x in images} != {x[0] for x in FILES.values()}:
        raise ValueError("incomplete migration image set")
    backup = directory / "backup"
    backup.mkdir(mode=0o700)
    with detect_chip(port=port) as device:
        complete = False
        try:
            mac = ":".join(f"{x:02x}" for x in device.read_mac("BASE_MAC"))
            if device.CHIP_NAME != "ESP32-S3" or mac != board_mac(manifest["board"]):
                raise ValueError(f"refusing board {mac}; expected {manifest['board']}")
            device = run_stub(device)
            attach_flash(device)
            if (device.flash_id() >> 16) & 0xFF != 0x19:
                raise ValueError("expected 32 MiB flash")
            table = read_flash(device, 0x8000, 0x1000, no_progress=True)
            old = parse_table(table)
            for name in ("nvs", "otadata", "phy_init"):
                if old.get(name) != PARTITIONS[name]:
                    raise ValueError(f"existing {name} layout differs; refusing migration")
            (backup / "partition-table.bin").write_bytes(table)
            before = {}
            for name in ("nvs", "phy_init", "otadata"):
                _, _, offset, size = PARTITIONS[name]
                data = read_flash(device, offset, size, no_progress=True)
                if len(data) != size:
                    raise ValueError(f"short {name} backup; no writes performed")
                (backup / (name + ".bin")).write_bytes(data)
                before[name] = data
            # Persist credentials before changing the table. Backup paths and
            # hashes may be logged; credential bytes must never reach stdout.
            for path in backup.iterdir():
                with path.open("rb") as source:
                    os.fsync(source.fileno())
            descriptor = os.open(backup, os.O_RDONLY)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
            print(f"Backups saved privately at {backup}", flush=True)
            for name in ("ota_1", "assets", "storage", "coredump"):
                _, _, offset, size = PARTITIONS[name]
                erase_region(device, offset, size)
            write_flash(device, images, flash_size="32MB", no_progress=True)
            verify_flash(device, images, flash_size="32MB")
            for name in ("nvs", "phy_init"):
                _, _, offset, size = PARTITIONS[name]
                after = read_flash(device, offset, size, no_progress=True)
                (backup / (name + "-after.bin")).write_bytes(after)
                if after != before[name]:
                    raise ValueError(f"{name} changed; board left in bootloader; restore backup")
            (backup / "verified.json").write_text(
                json.dumps(
                    {
                        "board": mac,
                        "nvs_identical": True,
                        "phy_identical": True,
                        "nvs_sha256": hashlib.sha256(before["nvs"]).hexdigest(),
                        "files": manifest["files"],
                    },
                    indent=2,
                )
                + "\n"
            )
            complete = True
            print("NVS and PHY byte-identical; images verified; resetting into ota_0.", flush=True)
        finally:
            # On any failure keep ROM/stub active for inspection/recovery.
            if complete:
                reset_chip(device, "hard-reset")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--dry-run", action="store_true", help="print offsets; no SSH/serial/key access"
    )
    mode.add_argument("--execute", action="store_true", help="migrate the explicitly named board")
    parser.add_argument(
        "--board",
        default=os.environ.get("ORB_DEV_BOARD"),
        help="dev MAC or orb device id (or ORB_DEV_BOARD)",
    )
    parser.add_argument("--build", type=Path, default=ROOT / "firmware/build")
    parser.add_argument("--app", type=Path)
    parser.add_argument("--assets", type=Path)
    parser.add_argument("--public-key", help="public PEM matching the installed trust key")
    parser.add_argument(
        "--signing-python",
        default=str(Path.home() / ".espressif/python_env/idf6.1_py3.13_env/bin/python"),
    )
    parser.add_argument("--backup-dir", type=Path, default=ROOT / "crashes/partition-migration")
    args = parser.parse_args(argv)
    os.umask(0o077)
    if args.execute and not args.board:
        parser.error("--execute requires --board or ORB_DEV_BOARD for your development board")
    expected = board_mac(args.board or DEV_MAC)
    print_plan(expected)
    if args.dry_run:
        return 0
    if not args.public_key:
        parser.error("--execute requires --public-key (never reads the Keychain)")
    host = os.environ.get("ESP_REMOTE_HOST", "my-mac.local")
    stamp = datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ")
    remote = f"esp-remote/orb-migrate-{stamp}-{os.getpid()}"
    target = args.backup_dir.resolve() / (stamp + "-" + expected.replace(":", ""))
    target.mkdir(mode=0o700, parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="orb-migrate-") as temporary:
        directory = Path(temporary)
        prepare(args, directory)
        subprocess.run(["ssh", host, "mkdir -m 700 -p " + shlex.quote(remote)], check=True)
        subprocess.run(["rsync", "-a", str(directory) + "/", host + ":" + remote + "/"], check=True)
        try:
            subprocess.run(
                [
                    str(ROOT / "scripts/orb-transport.sh"),
                    "--exec",
                    "~/.venvs/esptool/bin/python",
                    "~/" + remote + "/worker.py",
                    "--remote-worker",
                    "{port}",
                    "~/" + remote,
                ],
                check=True,
            )
        finally:
            # Retrieve backups even when flashing fails; keep remote originals.
            result = subprocess.run(
                [
                    "rsync",
                    "-a",
                    host + ":" + remote + "/backup/",
                    str(target) + "/",
                ],
                check=False,
            )
            print(f"Private local backup: {target}; remote staging retained: {remote}")
            if result.returncode:
                print(
                    "Backup retrieval failed; recover the remote backup before doing anything else.",
                    file=sys.stderr,
                )
        if not (target / "verified.json").is_file():
            raise ValueError(
                "migration has no verification receipt; inspect retained remote backup"
            )
    return 0


if __name__ == "__main__":
    try:
        if sys.argv[1:2] == ["--remote-worker"]:
            remote_migrate(sys.argv[2], Path(sys.argv[3]).expanduser())
        else:
            raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"partition migration: {error}", file=sys.stderr)
        raise SystemExit(1) from None
