"""Metadata is checked by default; clean installs require offline artifacts."""

import importlib.util
import os
import subprocess
import sys
import tomllib
from pathlib import Path

import pytest

HUB = Path(__file__).resolve().parents[1]


def test_capability_extras_match_local_package_versions():
    config = tomllib.loads((HUB / "pyproject.toml").read_text())
    extras = config["project"]["optional-dependencies"]
    for extra, package in (("agents", "orb_agents"), ("dot", "orb_mcp"), ("mcp", "orb_mcp")):
        metadata = tomllib.loads((HUB / "packages" / package / "pyproject.toml").read_text())["project"]
        assert f"{metadata['name']}=={metadata['version']}" in extras[extra]
    assert (HUB / "capability-lock.txt").is_file()


def test_capability_checker_disables_indexes_and_source_injection(monkeypatch):
    spec = importlib.util.spec_from_file_location("check_capabilities", HUB / "tools" / "check_capabilities.py")
    checker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checker)
    monkeypatch.setenv("PYTHONPATH", "owner-source-path")
    monkeypatch.setenv("PIP_INDEX_URL", "https://index.invalid")
    env = checker.environment()
    assert env["PIP_NO_INDEX"] == "1"
    assert env["PIP_CONFIG_FILE"] == os.devnull
    assert "PYTHONPATH" not in env
    assert "PIP_INDEX_URL" not in env
    assert set(checker.CAPABILITIES) == {"core", "agents", "dot", "mcp", "combined"}


def test_clean_installed_capability_matrix():
    wheelhouse = os.environ.get("ORB_HUB_CAPABILITY_WHEELHOUSE")
    if not wheelhouse:
        pytest.skip("Offline/no network: set ORB_HUB_CAPABILITY_WHEELHOUSE to run clean installed capability matrix")
    if not Path(wheelhouse).is_dir() or not any(Path(wheelhouse).glob("*.whl")):
        pytest.skip("Offline capability wheelhouse is unavailable; no downloads attempted")
    subprocess.run(
        [sys.executable, str(HUB / "tools" / "check_capabilities.py"), "--wheelhouse", wheelhouse],
        check=True,
    )
