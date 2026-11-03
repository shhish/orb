"""Keep the checked coverage matrix synchronized with the executable catalogue."""

import argparse
import json
import re
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    args = parser.parse_args()
    catalogue = json.loads(subprocess.check_output([str(args.binary), "--list"], text=True))
    ids = [entry["id"] for entry in catalogue]
    rows = re.findall(r"^\| `([^`]+)` \|.*?\| C \| C \| C \| C \|$", args.matrix.read_text(), re.M)
    if len(ids) != len(set(ids)) or len(rows) != len(set(rows)):
        raise SystemExit("duplicate catalogue or matrix IDs")
    missing, extra = sorted(set(ids) - set(rows)), sorted(set(rows) - set(ids))
    if missing or extra:
        raise SystemExit(f"coverage matrix mismatch: missing={missing}; extra={extra}")
    print(f"Coverage matrix: {len(ids)} scenarios x 4 profiles accounted for.")


if __name__ == "__main__":
    main()
