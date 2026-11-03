"""Repeatable CTest integration: capture actual transition renders twice."""

import argparse
import importlib.util
import sys
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location("orb_parity", Path(__file__).parents[1] / "parity.py")
parity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--filter", default="transition.*")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="orb-parity-repro-") as temporary:
        root = Path(temporary)
        captures = [root / "first", root / "second"]
        for directory in captures:
            status = parity.main(
                [
                    "parity-capture",
                    str(directory),
                    "--binary",
                    str(args.binary),
                    "--filter",
                    args.filter,
                    "--jobs",
                    "4",
                ]
            )
            if status:
                return status
        # Identical empty traces would also compare equal. Require observed
        # shell text so disabled power's revision-zero sentinel cannot hide it.
        for directory in captures:
            manifest = parity.read_json(directory / "manifest.json")
            for frame in manifest["frames"]:
                if frame["frame"] != "source":
                    continue
                trace = parity.read_json(directory / frame["trace"])
                if not any(node.get("text") for node in trace.get("controls", [])):
                    print(f"empty shell semantics: {frame['trace']}", file=sys.stderr)
                    return 1
        first, second = captures
        status = parity.main(
            [
                "parity-compare",
                str(first),
                str(second),
                "--out",
                str(root / "diff"),
            ]
        )
        if status:
            return status
        files = parity._files(first)
        if files != parity._files(second):
            print("parity reproducibility: artifact inventory changed", file=sys.stderr)
            return 1
        for name in sorted(files):
            if (first / name).read_bytes() != (second / name).read_bytes():
                print(f"parity reproducibility: bytes changed in {name}", file=sys.stderr)
                return 1
        print(f"Reproducibility proven: {len(files)} artifact files are byte-identical.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
