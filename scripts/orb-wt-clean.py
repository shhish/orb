#!/usr/bin/env python3
"""Keep the Orb worktree tree small. Dry run unless --apply.

  orb-wt-clean.py report
  orb-wt-clean.py builds [--idle-hours N] [NAME ...] [--apply]
  orb-wt-clean.py retire [--idle-hours N] [NAME ...] [--apply]

builds removes regenerable build folders from idle worktrees. retire removes a
merged, clean, idle worktree and keeps its branch; small ignored files go to
.retired/<name>.tar.gz first. Naming a worktree skips the idle check, which is
how the lead retires a lane right after merging it. A running process always
protects a worktree, and that check repeats right before every deletion.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import time
from dataclasses import dataclass
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[2]
TRUNK_BRANCH = "orb/w2-trunk"
TRUNK = "w2-trunk"
# The live hub runs from integration; trunk holds the shared managed_components.
PROTECTED = {TRUNK, "integration"}
PARITY_KEEP = 3
SKIP_DIRS = {".git", "managed_components", "third_party", "node_modules"}
ARCHIVE_MAX_BYTES = 2 * 1024 * 1024
# More than this many images and manifests in one worktree is a parity frame dump.
ARCHIVE_MAX_FRAMES = 2000
FRAME_SUFFIXES = {".png", ".json"}
ARCHIVE_SKIP_DIRS = {
    "host-venv",
    "venv",
    ".venv",
    "__pycache__",
    "node_modules",
    ".cache",
    ".ruff_cache",
    ".pytest_cache",
}
# Kept whatever their size: device screenshots, panics, flash profiles, UI results.
EVIDENCE_DIRS = {"shots", "crashes", "profiles", "ui-results"}
ARCHIVE_SKIP_SUFFIXES = {".o", ".obj", ".a", ".pyc", ".elf", ".bin", ".d", ".map", ".dylib"}


LIVE_CACHE_SECONDS = 15.0
_live_cache: tuple[float, set[Path], str] | None = None


def git(path: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "--no-optional-locks", "-c", "maintenance.auto=false", "-C", str(path), *args],
        check=check,
        capture_output=True,
        text=True,
    )


def is_build_name(name: str) -> bool:
    return name == "build" or name.startswith("build-")


@dataclass
class Worktree:
    path: Path
    locked: bool

    @property
    def name(self) -> str:
        return self.path.name


def worktrees(root: Path) -> list[Worktree]:
    anchor = root / TRUNK
    found: list[Worktree] = []
    path: Path | None = None
    locked = False
    lines = git(anchor, "worktree", "list", "--porcelain").stdout.splitlines() + [""]
    for line in lines:
        if line.startswith("worktree "):
            path = Path(line.split(" ", 1)[1])
        elif line == "locked" or line.startswith("locked "):
            locked = True
        elif not line:
            if path is not None and path.parent.resolve() == root.resolve() and path.is_dir():
                found.append(Worktree(path, locked))
            path = None
            locked = False
    return sorted(found, key=lambda w: w.name)


def live_paths() -> tuple[set[Path], str]:
    """Open files, working directories and command lines of every other process.

    Scanning the whole Mac takes seconds, so one scan serves deletions for a moment.
    """
    global _live_cache
    if _live_cache and time.monotonic() - _live_cache[0] < LIVE_CACHE_SECONDS:
        return _live_cache[1], _live_cache[2]
    me = str(os.getpid())
    opened: set[Path] = set()
    lsof = subprocess.run(["lsof", "-Fpn"], capture_output=True, text=True)
    pid = ""
    for line in lsof.stdout.splitlines():
        if line.startswith("p"):
            pid = line[1:]
        elif line.startswith("n/") and pid != me:
            opened.add(Path(line[1:].split(" ", 1)[0]))
    ps = subprocess.run(["ps", "-Ao", "pid=,command="], capture_output=True, text=True)
    commands = "\n".join(line for line in ps.stdout.splitlines() if line.split(None, 1)[:1] != [me])
    _live_cache = (time.monotonic(), opened, commands)
    return opened, commands


def is_live(worktree: Worktree) -> bool:
    cwds, commands = live_paths()
    target = worktree.path.resolve()
    if any(cwd == target or target in cwd.parents for cwd in cwds):
        return True
    text = str(worktree.path)
    start = commands.find(text)
    while start >= 0:
        end = start + len(text)
        if end >= len(commands) or not (commands[end].isalnum() or commands[end] in "-_."):
            return True
        start = commands.find(text, end)
    return False


def symlink_targets(items: list[Worktree]) -> set[Path]:
    """Worktrees another worktree's managed_components points into."""
    targets: set[Path] = set()
    for item in items:
        link = item.path / "firmware" / "managed_components"
        if link.is_symlink():
            targets.add(link.resolve())
    return targets


def shared_by_others(worktree: Worktree, targets: set[Path]) -> bool:
    root = worktree.path.resolve()
    return any(root == target or root in target.parents for target in targets)


def newest_mtime(path: Path) -> float:
    """Newest real file. Deleting a build folder freshens its parent directory,
    and Finder writes .DS_Store just from browsing, so neither counts as work."""
    newest = 0.0
    for folder, dirs, files in os.walk(path):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in files:
            if name == ".DS_Store":
                continue
            try:
                newest = max(newest, os.lstat(os.path.join(folder, name)).st_mtime)
            except OSError:
                continue
    return newest


def size_of(path: Path) -> int:
    total = 0
    for folder, dirs, files in os.walk(path):
        for name in [*dirs, *files]:
            try:
                total += os.lstat(os.path.join(folder, name)).st_blocks * 512
            except OSError:
                continue
    return total


def build_dirs(worktree: Worktree) -> list[Path]:
    """Real, untracked build folders. Tracked content and symlinks never count."""
    tracked = git(worktree.path, "ls-files", "-z").stdout.split("\0")
    found: list[Path] = []
    for folder, dirs, _files in os.walk(worktree.path):
        keep = []
        for name in dirs:
            full = Path(folder) / name
            if name in SKIP_DIRS or full.is_symlink():
                continue
            if is_build_name(name):
                prefix = str(full.relative_to(worktree.path)) + "/"
                if not any(entry.startswith(prefix) for entry in tracked):
                    found.append(full)
                    continue
            keep.append(name)
        dirs[:] = keep
    return found


def is_merged(worktree: Worktree) -> bool:
    result = git(worktree.path, "merge-base", "--is-ancestor", "HEAD", TRUNK_BRANCH, check=False)
    return result.returncode == 0


def real_changes(worktree: Worktree) -> list[str]:
    """Uncommitted paths, ignoring tracked build output already deleted from disk.

    Old lane commits tracked sim/build-asan by accident, so those worktrees show
    hundreds of deletions that are not anyone's work.
    """
    changes: list[str] = []
    for entry in git(worktree.path, "status", "--porcelain=v1", "-z", "--no-renames").stdout.split(
        "\0"
    ):
        if not entry:
            continue
        status, path = entry[:2], entry[3:]
        if status == " D" and path.startswith(("sim/build/", "sim/build-")):
            continue
        changes.append(entry)
    return changes


def retire_refusal(worktree: Worktree, targets: set[Path]) -> str | None:
    if worktree.name in PROTECTED:
        return "protected"
    if worktree.locked:
        return "locked"
    if shared_by_others(worktree, targets):
        return "other worktrees symlink into it"
    if not is_merged(worktree):
        return f"not merged into {TRUNK_BRANCH}"
    changes = real_changes(worktree)
    if changes:
        return f"{len(changes)} uncommitted changes"
    return None


def archive_candidates(worktree: Worktree) -> tuple[list[Path], int]:
    """Ignored files worth keeping, and the bytes of everything else that goes."""
    listing = git(
        worktree.path, "ls-files", "-z", "--others", "--ignored", "--exclude-standard"
    ).stdout.split("\0")
    listing = [rel for rel in listing if rel]
    frames = sum(Path(rel).suffix in FRAME_SUFFIXES for rel in listing)
    keep: list[Path] = []
    dropped = 0
    for rel in listing:
        full = worktree.path / rel
        if full.is_symlink() or not full.is_file():
            continue
        parts = rel.split("/")
        folders = parts[:-1]
        size = full.stat().st_size
        if parts[0] in EVIDENCE_DIRS:
            keep.append(full)
        elif (
            any(is_build_name(p) or p in ARCHIVE_SKIP_DIRS or p in SKIP_DIRS for p in folders)
            or full.suffix in ARCHIVE_SKIP_SUFFIXES
            or parts[-1] in {"sdkconfig", "sdkconfig.old", ".DS_Store"}
            or size > ARCHIVE_MAX_BYTES
            or (frames > ARCHIVE_MAX_FRAMES and full.suffix in FRAME_SUFFIXES)
        ):
            dropped += size
        else:
            keep.append(full)
    return keep, dropped


def write_archive(worktree: Worktree, root: Path, files: list[Path]) -> Path:
    folder = root / ".retired"
    folder.mkdir(exist_ok=True)
    target = folder / f"{worktree.name}.tar.gz"
    temporary = target.with_suffix(".tmp")
    with tarfile.open(temporary, "w:gz") as tar:
        for file in files:
            tar.add(file, arcname=str(file.relative_to(worktree.path)))
    os.replace(temporary, target)
    return target


def gb(size: int) -> str:
    return f"{size / 2**30:.2f} GB"


def selected(items: list[Worktree], names: list[str]) -> list[Worktree]:
    if not names:
        return items
    by_name = {item.name: item for item in items}
    missing = [name for name in names if name not in by_name]
    if missing:
        sys.exit(f"not a worktree here: {', '.join(missing)}")
    return [by_name[name] for name in names]


def is_idle(worktree: Worktree, hours: float, named: bool) -> bool:
    return named or time.time() - newest_mtime(worktree.path) >= hours * 3600


def trim_parity(worktree: Worktree, apply: bool) -> int:
    """Protected worktrees keep their builds but not every old parity capture."""
    build = worktree.path / "sim" / "build"
    if not build.is_dir():
        return 0
    captures = [p for p in build.iterdir() if p.name.startswith("parity-") and p.is_dir()]
    captures.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    freed = 0
    for capture in captures[PARITY_KEEP:]:
        size = size_of(capture)
        print(f"  {'remove' if apply else 'would remove'} {capture} ({gb(size)})")
        if apply:
            shutil.rmtree(capture)
        freed += size
    return freed


def cmd_report(root: Path, _args: argparse.Namespace) -> int:
    items = worktrees(root)
    targets = symlink_targets(items)
    total_builds = total_all = 0
    print(f"{'worktree':<28}{'total':>10}{'builds':>10}  state")
    for item in items:
        size = size_of(item.path)
        builds = sum(size_of(p) for p in build_dirs(item))
        refusal = retire_refusal(item, targets)
        print(f"{item.name:<28}{gb(size):>10}{gb(builds):>10}  {refusal or 'can retire'}")
        total_all += size
        total_builds += builds
    print(f"{len(items)} worktrees, {gb(total_all)}, of which {gb(total_builds)} is build output")
    return 0


def cmd_builds(root: Path, args: argparse.Namespace) -> int:
    freed = 0
    for item in selected(worktrees(root), args.names):
        if item.name in PROTECTED:
            freed += trim_parity(item, args.apply)
            continue
        if not is_idle(item, args.idle_hours, bool(args.names)):
            print(f"{item.name}: skipped, active within {args.idle_hours:g} h")
            continue
        for folder in build_dirs(item):
            if is_live(item):
                print(f"{item.name}: skipped, a process is using it")
                break
            size = size_of(folder)
            print(f"  {'remove' if args.apply else 'would remove'} {folder} ({gb(size)})")
            if args.apply:
                shutil.rmtree(folder)
            freed += size
    print(f"{'freed' if args.apply else 'would free'} {gb(freed)}")
    return 0


def cmd_retire(root: Path, args: argparse.Namespace) -> int:
    items = worktrees(root)
    targets = symlink_targets(items)
    freed = refused = 0
    for item in selected(items, args.names):
        refusal = retire_refusal(item, targets)
        if refusal:
            print(f"{item.name}: kept, {refusal}")
            refused += bool(args.names)
            continue
        if not is_idle(item, args.idle_hours, bool(args.names)):
            print(f"{item.name}: kept, active within {args.idle_hours:g} h")
            continue
        if is_live(item):
            print(f"{item.name}: kept, a process is using it")
            refused += bool(args.names)
            continue
        size = size_of(item.path)
        files, dropped = archive_candidates(item)
        print(
            f"  {'retire' if args.apply else 'would retire'} {item.name} "
            f"({gb(size)}, {len(files)} files archived, {gb(dropped)} of other ignored files dropped)"
        )
        freed += size
        if not args.apply:
            continue
        if files:
            write_archive(item, root, files)
        # --force only passes the build deletions real_changes already excused.
        git(item.path, "worktree", "remove", "--force", str(item.path))
    print(f"{'freed' if args.apply else 'would free'} {gb(freed)}")
    return 1 if refused else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("report").set_defaults(run=cmd_report)
    for name, run in (("builds", cmd_builds), ("retire", cmd_retire)):
        command = commands.add_parser(name)
        command.add_argument("names", nargs="*")
        command.add_argument("--idle-hours", type=float, default=24.0)
        command.add_argument("--apply", action="store_true")
        command.set_defaults(run=run)
    args = parser.parse_args(argv)
    return args.run(args.root.resolve(), args)


if __name__ == "__main__":
    sys.exit(main())
