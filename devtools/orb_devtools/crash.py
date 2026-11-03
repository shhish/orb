"""Extract and verify ESP-IDF crash artifacts before invoking symbol decoders."""

import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION = (9 << 16) | 0x0104  # ESP32-S3, IDF 6.1 ELF+SHA256 header


def core_identity(raw: bytes) -> tuple[bytes, str]:
    """Validate flash checksum and extract the crashing app's full ELF SHA.

    Deliberately fail closed on older/unknown formats or missing/truncated SHA;
    esp-coredump's own decoder performs an additional validation afterwards.
    """
    if len(raw) < 44:
        raise ValueError("core dump is truncated")
    length, version, _revision = struct.unpack_from("<III", raw)
    if length == 0xFFFFFFFF or length == 0:
        raise ValueError("no core dump in flash")
    if version != VERSION:
        raise ValueError(f"unsupported core format 0x{version:08x}; expected ESP-IDF 6.1 ESP32-S3")
    if not 96 <= length <= len(raw):
        raise ValueError("invalid core dump length")
    raw = raw[:length]
    if hashlib.sha256(raw[:-32]).digest() != raw[-32:]:
        raise ValueError("core dump SHA-256 integrity check failed")
    elf = raw[12:-32]
    if elf[:6] != b"\x7fELF\x01\x01" or len(elf) < 52:
        raise ValueError("expected 32-bit little-endian core ELF")
    kind, machine = struct.unpack_from("<HH", elf, 16)
    if (kind, machine) != (4, 94):
        raise ValueError("expected Xtensa core ELF")
    phoff = struct.unpack_from("<I", elf, 28)[0]
    phsize, count = struct.unpack_from("<HH", elf, 42)
    if phsize != 32 or phoff < 52 or phoff + count * phsize > len(elf):
        raise ValueError("invalid core ELF program headers")
    identities = []
    for i in range(count):
        ptype, offset, _, _, size, _, _, _ = struct.unpack_from(
            "<IIIIIIII", elf, phoff + i * phsize
        )
        if offset + size > len(elf):
            raise ValueError("core ELF segment outside file")
        if ptype != 4:  # PT_NOTE
            continue
        end = offset + size
        while offset < end:
            if offset + 12 > end:
                raise ValueError("truncated core ELF note")
            namesz, descsz, ntype = struct.unpack_from("<III", elf, offset)
            offset += 12
            name_end = offset + namesz
            desc_start = offset + ((namesz + 3) & ~3)
            next_offset = desc_start + ((descsz + 3) & ~3)
            if next_offset > end:
                raise ValueError("core ELF note outside segment")
            name = elf[offset:name_end].rstrip(b"\0")
            desc = elf[desc_start : desc_start + descsz]
            if name == b"ESP_CORE_DUMP_INFO" and ntype == 8266:
                if len(desc) < 68 or struct.unpack_from("<I", desc)[0] != version:
                    raise ValueError("invalid core application identity note")
                identity = desc[4:68].decode("ascii")
                if not re.fullmatch(r"[0-9a-fA-F]{64}", identity):
                    raise ValueError(
                        "core has no full ELF SHA; enable CONFIG_APP_RETRIEVE_LEN_ELF_SHA=64"
                    )
                identities.append(identity.lower())
            offset = next_offset
    if len(identities) != 1:
        raise ValueError("expected exactly one core application identity note")
    return raw, identities[0]


def verify_elf(path: Path, identity: str) -> str:
    if not re.fullmatch(r"[0-9a-fA-F]{64}", identity):
        raise ValueError("a full 64-character application ELF SHA-256 is required")
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != identity.lower():
        raise ValueError(f"ELF SHA mismatch: crash {identity.lower()}, {path.name} {actual}")
    return actual


def coredump_partition(data: bytes) -> tuple[int, int]:
    """Use the device's actual partition table, never assume it was migrated."""
    found = []
    for pos in range(0, len(data) - 31, 32):
        entry = data[pos : pos + 32]
        magic = struct.unpack_from("<H", entry)[0]
        if magic == 0xEBEB:
            if hashlib.md5(data[:pos], usedforsecurity=False).digest() != entry[16:]:
                raise ValueError("partition table MD5 mismatch")
            break
        if magic == 0xFFFF:
            raise ValueError("partition table missing MD5 checksum")
        if magic != 0x50AA:
            raise ValueError("invalid partition table magic")
        _, kind, subtype, offset, size, _label, flags = struct.unpack("<HBBII16sI", entry)
        if kind == 1 and subtype == 3:
            if flags & 1:
                raise ValueError("encrypted core partition requires a separate decryption workflow")
            if not size or offset < 0x9000 or offset + size > 32 * 1024 * 1024:
                raise ValueError("invalid core partition bounds")
            found.append((offset, size))
    else:
        raise ValueError("partition table missing checksum")
    if len(found) != 1:
        raise ValueError("device has no unique coredump partition; deploy the updated table first")
    return found[0]


def backtrace_addresses(text: str) -> list[str]:
    addresses = []
    active = False
    for line in text.splitlines():
        if "Backtrace:" in line:
            line = line.split("Backtrace:", 1)[1]
            active = True
        elif not active or not re.match(r"\s*0x[0-9a-fA-F]+:0x", line):
            active = False
            continue
        for pc in re.findall(r"(0x[0-9a-fA-F]{8}):0x[0-9a-fA-F]{8}", line):
            if pc not in addresses:
                addresses.append(pc)
    if not addresses:
        raise ValueError("no ESP-IDF PC:SP backtrace pairs found")
    return addresses


def log_identity(text: str) -> str:
    values = set(re.findall(r"ELF(?: file)? SHA256:\s*([0-9a-fA-F]{64})(?![0-9a-fA-F])", text))
    if len(values) != 1:
        raise ValueError(
            "log needs one unambiguous full ELF SHA256; use a single incident or --app-sha"
        )
    return values.pop()


def console_text(text: str) -> str:
    """Accept both plain panic logs and orb-logd's structured incident JSONL."""
    lines = []
    for line in text.splitlines():
        try:
            record = json.loads(line)
        except ValueError:
            lines.append(line)
        else:
            if isinstance(record, dict) and isinstance(record.get("text"), str):
                lines.append(record["text"])
            else:
                lines.append(line)
    return "\n".join(lines)


def tool(name: str) -> str:
    located = shutil.which(name)
    if located:
        return located
    if name == "esp-coredump":
        path = Path.home() / ".espressif/python_env/idf6.1_py3.13_env/bin" / name
        if path.is_file():
            return str(path)
    matches = list((Path.home() / ".espressif/tools").glob(f"**/bin/{name}"))
    if len(matches) == 1:
        return str(matches[0])
    raise ValueError(f"cannot choose {name}; source ESP-IDF export.sh or pass the tool path")


def resolve_elf(value: Path | None) -> Path:
    if value is not None:
        if not value.is_file():
            raise ValueError(f"ELF does not exist: {value}")
        return value
    paths = list((ROOT / "firmware/build").glob("*.elf"))
    if len(paths) != 1:
        raise ValueError("expected one firmware/build/*.elf; pass --elf explicitly")
    return paths[0]


def capture(output: Path, transport: Path) -> None:
    result = subprocess.run([str(transport), "--read-core"], check=True, capture_output=True)
    bundle = result.stdout
    if len(bundle) < 16 or bundle[:8] != b"ORBCORE1":
        raise ValueError("invalid core transfer header")
    table_size, raw_size = struct.unpack_from("<II", bundle, 8)
    if table_size != 0x1000 or len(bundle) != 16 + table_size + raw_size:
        raise ValueError("short core transfer")
    table, raw = bundle[16 : 16 + table_size], bundle[16 + table_size :]
    _, size = coredump_partition(table)
    if raw_size != size:
        raise ValueError("core transfer size differs from partition table")
    # Preserve the original read even when decode fails; this may be the only copy.
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(raw)
    output.with_suffix(output.suffix + ".partitions.bin").write_bytes(table)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    for name in ("capture", "decode", "backtrace"):
        command = sub.add_parser(name)
        command.add_argument("--elf", type=Path)
        if name == "capture":
            command.add_argument(
                "--output",
                type=Path,
                default=Path("crashes") / (datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ") + ".raw"),
            )
            command.add_argument(
                "--transport", type=Path, default=ROOT / "scripts/orb-transport.sh"
            )
        else:
            command.add_argument("input", type=Path)
        if name == "backtrace":
            command.add_argument("--app-sha", help="full SHA from the crashed app's build receipt")
            command.add_argument("--addr2line")
        else:
            command.add_argument("--decoder")
            command.add_argument("--gdb")
    args = parser.parse_args(argv)
    try:
        elf = resolve_elf(args.elf)
        if args.operation == "backtrace":
            text = console_text(args.input.read_text(errors="replace"))
            sha = verify_elf(elf, args.app_sha or log_identity(text))
            addresses = backtrace_addresses(text)
            command = [
                args.addr2line or tool("xtensa-esp-elf-addr2line"),
                "-pfiaC",
                "-e",
                str(elf),
                *addresses,
            ]
            artifact = args.input
        else:
            artifact = args.output if args.operation == "capture" else args.input
            if args.operation == "capture":
                if artifact.exists():
                    raise ValueError("capture output already exists; choose a new --output")
                capture(artifact, args.transport)
            raw, identity = core_identity(artifact.read_bytes())
            sha = verify_elf(elf, identity)
            verified = artifact.with_suffix(artifact.suffix + ".verified.raw")
            verified.write_bytes(raw)
            command = [
                args.decoder or tool("esp-coredump"),
                "--chip",
                "esp32s3",
                "info_corefile",
                "-t",
                "raw",
                "-c",
                str(verified),
                "--gdb",
                args.gdb or tool("xtensa-esp-elf-gdb"),
                str(elf),
            ]
        print(f"Verified application ELF SHA-256 {sha}", file=sys.stderr)
        result = subprocess.run(command, check=True, capture_output=True, text=True)
        report = artifact.with_suffix(artifact.suffix + ".txt")
        report.write_text(result.stdout + result.stderr)
        artifact.with_suffix(artifact.suffix + ".json").write_text(
            json.dumps(
                {
                    "elf": str(elf.resolve()),
                    "elf_sha256": sha,
                    "source": str(artifact),
                    "decoder": command[0],
                },
                indent=2,
            )
            + "\n"
        )
        print(result.stdout, end="")
        print(f"Report: {report}", file=sys.stderr)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"orb-crash: {exc}", file=sys.stderr)
        if isinstance(exc, subprocess.CalledProcessError) and exc.stderr:
            detail = (
                exc.stderr.decode(errors="replace") if isinstance(exc.stderr, bytes) else exc.stderr
            )
            print(detail, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
