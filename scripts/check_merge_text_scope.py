#!/usr/bin/env python3
"""Account for strict parity changes using the two polish lanes' exact lists."""

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BRIEFS = ROOT / "docs/agent-briefs/wave2"
SUPPLEMENTAL_TEXT = {
    "home-climate": (
        "Home's newer kit Climate rows inherit the text lane's AX stacking/restacking; "
        "this merge interaction was absent from the lane's original scenario table."
    ),
    "home-climate-offline": (
        "Home's newer kit Climate rows inherit the text lane's AX stacking/restacking; "
        "this merge interaction was absent from the lane's original scenario table."
    ),
    "home-rooms-empty": (
        "Shared state-caption balancing changes 'Add rooms in / the Home app.' to "
        "'Add rooms in the / Home app.' at AX3 and AX3 Bold, balancing the two lines."
    ),
    "home-rooms-offline": (
        "Shared state-caption balancing changes 'Connect your Mac / to load Home.' to "
        "'Connect your Mac to / load Home.' at AX3 and AX3 Bold, balancing the two lines."
    ),
    "home-scenes-empty": (
        "Shared state-caption balancing changes 'Add scenes in / the Home app.' to "
        "'Add scenes in the / Home app.' at AX3 and AX3 Bold, balancing the two lines."
    ),
    "home-sensors-empty": (
        "Shared state-caption widow repair changes 'humidity sensors appear / here.' to "
        "'humidity sensors / appear here.' at AX3 Bold, preserving text, bounds and line count."
    ),
    "weather.loading-long-place": (
        "Shared state-caption balancing redistributes the long place name across the same "
        "two lines in all profiles; semantic text and line count are preserved."
    ),
}


def listed_scenarios(filename: str, heading: str) -> set[str]:
    section = (BRIEFS / filename).read_text().split(f"## {heading}\n", 1)[1]
    section = section.split("\n## ", 1)[0]
    scenarios = set(re.findall(r"^\| `([^`]+)` \|", section, re.MULTILINE))
    if not scenarios:
        raise ValueError(f"{filename}: no exact scenario rows under {heading}")
    return scenarios


def check_scope(report: dict) -> dict:
    text = listed_scenarios("polish-text-results.md", "Exhaustive strict-parity change list")
    games = listed_scenarios("polish-games-results.md", "Exact changed-scenario list")
    changed = set()
    unexpected_files = []
    for difference in report["file_differences"]:
        name = difference["path"]
        if name == "manifest.json":
            continue
        parts = Path(name).parts
        if len(parts) == 3 and Path(name).suffix in {".png", ".json"}:
            changed.add(parts[0])
        else:
            unexpected_files.append(difference)
    # Hash leaves are expected consequences of changed artifacts. All other
    # manifest changes (including compiler/assets metadata) remain failures.
    manifest_changes = [
        difference
        for difference in report["manifest_differences"]
        if not re.fullmatch(
            r'\$\["frames"\]\[\d+\]\["(?:sha256|trace_sha256)"\]', difference["path"]
        )
    ]
    artifact_errors = {side: errors for side, errors in report["artifact_errors"].items() if errors}
    comparison_errors = [
        difference
        for key in ("pixel_differences", "trace_differences")
        for difference in report[key]
        if "error" in difference
    ]
    supplemental = changed & SUPPLEMENTAL_TEXT.keys()
    unlisted = changed - text - games
    unexplained = sorted(unlisted - supplemental)
    return {
        "listed_text_scenarios": len(text),
        "listed_games_scenarios": len(games),
        "changed_scenarios": len(changed),
        "explained_by_text_only": len(changed & (text - games)),
        "explained_by_games_only": len(changed & (games - text)),
        "explained_by_both": len(changed & text & games),
        "supplemental_text_scenarios": len(supplemental),
        "raw_unlisted_scenarios": sorted(unlisted),
        "supplemental_text_attribution": {
            scenario: SUPPLEMENTAL_TEXT[scenario] for scenario in sorted(supplemental)
        },
        "unexplained_scenarios": unexplained,
        "artifact_errors": artifact_errors,
        "comparison_errors": comparison_errors,
        "non_hash_manifest_changes": manifest_changes,
        "unexpected_file_changes": unexpected_files,
        "passed": not (
            unexplained
            or artifact_errors
            or comparison_errors
            or manifest_changes
            or unexpected_files
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="strict parity comparison report.json")
    parser.add_argument("--out", type=Path, help="also save the scope result as JSON")
    args = parser.parse_args()
    result = check_scope(json.loads(args.report.read_text()))
    output = json.dumps(result, indent=2, sort_keys=True) + "\n"
    print(output, end="")
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(output)
    return int(not result["passed"])


if __name__ == "__main__":
    raise SystemExit(main())
