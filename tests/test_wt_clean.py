import importlib.util
import os
import subprocess
import sys
import tarfile
import time
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "orb-wt-clean.py"
spec = importlib.util.spec_from_file_location("orb_wt_clean", SCRIPT)
clean = importlib.util.module_from_spec(spec)
sys.modules["orb_wt_clean"] = clean
spec.loader.exec_module(clean)


def git(path, *args):
    subprocess.run(
        [
            "git",
            "-c",
            "maintenance.auto=false",
            "-c",
            "user.name=t",
            "-c",
            "user.email=t@t",
            "-C",
            str(path),
            *args,
        ],
        check=True,
        capture_output=True,
    )


def age(path, hours=48):
    then = time.time() - hours * 3600
    for folder, dirs, files in os.walk(path):
        for name in [*dirs, *files]:
            os.utime(os.path.join(folder, name), (then, then), follow_symlinks=False)


@pytest.fixture
def tree(tmp_path, monkeypatch):
    monkeypatch.setattr(clean, "live_paths", lambda: (set(), ""))
    trunk = tmp_path / clean.TRUNK
    trunk.mkdir()
    git(trunk, "init", "-q", "-b", clean.TRUNK_BRANCH)
    (trunk / ".gitignore").write_text("build/\nwork/\n")
    (trunk / "a.txt").write_text("a\n")
    (trunk / "sim").mkdir()
    (trunk / "sim" / "keep.txt").write_text("k\n")
    git(trunk, "add", ".")
    git(trunk, "commit", "-qm", "base")
    return tmp_path


def lane(tree, name):
    path = tree / name
    git(tree / clean.TRUNK, "worktree", "add", "-q", "-b", f"orb/{name}", str(path))
    (path / "sim" / "build").mkdir(parents=True)
    (path / "sim" / "build" / "x.o").write_text("object")
    (path / "work" / "run" / "build").mkdir(parents=True)
    (path / "work" / "run" / "build" / "y.o").write_text("object")
    (path / "work" / "run" / "result.log").write_text("evidence")
    age(path)
    return path


def run(tree, *argv):
    return clean.main(["--root", str(tree), *argv])


def test_dry_run_deletes_nothing(tree):
    path = lane(tree, "w2-a")
    run(tree, "builds")
    run(tree, "retire")
    assert (path / "sim" / "build" / "x.o").exists()
    assert not (tree / ".retired").exists()


def test_builds_removes_output_and_keeps_scratch(tree):
    path = lane(tree, "w2-a")
    run(tree, "builds", "--apply")
    assert not (path / "sim" / "build").exists()
    assert not (path / "work" / "run" / "build").exists()
    assert (path / "work" / "run" / "result.log").exists()
    assert (path / "sim" / "keep.txt").exists()


def test_recent_worktree_is_kept_unless_named(tree):
    path = lane(tree, "w2-a")
    (path / "sim" / "build" / "fresh").write_text("now")
    run(tree, "builds", "--apply")
    assert (path / "sim" / "build").exists()
    run(tree, "builds", "w2-a", "--apply")
    assert not (path / "sim" / "build").exists()


def test_finder_metadata_is_not_activity(tree):
    path = lane(tree, "w2-a")
    (path / "sim" / "build" / ".DS_Store").write_text("finder")
    run(tree, "builds", "--apply")
    assert not (path / "sim" / "build").exists()


def test_live_process_protects_even_when_named(tree, monkeypatch):
    path = lane(tree, "w2-a")
    monkeypatch.setattr(clean, "live_paths", lambda: ({path / "sim"}, ""))
    run(tree, "builds", "w2-a", "--apply")
    assert (path / "sim" / "build").exists()
    assert run(tree, "retire", "w2-a", "--apply") == 1
    assert path.exists()


def test_command_line_match_respects_name_boundary(tree, monkeypatch):
    path = lane(tree, "w2-a")
    monkeypatch.setattr(clean, "live_paths", lambda: (set(), f"codex -C {path}-other"))
    assert not clean.is_live(clean.Worktree(path, False))
    monkeypatch.setattr(clean, "live_paths", lambda: (set(), f"codex -C {path}/sim"))
    assert clean.is_live(clean.Worktree(path, False))


def test_symlinks_and_tracked_build_folders_survive(tree):
    path = lane(tree, "w2-a")
    tracked = path / "tools" / "build-data"
    tracked.mkdir(parents=True)
    (tracked / "table.txt").write_text("source")
    git(path, "add", "tools")
    git(path, "commit", "-qm", "tracked")
    target = tree / "outside"
    target.mkdir()
    (target / "f").write_text("f")
    (path / "firmware").mkdir()
    (path / "firmware" / "build").symlink_to(target)
    age(path)
    run(tree, "builds", "--apply")
    assert (tracked / "table.txt").exists()
    assert (target / "f").exists()


def test_protected_worktrees_are_never_retired(tree):
    git(
        tree / clean.TRUNK,
        "worktree",
        "add",
        "-q",
        "-b",
        "orb/integration",
        str(tree / "integration"),
    )
    age(tree / "integration")
    assert run(tree, "retire", "integration", "--apply") == 1
    assert (tree / "integration" / "a.txt").exists()
    assert run(tree, "retire", clean.TRUNK, "--apply") == 1


def test_retire_archives_small_files_and_keeps_branch(tree):
    path = lane(tree, "w2-a")
    (path / "work" / "run" / "big.log").write_bytes(b"x" * (clean.ARCHIVE_MAX_BYTES + 1))
    age(path)
    assert run(tree, "retire", "--apply") == 0
    assert not path.exists()
    subprocess.run(
        ["git", "-C", str(tree / clean.TRUNK), "rev-parse", "--verify", "orb/w2-a"],
        check=True,
        capture_output=True,
    )
    with tarfile.open(tree / ".retired" / "w2-a.tar.gz") as tar:
        names = tar.getnames()
    assert names == ["work/run/result.log"]


def test_archive_skips_frame_dumps(tree, monkeypatch):
    monkeypatch.setattr(clean, "ARCHIVE_MAX_FRAMES", 3)
    path = lane(tree, "w2-a")
    for index in range(4):
        scene = path / "work" / f"scene{index}"
        scene.mkdir()
        (scene / "frame.png").write_text("frame")
    files, dropped = clean.archive_candidates(clean.Worktree(path, False))
    assert [p.name for p in files] == ["result.log"]
    assert dropped > 0


def test_evidence_folders_are_archived_whatever_their_size(tree):
    path = lane(tree, "w2-a")
    (path / ".gitignore").write_text("build/\nwork/\nshots/\n")
    git(path, "add", ".gitignore")
    git(path, "commit", "-qm", "ignore shots")
    (path / "shots").mkdir()
    (path / "shots" / "gate.bin").write_bytes(b"x" * (clean.ARCHIVE_MAX_BYTES + 1))
    files, _dropped = clean.archive_candidates(clean.Worktree(path, False))
    assert "gate.bin" in [p.name for p in files]


def test_real_source_deletion_under_a_build_named_folder_blocks_retire(tree):
    path = lane(tree, "w2-a")
    source = path / "tools" / "build-config"
    source.mkdir(parents=True)
    (source / "board.h").write_text("x")
    git(path, "add", "tools")
    git(path, "commit", "-qm", "source")
    (source / "board.h").unlink()
    assert len(clean.real_changes(clean.Worktree(path, False))) == 1


def test_a_rename_counts_as_one_change(tree):
    path = lane(tree, "w2-a")
    git(path, "mv", "a.txt", "b.txt")
    changes = clean.real_changes(clean.Worktree(path, False))
    assert changes and all(entry[2] == " " for entry in changes)


def test_open_file_makes_a_worktree_live(tree, monkeypatch):
    monkeypatch.undo()
    clean._live_cache = None
    path = lane(tree, "w2-a")
    with open(path / "a.txt"):
        assert not clean.is_live(clean.Worktree(path, False))  # this process does not count
        clean._live_cache = None
        holder = subprocess.Popen(
            ["python3", "-c", f"f=open({str(path / 'a.txt')!r}); import time; time.sleep(30)"],
            cwd="/",
        )
        time.sleep(0.5)
        try:
            assert clean.is_live(clean.Worktree(path, False))
        finally:
            holder.kill()
            holder.wait()
            clean._live_cache = None


def test_unmerged_or_changed_worktrees_are_kept(tree):
    unmerged = lane(tree, "w2-a")
    (unmerged / "new.c").write_text("x")
    git(unmerged, "add", ".")
    git(unmerged, "commit", "-qm", "work")
    dirty = lane(tree, "w2-b")
    (dirty / "a.txt").write_text("edited")
    untracked = lane(tree, "w2-c")
    (untracked / "notes.md").write_text("mine")
    for path in (unmerged, dirty, untracked):
        age(path)
    run(tree, "retire", "--apply")
    assert unmerged.exists() and dirty.exists() and untracked.exists()


def test_deleted_tracked_build_output_does_not_count_as_a_change(tree):
    path = lane(tree, "w2-a")
    git(path, "checkout", "-q", "--detach")
    stale = path / "sim" / "build-asan"
    stale.mkdir()
    (stale / "old.o").write_text("x")
    git(path, "add", "-f", "sim/build-asan")
    git(path, "commit", "-qm", "accidental")
    git(
        tree / clean.TRUNK,
        "merge",
        "-q",
        "--no-edit",
        subprocess.run(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip(),
    )
    (stale / "old.o").unlink()
    wt = clean.Worktree(path, False)
    assert clean.real_changes(wt) == []
    (path / "a.txt").unlink()
    assert len(clean.real_changes(wt)) == 1


def test_worktree_others_symlink_into_is_kept(tree):
    shared = lane(tree, "w2-shared")
    real = shared / "firmware" / "managed_components"
    real.mkdir(parents=True)
    user = lane(tree, "w2-user")
    (user / "firmware").mkdir()
    (user / "firmware" / "managed_components").symlink_to(real)
    age(shared)
    age(user)
    run(tree, "retire", "w2-shared", "--apply")
    assert shared.exists()


def test_parity_trim_keeps_newest_on_trunk(tree):
    build = tree / clean.TRUNK / "sim" / "build"
    build.mkdir()
    for index in range(5):
        capture = build / f"parity-{index}"
        capture.mkdir()
        (capture / "f").write_text("x")
        then = time.time() - (5 - index) * 3600
        os.utime(capture, (then, then))
    (build / "test_app").write_text("binary")
    run(tree, "builds", "--apply")
    assert sorted(p.name for p in build.iterdir()) == [
        "parity-2",
        "parity-3",
        "parity-4",
        "test_app",
    ]
